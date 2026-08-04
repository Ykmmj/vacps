#pragma once

/**
 * Reverse Promise bridge: JS Promise/thenable → Asio awaitable (event-driven).
 *
 * Public entry is Runtime::await_value. Settlement is driven by native then
 * reactions and a single steady_timer (timeout + cancel notification). Never
 * polls JS_PromiseState. Independent of the binding DSL.
 *
 * Contract: Narrow (entry preconditions)
 * Preconditions:
 *   - Owner-thread only; phase==running; engine open.
 *   - Matching live qjs::OwnedValue (non-empty, same owner context).
 *   - Runtime and its Impl remain alive until the returned awaitable completes.
 * These preconditions are caller obligations and are not checked by the
 * implementation. Misuse has no defined recovery behavior.
 *
 * Operational results (modeled):
 *   - JavaScript rejection, timeout, cancellation (caller stop_token or
 *     runtime-wide shutdown stop), and QuickJS API failures.
 *
 * Cancellation (JsAwaitOptions::stop and Runtime shutdown_stop_token):
 * - Pre-cancel (either stop already requested) returns Error::cancelled without
 *   invoking arbitrary thenable code.
 * - Pending thenables install concrete noexcept stop callbacks for both the
 *   caller token and the runtime-wide shutdown token. A stop request may arrive
 *   on any thread; cancellation is marshaled onto a copied owner executor with
 *   weak_ptr<AwaitState> before mutating AwaitState / timer / JSValues.
 *   Runtime lifetime is a Narrow precondition.
 * - Timeout and cancellation are distinct terminals (timed_out vs cancelled).
 * - No polling. In-flight reverse awaits participate in Asio natural drain:
 *   cancel wakes the waiter; the coroutine frame drops OwnedValues; main_io
 *   returns only after outstanding work is gone.
 */

#include "runtime/error.hpp"
#include "qjs/owned_value.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <stop_token>

namespace vacps {
class Runtime;
}

namespace vacps::runtime {

namespace asio = boost::asio;

/**
 * Options for Runtime::await_value.
 * timeout <= 0 means no deadline (wait until settle or cancel).
 * stop is optional; default-constructed token never stops. Runtime-wide
 * shutdown cancellation is always observed via Impl::shutdown_stop_token().
 */
struct JsAwaitOptions {
  std::chrono::milliseconds timeout{std::chrono::milliseconds{0}};
  std::stop_token stop{};
};

namespace detail {

/**
 * Friend of Runtime and Runtime::Impl. Implements reverse-await attach
 * without exposing engine() on the product API. Operates through Runtime&
 * (stable facade); does not accept or retain shared ownership of the whole
 * Runtime implementation.
 */
struct JsPromiseAwaitAccess {
  [[nodiscard]] static asio::awaitable<Result<vacps::qjs::OwnedValue>> await_value(
      vacps::Runtime& runtime,
      vacps::qjs::OwnedValue value,
      JsAwaitOptions options);
};

}  // namespace detail

}  // namespace vacps::runtime
