#pragma once

/**
 * Heap-allocated callable storage attached to a QuickJS function via
 * JS_NewCFunctionData.
 *
 * The slot lives in a tiny JS object (class vacps.NativeSlot) whose finalizer
 * deletes the C++ slot. The function holds a reference to that object in
 * func_data, so lifetime is tied to the JS function — never a stack lambda
 * address.
 *
 * Callables are std::move_only_function so move-only lambdas are supported.
 * Allocation is exception-safe: the slot stays in unique_ptr until opaque
 * ownership is established (no leak if assigning the callable throws).
 *
 * Finalizer contract (enforceable): clears opaque, then deletes the
 * NativeSlot only. That runs ~move_only_function and any captured state.
 * Captured destructors and any owned deleters MUST be non-throwing,
 * non-blocking, and MUST NOT call QuickJS. The finalizer is noexcept; a
 * throwing destructor would terminate rather than be caught. It does not
 * run JS or host business cleanup beyond deleting the slot.
 *
 * Synchronous callbacks execute directly in the current QuickJS owner-thread
 * turn (no per-callback Runtime gate). Async functions capture the non-owning
 * Runtime::Async* in their callable. In-flight work must not rely on
 * NativeSlot* remaining alive; it retains callable state separately.
 */

#include "binding/error.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace vacps::binding::detail {

struct NativeSlot {
  /** Invoked inside the noexcept QuickJS C callback. */
  std::move_only_function<
      JSValue(JSContext*, JSValueConst, int, JSValueConst*)>
      call;
};

inline std::mutex& native_slot_class_mu() noexcept {
  static std::mutex mu;
  return mu;
}

inline JSClassID& native_slot_class_id() noexcept {
  static JSClassID id = 0;
  return id;
}

inline void native_slot_finalizer(JSRuntime* /*rt*/, JSValue val) noexcept {
  // Raw class id only — no mutex (finalizer must stay noexcept).
  auto& id = native_slot_class_id();
  auto* slot = static_cast<NativeSlot*>(JS_GetOpaque(val, id));
  JS_SetOpaque(val, nullptr);
  // ~NativeSlot / ~move_only_function / captured state is a noexcept chain;
  // a throwing destructor would terminate rather than be catchable here.
  delete slot;
}

inline int ensure_native_slot_class(JSRuntime* rt) {
  JSClassID id = 0;
  {
    std::lock_guard<std::mutex> lock(native_slot_class_mu());
    auto& ref = native_slot_class_id();
    if (ref == 0) {
      JS_NewClassID(&ref);
    }
    id = ref;
  }
  if (JS_IsRegisteredClass(rt, id)) {
    return 0;
  }
  // JS_NewClass is per-runtime; serialize registration attempts.
  std::lock_guard<std::mutex> lock(native_slot_class_mu());
  if (JS_IsRegisteredClass(rt, id)) {
    return 0;
  }
  JSClassDef def{};
  def.class_name = "vacps.NativeSlot";
  def.finalizer = &native_slot_finalizer;
  return JS_NewClass(rt, id, &def);
}

inline int ensure_native_slot_class(JSContext* ctx) {
  return ensure_native_slot_class(JS_GetRuntime(ctx));
}

/**
 * Build JS_NewCFunctionData wrapping `slot`.
 * On success takes ownership of `slot` (released into opaque).
 * On failure destroys `slot` via unique_ptr and returns an exception qjs::OwnedValue
 * (pending JS exception preserved when already set by QuickJS).
 *
 * Ownership out: caller owns the returned function JSValue.
 */
inline qjs::OwnedValue make_cfunction_data(
    JSContext* ctx,
    int length,
    std::unique_ptr<NativeSlot> slot) {
  if (ensure_native_slot_class(ctx) < 0) {
    if (!JS_HasException(ctx)) {
      return qjs::OwnedValue::take(
          ctx, throw_internal(ctx, "NativeSlot class registration failed"));
    }
    return qjs::OwnedValue::exception(ctx);
  }

  JSValue data =
      JS_NewObjectClass(ctx, static_cast<int>(native_slot_class_id()));
  if (JS_IsException(data)) {
    // Pending exception preserved; unique_ptr deletes slot.
    return qjs::OwnedValue::take(ctx, data);
  }

  // Transfer slot ownership to the JS object only after the object exists.
  JS_SetOpaque(data, slot.release());

  JSValueConst data_c = data;
  JSValue func = JS_NewCFunctionData(
      ctx,
      [](JSContext* c,
         JSValueConst this_val,
         int argc,
         JSValueConst* argv,
         int /*magic*/,
         JSValue* func_data) noexcept -> JSValue {
        auto* s = static_cast<NativeSlot*>(
            JS_GetOpaque(func_data[0], native_slot_class_id()));
        return s->call(c, this_val, argc, argv);
      },
      length,
      0,
      1,
      &data_c);

  // NewCFunctionData dups data; drop our reference. On failure, data (and
  // therefore the slot via finalizer) is still freed here.
  JS_FreeValue(ctx, data);

  return qjs::OwnedValue::take(ctx, func);
}

/**
 * Optional: set function.name for nicer stack traces (best-effort).
 * Does not throw to C++; failures are cleared (Result-style side effect).
 */
inline void set_function_name(
    JSContext* ctx,
    JSValueConst func,
    const char* name) noexcept {
  if (name == nullptr || name[0] == '\0') {
    return;
  }
  JSValue n = JS_NewString(ctx, name);
  if (JS_IsException(n)) {
    clear_exception(ctx);
    return;
  }
  // name is non-writable / non-enumerable on real JS functions; best-effort.
  // DefinePropertyValueStr takes ownership of n even on failure.
  if (JS_DefinePropertyValueStr(
          ctx, func, "name", n, JS_PROP_CONFIGURABLE) < 0) {
    clear_exception(ctx);
  }
}

}  // namespace vacps::binding::detail
