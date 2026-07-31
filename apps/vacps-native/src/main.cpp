#include "app/log.hpp"
#include "app/version.hpp"
#include "fs/sandbox.hpp"
#include "quickjs/host.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace asio = boost::asio;

namespace {

void print_usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "  --version, -V     Print version and exit\n"
      << "  --help, -h        Show help\n"
      << "  --data-dir DIR    Data directory (default: VACPS_DATA_DIR or \"data\")\n"
      << "  --script PATH     Business ESM bundle (required)\n"
      << "  --log-level LVL   log level\n"
      << "\n"
      << "C++: Asio event loop + vacps:* modules.\n"
      << "Agent policy (listen, auth, CP keys, …) is owned by the business script.\n";
}

std::string resolve_default_script() {
  namespace fs = std::filesystem;
  if (const char* v = std::getenv("VACPS_SCRIPT"); v != nullptr && v[0] != '\0') {
    return v;
  }
  for (const char* c : {"script/dist/vacps.mjs", "apps/vacps-native/script/dist/vacps.mjs"}) {
    std::error_code ec;
    if (fs::is_regular_file(c, ec)) return c;
  }
  return {};
}

std::string env_or_empty(const char* key) {
  if (const char* v = std::getenv(key); v != nullptr && v[0] != '\0') {
    return v;
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  vacps::js::EngineOptions engine_opts;
  vacps::js::ModuleEnvOptions module_opts;
  module_opts.data_dir = "data";
  if (auto d = env_or_empty("VACPS_DATA_DIR"); !d.empty()) {
    module_opts.data_dir = std::move(d);
  }
  module_opts.ca_bundle = env_or_empty("VACPS_CA_BUNDLE");
  module_opts.fs_extra_roots = vacps::fs::fs_extra_roots_from_env();

  std::string log_level = "info";
  if (auto l = env_or_empty("VACPS_LOG_LEVEL"); !l.empty()) {
    log_level = std::move(l);
  }
  std::string script_path = resolve_default_script();

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] != nullptr ? argv[i] : "";
    if (arg == "--version" || arg == "-V") {
      std::cout << "vacps-agent-linux-x86_64 " << vacps::version() << '\n';
      return EXIT_SUCCESS;
    }
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    }
    if (arg == "--data-dir" && i + 1 < argc) {
      module_opts.data_dir = argv[++i];
      continue;
    }
    if (arg == "--script" && i + 1 < argc) {
      script_path = argv[++i];
      continue;
    }
    if (arg == "--log-level" && i + 1 < argc) {
      log_level = argv[++i];
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  vacps::log::init(log_level);
  vacps::log::info("vacps-agent {} starting", vacps::version());

  if (script_path.empty()) {
    vacps::log::error("business script required (VACPS_SCRIPT or script/dist/vacps.mjs)");
    vacps::log::flush();
    return EXIT_FAILURE;
  }

  try {
    asio::io_context ioc{1};

    auto host_r = vacps::js::Host::create(ioc, std::move(engine_opts), std::move(module_opts));
    if (!host_r) {
      vacps::log::error("quickjs host failed: {}", host_r.error().message);
      vacps::log::flush();
      return EXIT_FAILURE;
    }
    auto host = std::move(*host_r);

    int bootstrap_ec = 0;
    bool running = false;
    // Tick timer is cancellable so Server graceful_shutdown / process stop
    // does not leave a 15s timer that can fire mid-shutdown.
    auto tick_timer = std::make_shared<asio::steady_timer>(ioc);
    asio::co_spawn(
        ioc,
        [host, script_path, &ioc, &bootstrap_ec, &running, tick_timer]()
            -> asio::awaitable<void> {
          auto init = co_await host->load_and_initialize(script_path);
          if (!init) {
            vacps::log::error("script init failed: {}", init.error().message);
            bootstrap_ec = 1;
            ioc.stop();
            co_return;
          }
          running = true;

          for (;;) {
            tick_timer->expires_after(std::chrono::seconds(15));
            auto [ec] =
                co_await tick_timer->async_wait(asio::as_tuple(asio::use_awaitable));
            if (ec) {
              break;
            }
            if (!host->script_ready()) {
              break;
            }
            auto tick = co_await host->invoke_export("tickControlPlane", 0, nullptr);
            if (!tick) {
              vacps::log::debug("tickControlPlane: {}", tick.error().message);
            }
          }
          co_return;
        },
        asio::detached);

    ioc.run();

    if (bootstrap_ec != 0) {
      vacps::log::flush();
      return EXIT_FAILURE;
    }

    // Fallback path when the process stops without Server::request_stop
    // (e.g. bootstrap tick ended while script still ready). Prefer Server's
    // structured graceful_shutdown when a listener is up.
    if (running && host->script_ready()) {
      ioc.restart();
      asio::co_spawn(
          ioc,
          [host, &ioc, tick_timer]() -> asio::awaitable<void> {
            tick_timer->cancel();
            host->env().processes().shutdown();
            co_await host->wait_async_idle(std::chrono::seconds{5});
            if (auto sh = co_await host->shutdown_script(); !sh) {
              vacps::log::error("script shutdown: {}", sh.error().message);
            }
            co_await host->wait_async_idle(std::chrono::seconds{5});
            host->cancel_host_async();
            if (auto drain = host->drain_jobs(); !drain) {
              vacps::log::debug("post-shutdown job drain: {}", drain.error().message);
            }
            ioc.stop();
            co_return;
          },
          asio::detached);
      ioc.run();
    }
  } catch (const std::exception& e) {
    vacps::log::error("fatal: {}", e.what());
    vacps::log::flush();
    return EXIT_FAILURE;
  }

  vacps::log::info("stopped");
  vacps::log::flush();
  return EXIT_SUCCESS;
}
