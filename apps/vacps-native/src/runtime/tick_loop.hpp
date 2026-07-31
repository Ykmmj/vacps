#pragma once

/**
 * Periodic control-plane tick (temp/n1.md §十七 / §十八 tick_loop).
 *
 * Owns the steady_timer used by ShutdownCoordinator::cancel path.
 * start() co_spawns a loop that invokes script export "tickControlPlane".
 */

#include "quickjs/script_runtime.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <memory>

namespace vacps::runtime {

namespace asio = boost::asio;

class TickLoop {
 public:
  explicit TickLoop(asio::io_context& ioc);

  TickLoop(const TickLoop&) = delete;
  TickLoop& operator=(const TickLoop&) = delete;

  /** Timer shared with ShutdownCoordinator so graceful stop can cancel waits. */
  [[nodiscard]] std::shared_ptr<asio::steady_timer> timer() const noexcept {
    return timer_;
  }

  /**
   * co_spawn a loop: wait(interval) → invoke_export("tickControlPlane").
   * Call after ScriptRuntime is initialized (script_ready).
   * cancel() or timer cancel ends the loop.
   */
  void start(
      std::shared_ptr<vacps::js::ScriptRuntime> rt,
      std::chrono::milliseconds interval);

  void cancel();

 private:
  asio::io_context& ioc_;
  std::shared_ptr<asio::steady_timer> timer_;
};

}  // namespace vacps::runtime
