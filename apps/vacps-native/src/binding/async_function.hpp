#pragma once

/**
 * create_async_function — free function / lambda → JS function returning
 * Promise, backed solely by Runtime::Async (Env::async()).
 *
 * Also used by ModuleBuilder::async_function and
 * ClassBuilder::static_async_function (static path).
 *
 * Accepted callables:
 *   (Args...) -> runtime::Task<T>
 *   (std::stop_token, Args...) -> runtime::Task<T>
 * where runtime::Task<T> = asio::awaitable<runtime::Result<T>> (T may be void).
 *
 * Contract:
 * - Registration requires a non-null Runtime::Async* on Env (Narrow
 *   composition). Missing wiring is host/programmer misuse.
 * - JS args decode synchronously before Runtime::Async::promise; decode
 *   failures throw and create no Promise. No per-callback is_accepting gate.
 * - start / encode failures after Promise creation reject through
 *   Runtime::Async; binding never settles directly.
 * - No JSContext/JSValue/qjs::OwnedValue may be parked across suspension.
 * - The move-only user callable (and optional custom Encode) are stored in
 *   shared_ptr; every in-flight Start captures those shared_ptrs (not
 *   NativeSlot* / slot move_only_function) so pending work survives JS
 *   function finalization and repeated calls.
 * - Repeated calls share one callable instance; mutable callable state must
 *   be reentrant or serialized by the author. Invocation begins on the JS
 *   owner executor.
 * - Default result encode uses Converter<T> on the owner thread. Pass a
 *   custom Encode (runtime::JsEncode) when Converter is insufficient.
 *   Custom Encode is rejected for Task<void> (always resolves undefined).
 * - Custom Encode failures (C++ throw, pending QuickJS exception, bad
 *   OwnedValue) are mapped to runtime::Error; Runtime::Async is the sole settler.
 * - async_function does not automatically run_blocking. Authors may capture the
 *   non-owning Runtime::Async* from env.async() at registration and call
 *   Runtime::Async::run_blocking(stop, pure_cpp_work) inside the coroutine.
 *
 * Ownership: same as create_function — caller-owned qjs::OwnedValue function, or
 * qjs::OwnedValue(JS_EXCEPTION) with exactly one pending exception.
 */

#include "binding/detail/async_invoke.hpp"
#include "binding/detail/async_traits.hpp"
#include "binding/env.hpp"
#include "qjs/owned_value.hpp"

#include <type_traits>
#include <utility>

namespace vacps::binding {

/**
 * Wrap an async callable as a JS function that returns a Promise.
 * Result encoding uses Converter<T> on the owner thread.
 *
 * @param ctx     Non-owning context view (Runtime::Async* is captured)
 * @param name    Optional function.name (may be null)
 * @param fn      Movable callable returning runtime::Task<T>; may be move-only
 * @param length  JS function.length (argv arity; stop_token is not counted).
 *                Pass -1 (the default) to use the decoded C++ arity; any
 *                non-negative value is an explicit override (including 0).
 */
template <class Fn>
[[nodiscard]] qjs::OwnedValue create_async_function(
    Env ctx,
    const char* name,
    Fn fn,
    int length = -1) {
  using F = std::decay_t<Fn>;
  using T = typename detail::async_callable_traits<F>::result_type;
  if constexpr (std::is_void_v<T>) {
    return detail::make_async_free_function(
        ctx,
        name,
        std::move(fn),
        detail::void_encode_unused{},
        length);
  } else {
    return detail::make_async_free_function(
        ctx,
        name,
        std::move(fn),
        detail::async_result_encode<T>{},
        length);
  }
}

/**
 * Same as create_async_function, with a custom owner-thread Encode for the
 * Task result. Encode must satisfy runtime::JsEncode for the (non-void)
 * result type. Task<void> does not accept a custom Encode — it always
 * resolves undefined via Runtime::Async::promise_void.
 *
 * Encode is retained via shared_ptr for the lifetime of the JS function and
 * any in-flight Starts (safe after finalization / across repeated calls).
 * Failures are hardened centrally into runtime::Error (no pending JS
 * exception left for the settler).
 *
 * @param length Same meaning as the Converter overload. Encode is distinguished
 *               from length by not being convertible to int.
 */
template <class Fn, class Encode>
  requires detail::is_async_encode_arg_v<Encode>
[[nodiscard]] qjs::OwnedValue create_async_function(
    Env ctx,
    const char* name,
    Fn fn,
    Encode encode,
    int length = -1) {
  using F = std::decay_t<Fn>;
  using T = typename detail::async_callable_traits<F>::result_type;
  static_assert(
      !std::is_void_v<T>,
      "create_async_function: custom Encode is not supported for Task<void> "
      "(void results always resolve to undefined)");
  return detail::make_async_free_function(
      ctx, name, std::move(fn), std::move(encode), length);
}

}  // namespace vacps::binding
