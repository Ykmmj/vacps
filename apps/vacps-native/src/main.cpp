#include "app/config.hpp"
#include "quickjs/host.hpp"
#include "app/log.hpp"
#include "app/version.hpp"

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
      << "  --host HOST       Listen host\n"
      << "  --port PORT       Listen port (1..65535)\n"
      << "  --data-dir DIR    Data directory\n"
      << "  --script PATH     Business ESM bundle (required)\n"
      << "  --log-level LVL   log level\n"
      << "\n"
      << "C++: Asio transport + vacps:* capabilities.\n"
      << "JS: vacps:http.Server + all routes. Promise await uses Asio.\n"
      << "Non-loopback --host requires VACPS_ALLOW_REMOTE_BIND=true.\n";
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

}  // namespace

int main(int argc, char** argv) {
  auto cfg = vacps::Config::from_env();
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
    if (arg == "--host" && i + 1 < argc) {
      cfg.listen_host = argv[++i];
      continue;
    }
    if (arg == "--port" && i + 1 < argc) {
      auto port = vacps::parse_port(argv[++i]);
      if (!port) {
        std::cerr << "invalid --port: " << port.error().message << '\n';
        return EXIT_FAILURE;
      }
      cfg.listen_port = *port;
      continue;
    }
    if (arg == "--data-dir" && i + 1 < argc) {
      cfg.data_dir = argv[++i];
      continue;
    }
    if (arg == "--script" && i + 1 < argc) {
      script_path = argv[++i];
      continue;
    }
    if (arg == "--log-level" && i + 1 < argc) {
      cfg.log_level = argv[++i];
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  vacps::apply_remote_bind_policy(cfg);

  vacps::log::init(cfg.log_level);
  vacps::log::info("vacps-agent {} starting", vacps::version());

  if (script_path.empty()) {
    vacps::log::error("business script required (VACPS_SCRIPT or script/dist/vacps.mjs)");
    vacps::log::flush();
    return EXIT_FAILURE;
  }

  try {
    asio::io_context ioc{1};

    auto host_r = vacps::js::Host::create(cfg, ioc);
    if (!host_r) {
      vacps::log::error("quickjs host failed: {}", host_r.error().message);
      vacps::log::flush();
      return EXIT_FAILURE;
    }
    auto host = std::move(*host_r);

    int bootstrap_ec = 0;
    bool running = false;
    asio::co_spawn(
        ioc,
        [host, script_path, &ioc, &bootstrap_ec, &running]() -> asio::awaitable<void> {
          // Business script owns vacps:http.Server (createServer + listen in initialize).
          auto init = co_await host->load_and_initialize(script_path);
          if (!init) {
            vacps::log::error("script init failed: {}", init.error().message);
            bootstrap_ec = 1;
            ioc.stop();
            co_return;
          }
          running = true;

          // Periodic registration / telemetry (JS tickControlPlane).
          auto executor = co_await asio::this_coro::executor;
          asio::steady_timer timer{executor};
          for (;;) {
            timer.expires_after(std::chrono::seconds(15));
            auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
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

    // Server/signal stopped the reactor; drain then script shutdown.
    if (running) {
      ioc.restart();
      ioc.poll();

      ioc.restart();
      asio::co_spawn(
          ioc,
          [host, &ioc]() -> asio::awaitable<void> {
            if (auto sh = co_await host->shutdown_script(); !sh) {
              vacps::log::error("script shutdown: {}", sh.error().message);
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
