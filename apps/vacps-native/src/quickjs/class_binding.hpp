#pragma once

#include "quickjs/object_holder.hpp"
#include "quickjs/raii/value.hpp"

#include <quickjs.h>

#include <memory>
#include <utility>

namespace vacps::js {

/**
 * Minimal reusable helpers for QuickJS class registration when the opaque is
 * ObjectHolder<T> (shared_ptr so async ops + finalizer are safe).
 *
 * Per-T state: one process-global JSClassID (allocated once via JS_NewClassID).
 * Class registration (JS_NewClass) is per JSRuntime.
 *
 * Does not own business logic — only class id, finalizer, opaque get/set, and
 * wrapping a shared_ptr into a JS object of the class.
 */
template <typename T>
struct ClassBinding {
  /** Process-global class id storage (0 until ensure_class_id). */
  [[nodiscard]] static JSClassID& class_id() noexcept {
    static JSClassID id = 0;
    return id;
  }

  /** Allocate JSClassID once (idempotent). */
  static void ensure_class_id() noexcept {
    auto& id = class_id();
    if (id == 0) {
      JS_NewClassID(&id);
    }
  }

  static void finalizer(JSRuntime* /*rt*/, JSValue val) {
    auto* holder =
        static_cast<ObjectHolder<T>*>(JS_GetOpaque(val, class_id()));
    delete holder;
  }

  /**
   * Ensure class id exists and class is registered on this runtime.
   * @param class_name  Static string lifetime (copied into atom by QuickJS).
   * @return 0 on success, -1 on JS_NewClass failure.
   */
  [[nodiscard]] static int ensure_registered(
      JSRuntime* rt,
      const char* class_name) {
    ensure_class_id();
    if (JS_IsRegisteredClass(rt, class_id())) {
      return 0;
    }
    // Function pointers are static; class_name is atomized by QuickJS.
    JSClassDef def{};
    def.class_name = class_name;
    def.finalizer = &finalizer;
    return JS_NewClass(rt, class_id(), &def);
  }

  [[nodiscard]] static int ensure_registered(
      JSContext* ctx,
      const char* class_name) {
    return ensure_registered(JS_GetRuntime(ctx), class_name);
  }

  /** Borrowed opaque; null if wrong class or unset. Does not throw. */
  [[nodiscard]] static ObjectHolder<T>* get_opaque(JSValueConst obj) noexcept {
    return static_cast<ObjectHolder<T>*>(JS_GetOpaque(obj, class_id()));
  }

  /**
   * JS_GetOpaque2: throws TypeError and returns null if not this class.
   * Does not validate that holder->value is non-null.
   */
  [[nodiscard]] static ObjectHolder<T>* get_opaque2(
      JSContext* ctx,
      JSValueConst obj) {
    return static_cast<ObjectHolder<T>*>(
        JS_GetOpaque2(ctx, obj, class_id()));
  }

  /** Shared pointer stored in opaque, or empty if missing/null holder. */
  [[nodiscard]] static std::shared_ptr<T> get_value(
      JSContext* ctx,
      JSValueConst obj) {
    auto* holder = get_opaque2(ctx, obj);
    if (holder == nullptr) {
      return {};
    }
    return holder->value;
  }

  static void set_opaque(JSValue obj, ObjectHolder<T>* holder) noexcept {
    JS_SetOpaque(obj, holder);
  }

  /**
   * Create a class instance holding shared_ptr.
   * On failure returns a JS exception value and deletes any allocated holder.
   * Caller owns the returned JSValue (must free or return to engine).
   */
  [[nodiscard]] static JSValue wrap(
      JSContext* ctx,
      const char* class_name,
      std::shared_ptr<T> value) {
    if (ensure_registered(ctx, class_name) < 0) {
      return JS_ThrowInternalError(ctx, "ClassBinding: JS_NewClass failed");
    }
    auto* holder = new ObjectHolder<T>{std::move(value)};
    Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(class_id()))};
    if (obj.is_exception()) {
      delete holder;
      return obj.release();
    }
    set_opaque(obj.get(), holder);
    return obj.release();
  }
};

}  // namespace vacps::js
