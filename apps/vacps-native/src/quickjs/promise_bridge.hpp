#pragma once

/**
 * Unified JS Promise ↔ Asio coroutine bridge.
 *
 * Architecture (vacps-native event loop):
 * - io_context::run() is the only process event loop.
 * - spawn_js_promise is the sole entry for native async → JS Promise.
 * - settle-once + scope-exit notify_progress (never leave await_settled asleep).
 * - Business errors: prefer Result/expected inside work; catch is a fault barrier.
 *
 * QuickJS: JS_NewPromiseCapability + JS_Call(resolve|reject)
 *   (see QuickJS Promise / JS_ExecutePendingJob docs).
 * Asio: co_spawn(executor, awaitable, detached) with try/catch around work
 *   (Boost.Asio C++20 coroutine pattern).
 */

#include "app/error.hpp"
#include "quickjs/convert.hpp"
#include "quickjs/host.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/value.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace vacps::js {

/**
 * Settle-once resolve/reject wrappers. JS_Call exceptions are swallowed after
 * clearing the engine exception so the runtime stays usable; notify still runs
 * via spawn_js_promise scope exit.
 */
class PromiseBridge {
 public:
  PromiseBridge(JSContext* ctx, Value resolve, Value reject) noexcept
      : ctx_(ctx), resolve_(std::move(resolve)), reject_(std::move(reject)) {}

  PromiseBridge(const PromiseBridge&) = delete;
  PromiseBridge& operator=(const PromiseBridge&) = delete;
  PromiseBridge(PromiseBridge&&) noexcept = default;
  PromiseBridge& operator=(PromiseBridge&&) noexcept = default;

  [[nodiscard]] bool settled() const noexcept { return settled_; }
  [[nodiscard]] JSContext* context() const noexcept { return ctx_; }

  void resolve(Value val) {
    if (settled_ || ctx_ == nullptr) return;
    settled_ = true;
    JSValueConst args[1] = {val.get()};
    Value r{ctx_, JS_Call(ctx_, resolve_.get(), JS_UNDEFINED, 1, args)};
    swallow_call_exception(r);
  }

  void resolve_undefined() {
    if (settled_ || ctx_ == nullptr) return;
    settled_ = true;
    JSValueConst args[1] = {JS_UNDEFINED};
    Value r{ctx_, JS_Call(ctx_, resolve_.get(), JS_UNDEFINED, 1, args)};
    swallow_call_exception(r);
  }

  void reject(const Error& e) { reject_message(e.message); }

  /** Reject with a JS Error object (name=Error, message=msg) so .stack / instanceof work. */
  void reject_message(std::string_view msg) {
    if (settled_ || ctx_ == nullptr) return;
    Value err = make_js_error(ctx_, msg);
    if (err.is_exception()) {
      // Fallback to string if Error allocation fails.
      settled_ = true;
      Value m = converter<std::string>::to_js(ctx_, std::string{msg});
      JSValueConst args[1] = {m.get()};
      Value r{ctx_, JS_Call(ctx_, reject_.get(), JS_UNDEFINED, 1, args)};
      swallow_call_exception(r);
      return;
    }
    reject_value(std::move(err));
  }

  void reject_value(Value reason) {
    if (settled_ || ctx_ == nullptr) return;
    settled_ = true;
    JSValueConst args[1] = {reason.get()};
    Value r{ctx_, JS_Call(ctx_, reject_.get(), JS_UNDEFINED, 1, args)};
    swallow_call_exception(r);
  }

 private:
  static void swallow_call_exception(Value& call_result) {
    if (!call_result.is_exception()) return;
    JSContext* ctx = call_result.context();
    if (ctx == nullptr) return;
    // Drop engine exception so subsequent jobs can run.
    Value ex{ctx, JS_GetException(ctx)};
    (void)ex;
  }

  JSContext* ctx_{nullptr};
  Value resolve_;
  Value reject_;
  bool settled_{false};
};

/**
 * Create a JS Promise and run `work` on the host JS executor (io_context).
 *
 * Work signature:
 *   boost::asio::awaitable<void>(JSContext* ctx, PromiseBridge& bridge)
 *
 * work should call bridge.resolve / reject with Result-style control flow.
 * Uncaught C++ exceptions become reject (if not yet settled).
 * Scope exit always calls Host::notify_progress().
 */
template <class Work>
JSValue spawn_js_promise(JSContext* ctx, Host* host, Work work) {
  if (host == nullptr) {
    return JS_ThrowInternalError(ctx, "spawn_js_promise: host not wired");
  }

  JSValue resolving[2];
  JSValue promise = JS_NewPromiseCapability(ctx, resolving);
  if (JS_IsException(promise)) {
    return promise;
  }

  Value resolve{ctx, resolving[0]};
  Value reject{ctx, resolving[1]};
  auto host_sp = host->shared_from_this();

  host->async_op_begin();
  // host_sp first in capture so it outlives resolve/reject Value free on abandon.
  boost::asio::co_spawn(
      host->ioc(),
      [host_sp,
       ctx,
       resolve = std::move(resolve),
       reject = std::move(reject),
       work = std::move(work)]() mutable -> boost::asio::awaitable<void> {
        // Unconditional wake of await_settled — even if settle JS_Call fails.
        // async_op_end pairs with async_op_begin in spawn_js_promise.
        struct NotifyGuard {
          std::shared_ptr<Host> host;
          ~NotifyGuard() {
            if (host) {
              host->async_op_end();
              host->notify_progress();
            }
          }
        } notify_guard{host_sp};

        PromiseBridge bridge{ctx, std::move(resolve), std::move(reject)};
        try {
          co_await work(ctx, bridge);
          if (!bridge.settled()) {
            bridge.reject_message("native async work returned without settling the Promise");
          }
        } catch (const std::exception& ex) {
          if (!bridge.settled()) {
            bridge.reject_message(ex.what());
          }
        } catch (...) {
          if (!bridge.settled()) {
            bridge.reject_message("native async work: unknown error");
          }
        }
        co_return;
      },
      boost::asio::detached);

  return promise;
}

}  // namespace vacps::js
