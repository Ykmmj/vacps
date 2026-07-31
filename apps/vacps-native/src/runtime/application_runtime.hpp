#pragma once

/**
 * Process composition root (temp/n1.md ApplicationRuntime).
 *
 * Owns io_context, ScriptServices, ScriptRuntime, TickLoop, and ShutdownCoordinator.
 * Keeps main thin: BootstrapConfig → options → construct → start(script) → run().
 */

#include "app/error.hpp"
#include "bootstrap/config.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/script_services.hpp"
#include "runtime/shutdown_coordinator.hpp"
#include "runtime/tick_loop.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace vacps::runtime {

namespace asio = boost::asio;

/** Construction-time knobs for the agent process. */
struct ApplicationRuntimeOptions {
  vacps::js::EngineOptions engine{};
  vacps::js::ScriptServicesOptions services{};
  /** Control-plane tick interval (n1: 15s tickControlPlane). */
  std::chrono::milliseconds tick_interval{std::chrono::seconds{15}};

  /**
   * Map BootstrapConfig typed fields + EnvironmentSnapshot into engine +
   * services options. Does not apply log_level (caller inits logging) or
   * script_path (caller resolves default filesystem fallback + CLI override).
   */
  [[nodiscard]] static ApplicationRuntimeOptions from_bootstrap(
      const vacps::bootstrap::BootstrapConfig& cfg);
};

class ApplicationRuntime {
 public:
  explicit ApplicationRuntime(ApplicationRuntimeOptions opts = {});

  ApplicationRuntime(const ApplicationRuntime&) = delete;
  ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;
  ApplicationRuntime(ApplicationRuntime&&) = delete;
  ApplicationRuntime& operator=(ApplicationRuntime&&) = delete;

  /**
   * Create ScriptServices + ScriptRuntime, arm signals, co_spawn
   * load_and_initialize then TickLoop.
   * Sync failures (e.g. QuickJS create) return Error; script init failure is
   * reported via run()'s exit code after ioc.stop().
   */
  [[nodiscard]] VoidResult start(std::string_view script_path);

  /**
   * Drive io_context until stopped. Runs ordered graceful teardown if the loop
   * ended without ShutdownCoordinator. Returns process exit code.
   */
  [[nodiscard]] int run();

  /** Idempotent request for ordered graceful stop. */
  void stop();

  [[nodiscard]] bool stopping() const noexcept { return shutdown_.stopping(); }

 private:
  ApplicationRuntimeOptions opts_;
  asio::io_context ioc_;
  /** Composition-owned process services; also retained by ScriptRuntime. */
  std::shared_ptr<vacps::js::ScriptServices> services_;
  std::shared_ptr<vacps::js::ScriptRuntime> rt_;
  TickLoop tick_;
  ShutdownCoordinator shutdown_;
  int exit_code_{0};
};

}  // namespace vacps::runtime
