#pragma once

/**
 * Process-level graceful shutdown (temp/n1.md §十七).
 *
 * Single owner of SIGINT/SIGTERM and the ordered teardown sequence.
 * HTTP Server must not own signal_set, call ioc.stop(), or run JS shutdown.
 *
 * Sequence (as far as current APIs allow):
 *   1. mark stopping
 *   2. cancel tick timer
 *   3. processes.shutdown
 *   4. wait_async_idle
 *   5. shutdown_script
 *   6. wait_async_idle
 *   7. cancel_host_async / drain_jobs
 *   8. ioc.stop
 *
 * HTTP server accept/session drain, JS value free, and executor stop are
 * not yet centralized (no global server registry; ApplicationRuntime owns this).
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
