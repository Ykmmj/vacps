#include "runtime/shutdown_coordinator.hpp"

#include "app/log.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <csignal>
#include <utility>

namespace vacps::runtime {

namespace {

/** Max time to wait for native Promise async scope during process shutdown. */
constexpr std::chrono::seconds kGracefulAsyncIdle{5};

}  // namespace

ShutdownCoordinator::ShutdownCoordinator(asio::io_context& ioc)
    : ioc_(ioc), signals_(ioc, SIGINT, SIGTERM) {}

void ShutdownCoordinator::arm_signals(
    std::shared_ptr<vacps::js::ScriptRuntime> rt,
    std::shared_ptr<asio::steady_timer> tick_timer) {
  rt_ = std::move(rt);
  tick_timer_ = std::move(tick_timer);

  signals_.async_wait(
      [this](const boost::system::error_code& ec, int signo) {
        if (ec) {
          return;
        }
        vacps::log::info("signal {}, graceful shutdown", signo);
        if (rt_ && tick_timer_) {
          request_stop(rt_, tick_timer_);
        } else {
          // Very early signal before targets were bound.
          stopping_.store(true, std::memory_order_release);
          ioc_.stop();
        }
      });
}

void ShutdownCoordinator::request_stop(
    std::shared_ptr<vacps::js::ScriptRuntime> rt,
    std::shared_ptr<asio::steady_timer> tick_timer) {
  if (rt) {
    rt_ = rt;
  }
  if (tick_timer) {
    tick_timer_ = tick_timer;
  }

  if (stopping_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  if (!rt_ || !tick_timer_) {
    vacps::log::warn("request_stop without runtime/timer; stopping io_context");
    ioc_.stop();
    return;
  }

  asio::co_spawn(
      ioc_,
      [this, rt = rt_, tick = tick_timer_]() -> asio::awaitable<void> {
        co_await run_graceful_(std::move(rt), std::move(tick));
      },
      asio::detached);
}

bool ShutdownCoordinator::stopping() const noexcept {
  return stopping_.load(std::memory_order_acquire);
}

asio::awaitable<void> ShutdownCoordinator::run_graceful_(
    std::shared_ptr<vacps::js::ScriptRuntime> rt,
    std::shared_ptr<asio::steady_timer> tick_timer) {
  // Host-only teardown (docs/NATIVE_RESOURCE_OWNERSHIP.md §九).
  // Do NOT stopAll JS-created Servers/Processes here — JS owns them.

  // 1. stopping_ already marked in request_stop. Soft-mark ScriptRuntime so
  //    ScriptRequestHandler rejects new work (503) without cancelling waiters.
  if (!rt->closed()) {
    rt->mark_stopping();
  }

  // 2. stop host tick (no more periodic JS entry)
  tick_timer->cancel();

  auto& services = rt->services();

  // 3–4. JS shutdown() — business closes Server/Process/Store/… in its order.
  // Do NOT cancel_host_async before this: await_settled must still complete
  // native ops used by await server.close() / store.close().
  if (!rt->closed()) {
    if (auto sh = co_await rt->shutdown_script(); !sh) {
      vacps::log::error("script shutdown: {}", sh.error().message);
    }
    co_await rt->wait_async_idle(
        std::chrono::duration_cast<std::chrono::milliseconds>(kGracefulAsyncIdle));
  }

  // 5. drain QuickJS jobs (after cancel host async waiters)
  if (!rt->closed()) {
    rt->cancel_host_async();
    if (auto drain = rt->drain_jobs(); !drain) {
      vacps::log::debug("post-shutdown job drain: {}", drain.error().message);
    }
  }

  // 6–8. FreeContext / FreeRuntime — finalizers drop holders; C++ dtors
  // force-clean any OS resources not closed by JS (no Host business stopAll).
  rt->close();

  // 9. executors after FreeContext (Process children cleaned by ~Process)
  services.stop_executors();

  // 10. stop io_context
  vacps::log::info("graceful shutdown complete; stopping io_context");
  ioc_.stop();
  co_return;
}

}  // namespace vacps::runtime
