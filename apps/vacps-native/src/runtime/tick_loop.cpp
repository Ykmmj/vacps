#include "runtime/tick_loop.hpp"

#include "app/log.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <utility>

namespace vacps::runtime {

TickLoop::TickLoop(asio::io_context& ioc)
    : ioc_(ioc), timer_(std::make_shared<asio::steady_timer>(ioc)) {}

void TickLoop::start(
    std::shared_ptr<vacps::js::ScriptRuntime> rt,
    std::chrono::milliseconds interval) {
  if (!rt || !timer_) {
    return;
  }

  asio::co_spawn(
      ioc_,
      [rt = std::move(rt), timer = timer_, interval]() -> asio::awaitable<void> {
        for (;;) {
          timer->expires_after(interval);
          auto [ec] =
              co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
          if (ec) {
            break;
          }
          if (!rt->script_ready()) {
            break;
          }
          auto tick = co_await rt->invoke_export("tickControlPlane", 0, nullptr);
          if (!tick) {
            vacps::log::debug("tickControlPlane: {}", tick.error().message);
          }
        }
        co_return;
      },
      asio::detached);
}

void TickLoop::cancel() {
  if (timer_) {
    timer_->cancel();
  }
}

}  // namespace vacps::runtime
