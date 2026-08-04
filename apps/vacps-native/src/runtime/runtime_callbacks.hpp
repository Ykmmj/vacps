#pragma once

/**
 * Runtime::Callbacks — native event → JS function → await sync/thenable result.
 *
 * Sibling facade to Runtime::Async / Runtime::Script:
 *   Runtime::Async     = JS → native Task → JS Promise
 *   Runtime::Callbacks = native event → JS function → await sync/thenable
 *
 * Synchronous binding callbacks run directly in the owner-thread QuickJS turn
 * and do not use a Runtime capability facade.
 *
 * Directional contract:
 * - Does NOT reverse into Runtime::Async and does NOT own a second Promise
 *   subsystem. Settlement of thenables is delegated exclusively to
 *   detail::JsPromiseAwaitAccess / Runtime::await_value (runtime::JsAwaitOptions,
 *   including stop_token cancellation and timeout).
 * - Does NOT own callback roots. Binding state (e.g. HTTP Server) owns
 *   qjs::OwnedValue callback/self edges and specializes ClassJsEdges for VM
 *   mark/release bookkeeping. Explicit resource close is business shutdown;
 *   ClassJsEdges::release is not a substitute for JS-orchestrated close.
 * - Does NOT expose Impl engine / async_scope on the product API.
 * - Does NOT register JS-handle shutdown cleanup. Correct JS shutdown
 *   (EntryModule shutdown / explicit close) plus Asio natural drain is the
 *   contract; there is no Runtime cleanup-registry fallback for dropping
 *   business callback roots before FreeContext.
 * - call_and_await is a native-to-JS entry whose owner/live/running
 *   preconditions are established by its caller.
 * - Lifetime precondition: owning Runtime/Impl outlives every use.
 *
 * Contract: Narrow (call_and_await entry)
 * Preconditions:
 *   - Owner thread, phase==running, engine open.
 *   - callable is a function in the live owner context.
 *   - Each argument is a non-empty, non-exception-tag OwnedValue for the same
 *     owner context; argument count fits QuickJS (int range).
 * Errors (operational):
 *   - JavaScript throw/reject, cancellation, timeout, QuickJS API failures.
 * Preconditions are not dynamically checked. Violations are programmer
 * misuse, not recoverable Error::invalid_state product paths.
 */

#include "runtime/error.hpp"
#include "runtime/js_promise_await.hpp"
#include "runtime/runtime_fwd.hpp"
#include "qjs/owned_value.hpp"

#include <boost/asio/awaitable.hpp>

#include <vector>

namespace vacps {

class Runtime::Callbacks {
 public:
  /**
   * Non-owning reference to the owning Runtime::Impl. Valid for the full Runtime
   * lifetime under the documented Narrow contract.
   */
  explicit Callbacks(Impl& impl) noexcept;

  Callbacks(const Callbacks&) = delete;
  Callbacks& operator=(const Callbacks&) = delete;
  Callbacks(Callbacks&&) = delete;
  Callbacks& operator=(Callbacks&&) = delete;

  /**
   * Call a borrowed JS callable with owned arguments, then await a sync
   * value or thenable via await_value.
   *
   * Contract: Narrow — see file header.
   * - `callable` is borrowed for the synchronous JS_Call only; it is not
   *   retained across suspension. Caller must keep any long-lived root.
   * - `args` are owned and released before the first coroutine suspension
   *   (and before await_value), so no argument JSValue can outlive FreeContext
   *   via this frame.
   * - `this` is undefined for this overload.
   * - Pending QuickJS exceptions are consumed into runtime::Error exactly once.
   * - Bounded job pump is scheduled after a successful call.
   * - Thenable settlement / timeout / stop cancellation use runtime::JsAwaitOptions
   *   through the existing reverse-await path only.
   */
  [[nodiscard]] runtime::asio::awaitable<runtime::Result<vacps::qjs::OwnedValue>> call_and_await(
      JSValueConst callable,
      std::vector<vacps::qjs::OwnedValue> args,
      runtime::JsAwaitOptions options = {});

 private:
  Impl& impl_;
};

}  // namespace vacps
