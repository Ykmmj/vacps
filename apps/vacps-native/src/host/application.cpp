#include "host/application.hpp"

#include "app/log.hpp"
#include "globals/install.hpp"
#include "modules/catalog.hpp"
#include "process/runtime.hpp"

#include <boost/asio/co_spawn.hpp>

#include <quickjs.h>

#include <csignal>
#include <exception>
#include <fstream>
#include <iterator>
#include <utility>

namespace vacps::host {

namespace {

[[nodiscard]] runtime::Error map_vacps_error(const vacps::Error& err) {
  return runtime::Error::native(err.message);
}

}  // namespace

Application::Application(Options options)
    : options_(std::move(options)),
      runtime_(options_.runtime) {}

Application::~Application() noexcept {
  if (signals_.has_value()) {
    boost::system::error_code ec;
    signals_->cancel(ec);
    signals_.reset();
  }
}

runtime::VoidResult Application::initialize() {
  if (auto r = runtime_.initialize(); !r) {
    return r;
  }

  // ProcessRuntime after runtime.initialize() so main_executor is live.
  // Declared before runtime_ so it outlives QuickJS teardown.
  process_runtime_ = std::make_unique<process::ProcessRuntime>(
      runtime_.main_executor());

  // Catalog must be stored before any later failure so destruction order
  // keeps the loader / runtime composition opaque alive through
  // Runtime/JSRuntime teardown. Product runtime always wires Runtime::Async,
  // Runtime::Callbacks, and ProcessRuntime plus host data_dir / ca_bundle for
  // vacps:host / vacps:http / vacps:process. Capabilities are Impl-owned;
  // references remain valid for the Runtime lifetime.
  module_catalog_ = std::make_unique<js::ModuleCatalog>(
      runtime_.async(),
      runtime_.callbacks(),
      *process_runtime_,
      options_.data_dir,
      options_.ca_bundle);

  JSContext* ctx = runtime_.context();
  JSRuntime* rt = JS_GetRuntime(ctx);

  // Do not call JS_SetContextOpaque — Runtime::Impl owns that slot.
  // install_loader also sets JSRuntime opaque to catalog composition state.
  module_catalog_->install_loader(rt);

  if (auto installed = js::install_global_apis(ctx); !installed) {
    return std::unexpected(map_vacps_error(installed.error()));
  }

  return runtime::success();
}

int Application::run() {
  // Own the path/source before entering Runtime::run so the startup callback
  // and entry coroutine do not depend on Options storage after move into frames.
  std::string owned_path{options_.script_path};
  std::string owned_source;
  {
    std::ifstream in{owned_path, std::ios::in | std::ios::binary};
    if (!in) {
      vacps::log::error("failed to read script '{}'", owned_path);
      return 1;
    }
    owned_source.assign(
        std::istreambuf_iterator<char>{in},
        std::istreambuf_iterator<char>{});
    if (in.bad()) {
      vacps::log::error("failed to read script '{}'", owned_path);
      return 1;
    }
  }

  signals_.emplace(runtime_.main_executor(), SIGINT, SIGTERM);
  arm_signal_wait();

  // Completed pre-run application stop: cancel the armed signal wait and stick
  // the kernel stop so Runtime::run deterministically skips startup. Do not do
  // this after every successful post — in-run first stop must still reach
  // EntryModule::shutdown.
  if (stop_requested_.load(std::memory_order_acquire)) {
    cancel_signals_and_request_stop();
  }

  const int runtime_rc = runtime_.run(
      [this,
       source = std::move(owned_source),
       path = std::move(owned_path)]() mutable noexcept -> runtime::VoidResult {
        return start_entry(std::move(source), std::move(path));
      });

  // Runtime::run returned after natural drain.
  // Drop any remaining signal wait before tearing down EntryModule.
  if (signals_.has_value()) {
    boost::system::error_code ec;
    signals_->cancel(ec);
    signals_.reset();
  }
  entry_.reset();
  entry_ready_ = false;

  if (lifecycle_failed_ || runtime_rc != 0) {
    return 1;
  }
  return 0;
}

void Application::request_stop() noexcept {
  // Sticky application intent first — observed before Runtime::run.
  stop_requested_.store(true, std::memory_order_release);

  // Application outlives every queued callback by contract. A successful
  // post is drained before run() returns; no defensive lifetime token needed.
  auto posted = runtime_.post_to_owner([this]() noexcept {
    on_stop_request();
  });
  if (!posted) {
    runtime_.request_stop();
  }
}

runtime::VoidResult Application::start_entry(
    std::string source,
    std::string path) noexcept {
  asio::co_spawn(
      runtime_.main_executor(),
      [this,
       source = std::move(source),
       path = std::move(path)]() -> asio::awaitable<void> {
        auto started = co_await entry_.load_and_initialize(
            runtime_,
            source,
            path,
            options_.lifecycle_timeout);
        if (!started) {
          fail_lifecycle(started.error());
          cancel_signals_and_request_stop();
          co_return;
        }
        entry_ready_ = true;
        vacps::log::write_info("business script ready");
      },
      [](std::exception_ptr ep) noexcept {
        if (ep != nullptr) {
          std::terminate();
        }
      });
  return runtime::success();
}

void Application::cancel_signals_and_request_stop() noexcept {
  if (signals_.has_value()) {
    boost::system::error_code ec;
    signals_->cancel(ec);
    signals_.reset();
  }
  runtime_.request_stop();
}

void Application::arm_signal_wait() noexcept {
  signals_->async_wait(
      [this](const boost::system::error_code& ec, int /*signo*/) noexcept {
        if (ec) {
          return;
        }
        on_stop_request();
        // First graceful stop may still need a second signal. Terminal paths
        // reset signals_ before returning here.
        if (signals_.has_value()) {
          arm_signal_wait();
        }
      });
}

void Application::on_stop_request() noexcept {
  if (stop_seen_) {
    // Second stop: cancel signal wait and force immediate kernel stop.
    cancel_signals_and_request_stop();
    return;
  }
  stop_seen_ = true;

  if (!entry_ready_) {
    // Startup incomplete (or never started): skip JS shutdown. Do not reset
    // EntryModule here — let the startup coroutine/completion unwind through
    // runtime shutdown; reset on the owner thread after Runtime::run returns.
    cancel_signals_and_request_stop();
    return;
  }

  begin_graceful_stop();
}

void Application::begin_graceful_stop() noexcept {
  asio::co_spawn(
      runtime_.main_executor(),
      [this]() -> asio::awaitable<void> {
        auto stopped = co_await entry_.shutdown(
            runtime_, options_.lifecycle_timeout);
        if (!stopped) {
          fail_lifecycle(stopped.error());
        }
        entry_.reset();
        entry_ready_ = false;
        cancel_signals_and_request_stop();
      },
      [](std::exception_ptr ep) noexcept {
        if (ep != nullptr) {
          std::terminate();
        }
      });
}

void Application::fail_lifecycle(const runtime::Error& err) noexcept {
  lifecycle_failed_ = true;
  vacps::log::error("{}", err.message);
}

}  // namespace vacps::host
