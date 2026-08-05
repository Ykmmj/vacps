#pragma once

/**
 * Application — thin owner-thread composition root.
 *
 * Owns Runtime, ProcessRuntime, and module-catalog composition state with
 * destruction order (reverse of declaration):
 *   signals/entry → Runtime → ProcessRuntime → ModuleCatalog
 * so ProcessRuntime / budget / catalog outlive JSRuntime teardown.
 * Runtime::Impl owns stable Async/Callbacks/Script capabilities; Application
 * wires required references into the catalog composition.
 * Does not overwrite JS_SetContextOpaque (remains vacps::Runtime*).
 * Application owns its signal_set; owner-thread terminal paths cancel/reset that
 * wait before Runtime::request_stop so the wait cannot block natural main_io drain.
 *
 * Lifecycle: construct(Options) → initialize() → run() → destroy on the
 * owner thread. request_stop() is any-thread safe. The application must not be
 * destroyed concurrently with run() / request_stop().
 *
 * Startup configuration is Application::Options only (CLI-produced). Application
 * does not parse argv or read process environment for C++ knobs.
 *
 * No second io_context, business registries, product tick, or domain
 * executors in this slice. data_dir and ca_bundle are wired into
 * ModuleCatalog composition (vacps:host dataDir(); vacps:http TLS).
 */

#include "host/entry_module.hpp"
#include "runtime/error.hpp"
#include "runtime/runtime.hpp"

#include <boost/asio/signal_set.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace vacps::js {
class ModuleCatalog;
}

namespace vacps::process {
class ProcessRuntime;
}

namespace vacps::host {

namespace asio = boost::asio;

class Application {
 public:
  /**
   * Sole C++ application startup configuration type.
   * Produced by the command-line parser; not read from the environment.
   */
  struct Options {
    Runtime::Options runtime{};
    /** Entry ESM path; required before run() (main resolves defaults when CLI omits --script). */
    std::string script_path;
    /** Host data directory exposed by vacps:host dataDir(). */
    std::string data_dir{"data"};
    /** spdlog level name (canonical: trace|debug|info|warn|error|critical|off). */
    std::string log_level{"info"};
    /**
     * CA bundle path for vacps:http outbound TLS (module composition only;
     * not exposed to JavaScript). Empty → platform default resolution.
     */
    std::string ca_bundle{};
    /** Entry initialize / shutdown wall timeout. */
    std::chrono::milliseconds lifecycle_timeout{std::chrono::seconds{30}};
  };

  explicit Application(Options options);
  ~Application() noexcept;

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  /**
   * Owner-thread: runtime.initialize → ProcessRuntime → module catalog →
   * loader → globals. Partial failure leaves members safe to destroy
   * (catalog still outlives Runtime when present).
   *
   * Contract: Narrow
   * Preconditions: called exactly once before run() on the future owner thread.
   * Errors: expected engine/global installation failures only.
   */
  [[nodiscard]] runtime::VoidResult initialize();

  /**
   * Owner-thread, one-shot after successful initialize.
   * Loads options_.script_path, arms SIGINT/SIGTERM, drives Runtime::run with
   * asynchronous EntryModule startup, and returns nonzero if application
   * lifecycle or the kernel reported failure.
   *
   * Contract: Narrow
   * Preconditions: initialize() succeeded; called exactly once on the owner
   * thread; options.script_path is non-empty.
   */
  int run();

  /**
   * Any thread. Sets sticky application stop, then posts pure application
   * orchestration via Runtime::post_to_owner (no JSValue captures). Falls back to
   * Runtime::request_stop when posting is rejected. Does not
   * unconditionally call Runtime::request_stop after a successful post so an
   * in-run first stop can still run EntryModule::shutdown.
   *
   * Not concurrent with Application destruction (documented owner rule).
   */
  void request_stop() noexcept;

 private:
  [[nodiscard]] runtime::VoidResult start_entry(
      std::string source,
      std::string path) noexcept;

  void arm_signal_wait() noexcept;
  /**
   * Owner-thread terminal stop helper.
   * Cancels/resets Application-owned signals_ (outstanding Asio work), then
   * calls Runtime::request_stop(). First-signal graceful stop may keep the
   * wait armed until this helper runs immediately before the final stop.
   */
  void cancel_signals_and_request_stop() noexcept;
  void on_stop_request() noexcept;
  void begin_graceful_stop() noexcept;
  void fail_lifecycle(const runtime::Error& err) noexcept;

  Options options_;

  // Owner-thread lifecycle state (except stop_requested_ / public any-thread entry).
  bool entry_ready_{false};
  bool stop_seen_{false};
  bool lifecycle_failed_{false};

  /** Sticky application stop; set by request_stop before posting. */
  std::atomic<bool> stop_requested_{false};

  // Destruction order (reverse of declaration):
  //   signals_ → entry_ → runtime_ → process_runtime_ → module_catalog_
  //
  // ModuleCatalog outlives Runtime (loader / JSRuntime opaque).
  // ProcessRuntime + ProcessBudget outlive Runtime/QuickJS teardown so child
  // finalizers can still post dispose onto main_executor and release slots.
  // Catalog holds required Runtime::Async / Runtime::Callbacks /
  // ProcessRuntime references. They remain valid for the Runtime lifetime;
  // after Runtime teardown they must not be used.
  std::unique_ptr<js::ModuleCatalog> module_catalog_;
  std::unique_ptr<process::ProcessRuntime> process_runtime_;
  Runtime runtime_;
  EntryModule entry_;
  std::optional<asio::signal_set> signals_;
};

}  // namespace vacps::host
