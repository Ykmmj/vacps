#pragma once

/**
 * Process-level graceful shutdown (see docs/NATIVE_RESOURCE_OWNERSHIP.md).
 *
 * Single owner of SIGINT/SIGTERM and the ordered teardown sequence.
 * HTTP Server / Process domain objects are JS-owned — Host does not enumerate
 * them or call stopAll before JS shutdown().
 *
 * Sequence:
 *   1. mark Host stopping (ScriptRuntime::mark_stopping → 503 / reject new work)
 *   2. cancel tick timer (stop periodic JS entry)
 *   3. JS shutdown() export (business closes Server/Process/Store/…)
 *   4. wait promises / async idle from that shutdown
 *   5. cancel_host_async + drain QuickJS jobs
 *   6. ScriptRuntime::close() (free JS values, FreeContext, FreeRuntime;
 *      finalizers release holders → C++ dtors force-clean OS resources)
 *   7. stop pool/executors (+ process backend teardown as infrastructure)
 *   8. ioc.stop()
 *
 * On JS shutdown failure/timeout: log, still FreeContext (no second Host
 * business-resource stopAll). See ownership doc §十.
 *
 * All exit paths (signal, ApplicationRuntime::stop, init failure after runtime
 * create) should call request_stop — not raw ioc.stop() alone.
 */

#include "quickjs/script_runtime.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <memory>

namespace vacps::runtime {

namespace asio = boost::asio;

class ShutdownCoordinator {
 public:
  explicit ShutdownCoordinator(asio::io_context& ioc);

  ShutdownCoordinator(const ShutdownCoordinator&) = delete;
  ShutdownCoordinator& operator=(const ShutdownCoordinator&) = delete;

  /**
   * Register SIGINT/SIGTERM. On signal, runs the ordered shutdown via
   * request_stop with the runtime/timer retained here.
   *
   * Call after ScriptRuntime and the tick timer exist; retains shared_ptrs
   * for the signal path (does not start shutdown).
   */
  void arm_signals(
      std::shared_ptr<vacps::js::ScriptRuntime> rt,
      std::shared_ptr<asio::steady_timer> tick_timer);

  /**
   * Idempotent start of the graceful sequence. Also updates the targets
   * retained for a subsequent signal-driven request_stop.
   */
  void request_stop(
      std::shared_ptr<vacps::js::ScriptRuntime> rt,
      std::shared_ptr<asio::steady_timer> tick_timer);

  [[nodiscard]] bool stopping() const noexcept;

 private:
  asio::awaitable<void> run_graceful_(
      std::shared_ptr<vacps::js::ScriptRuntime> rt,
      std::shared_ptr<asio::steady_timer> tick_timer);

  asio::io_context& ioc_;
  asio::signal_set signals_;
  std::atomic<bool> stopping_{false};
  std::shared_ptr<vacps::js::ScriptRuntime> rt_;
  std::shared_ptr<asio::steady_timer> tick_timer_;
};

}  // namespace vacps::runtime
