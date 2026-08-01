#include "runtime/application_runtime.hpp"

#include "app/log.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <cstdlib>
#include <string>
#include <utility>

namespace vacps::runtime {

ApplicationRuntimeOptions ApplicationRuntimeOptions::from_bootstrap(
    const vacps::bootstrap::BootstrapConfig& cfg) {
  ApplicationRuntimeOptions opts;
  opts.services.data_dir = cfg.data_dir;
  opts.services.ca_bundle = cfg.ca_bundle;
  opts.services.environment = cfg.environment;
  opts.engine.heap_limit_bytes = cfg.js_heap_limit_bytes;
  opts.engine.stack_limit_bytes = cfg.js_stack_limit_bytes;
  opts.engine.js_time_budget = cfg.js_time_budget;
  return opts;
}

ApplicationRuntime::ApplicationRuntime(ApplicationRuntimeOptions opts)
    : opts_(std::move(opts)),
      ioc_(1),
      tick_(ioc_),
      shutdown_(ioc_) {}

VoidResult ApplicationRuntime::start(std::string_view script_path) {
  services_ = vacps::js::ScriptServices::create(ioc_, std::move(opts_.services));
  auto host_r = vacps::js::ScriptRuntime::create(
      ioc_, std::move(opts_.engine), services_);
  if (!host_r) {
    return std::unexpected(host_r.error());
  }
  rt_ = std::move(*host_r);

  // Composition root installs vacps:* + globals (not ScriptRuntime::create)
  // so quickjs_core stays free of bindings / domain-module link edges.
  if (auto mods = vacps::js::install_default_modules(*rt_); !mods) {
    return std::unexpected(std::move(mods.error()));
  }

  // Signals + tick timer before bootstrap so early SIGINT/SIGTERM still
  // reach ordered teardown once targets are bound.
  shutdown_.arm_signals(rt_, tick_.timer());

  // Read script on the caller thread; ScriptRuntime only compiles/evals source.
  auto source = vacps::js::read_script_file(script_path);
  if (!source) {
    return std::unexpected(std::move(source.error()));
  }
  const std::string path{script_path};
  const std::string src = std::move(*source);
  asio::co_spawn(
      ioc_,
      [this, path, src]() -> asio::awaitable<void> {
        auto init = co_await rt_->initialize_from_source(src, path);
        if (!init) {
          vacps::log::error("script init failed: {}", init.error().message);
          exit_code_ = 1;
          // Same ordered teardown as signal/stop — not raw ioc.stop() alone.
          shutdown_.request_stop(rt_, tick_.timer());
          co_return;
        }
        if (shutdown_.stopping()) {
          // Signal arrived during init; request_stop already scheduled.
          co_return;
        }
        tick_.start(rt_, opts_.tick_interval);
        co_return;
      },
      asio::detached);

  return success();
}

int ApplicationRuntime::run() {
  ioc_.run();

  // Event loop ended without the signal/init-fail path (e.g. tick loop exited
  // while the script is still ready) — run the same ordered teardown once.
  if (rt_ && !rt_->closed() && !shutdown_.stopping()) {
    ioc_.restart();
    shutdown_.request_stop(rt_, tick_.timer());
    ioc_.run();
  }

  vacps::log::info("stopped");
  vacps::log::flush();
  return exit_code_ != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

void ApplicationRuntime::stop() {
  if (rt_) {
    shutdown_.request_stop(rt_, tick_.timer());
  } else {
    tick_.cancel();
    ioc_.stop();
  }
}

}  // namespace vacps::runtime
