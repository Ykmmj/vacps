#pragma once

/**
 * create_function — register free functions or lambdas as QuickJS C functions
 * with automatic argument decode and return encode.
 *
 * Storage: callables are heap-allocated in a NativeSlot held by a JS object
 * referenced from JS_NewCFunctionData func_data. The slot uses
 * std::move_only_function (move-only lambdas OK). Slot construction is
 * exception-safe via unique_ptr until QuickJS owns the opaque.
 *
 * vacps::Runtime::Async* from the creating Env is captured by the callable so
 * callbacks rebuild Env{jc, async}. Synchronous callbacks execute directly in
 * the current owner-thread QuickJS turn (no per-callback Runtime gate).
 * create_async_function keeps its own Runtime::Async contract.
 *
 * All QuickJS entry points are noexcept boundaries. Expected failures use
 * binding::Result / JS exceptions; unexpected C++ throws terminate.
 *
 * Ownership:
 * - create_function returns caller-owned qjs::OwnedValue (JS function). On
 *   failure returns qjs::OwnedValue(JS_EXCEPTION) with exactly one pending
 *   exception.
 * - Callers that install the function (ModuleBuilder / ClassBuilder /
 *   JS_SetPropertyStr) transfer ownership into the engine; on SetProperty
 *   failure QuickJS frees the value — do not double-free.
 */

#include "binding/callback_info.hpp"
#include "binding/detail/invoke.hpp"
#include "binding/detail/native_slot.hpp"
#include "binding/env.hpp"
#include "binding/error.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <memory>
#include <utility>

namespace vacps::binding {

/**
 * Wrap `fn` as a JS function value.
 *
 * @param ctx     Non-owning context view (async capability pointer captured)
 * @param name    Optional function.name (may be null)
 * @param fn      Movable callable (may be move-only); invoked on the JS thread
 * @param length  JS function.length
 *
 * @return Caller-owned function object, or qjs::OwnedValue holding JS_EXCEPTION.
 */
template <class Fn>
[[nodiscard]] qjs::OwnedValue create_function(
    Env ctx,
    const char* name,
    Fn fn,
    int length = 0) {
  JSContext* c = ctx.context();
  vacps::Runtime::Async* async = ctx.async();

  auto slot = std::make_unique<detail::NativeSlot>();
  // The unique_ptr owns the slot until QuickJS opaque ownership is installed.
  slot->call =
      [fn = std::move(fn), async](
          JSContext* jc,
          JSValueConst this_val,
          int argc,
          JSValueConst* argv) mutable -> JSValue {
        // Direct decode/invoke/encode in the current QuickJS owner-thread
        // turn. Runtime entry already established owner/context invariants.
        Env env{jc, async};
        CallbackInfo info{env, this_val, argc, argv};
        qjs::OwnedValue result = detail::dispatch_free(info, fn);
        return result.release();
      };

  qjs::OwnedValue func =
      detail::make_cfunction_data(c, length, std::move(slot));
  if (!func.is_exception() && name != nullptr) {
    detail::set_function_name(c, func.get(), name);
  }
  return func;
}

}  // namespace vacps::binding
