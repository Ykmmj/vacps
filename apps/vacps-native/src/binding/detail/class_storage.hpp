#pragma once

/**
 * Per-T QuickJS class id + opaque holder (std::shared_ptr<T>).
 *
 * Identity contract:
 * - One process-wide JSClassID per T (JS_NewClassID once, under a mutex).
 * - One JS class identity per T for the whole process: the first registered
 *   class_name wins. Supplying the same name later is a caller precondition;
 *   it is not dynamically checked.
 * - JS_NewClass itself is per JSRuntime; each runtime may register the same
 *   id once. Multiple host threads/runtimes serialize id init + registration
 *   through class_mu().
 *
 * Finalizer contract (enforceable):
 * - When ClassJsEdges<T>::enabled, the finalizer first runs
 *   ClassJsEdges<T>::release (JS_FreeValueRT edge bookkeeping only) so any
 *   native-owned JSValue / qjs::OwnedValue edges are emptied before the
 *   holder is deleted. Official QuickJS uses JS_FreeValueRT in class
 *   finalizers; that operation is intentional VM bookkeeping.
 * - The finalizer then deletes the heap ClassHolder. That drops one
 *   shared_ptr<T> and destroys any callable state still owned by the holder
 *   graph. ~T, captured-state destructors, and any shared_ptr deleters MUST
 *   be non-throwing, non-blocking, and MUST NOT call QuickJS (including
 *   thrashing other JSContexts) after the release hook has run. The
 *   finalizer is noexcept and uses the already-established raw class id
 *   (no mutex). A throwing destructor or deleter would terminate rather
 *   than be caught here — there is no public Finalize hook and no custom
 *   ClassBuilder destructor.
 * - Long-lived resources need an explicit close/host-ownership path; async
 *   work must retain its own shared_ptr<T> so the object outlives the JS
 *   wrapper. This layer does not run business cleanup, JS_Call, allocation,
 *   blocking work, or async close from the finalizer beyond the optional
 *   ClassJsEdges release hook and deleting the holder.
 *
 * Mutex / exceptions:
 * - ensure_class_id / class_id / ensure_registered lock a std::mutex and are
 *   therefore not noexcept (lock acquisition may throw system_error).
 * - Call them only from C++ registration / wrap paths. A failure escaping a
 *   noexcept QuickJS callback terminates. The finalizer / gc_mark never lock.
 */

#include "binding/error.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace vacps::binding {

/**
 * Optional per-T QuickJS edge bookkeeping for ClassStorage opaque natives.
 *
 * LOUD CONTRACT — VM EDGE BOOKKEEPING ONLY:
 * - Specializations may implement mark + release. Both are noexcept and run
 *   on the QuickJS owner / GC paths.
 * - mark: call JS_MarkValue on every native-owned JSValue edge so cycle GC
 *   can see them. No JS_Call, no allocation, no blocking, no async work.
 * - release: free those edges with JS_FreeValueRT and leave any
 *   qjs::OwnedValue containers empty (e.g. release() then JS_FreeValueRT)
 *   BEFORE ClassHolder deletion. No JS_Call, no allocation, no blocking,
 *   no async close, no business finalization.
 * - After release returns, ~T / shared_ptr deleters still MUST NOT call
 *   QuickJS. Emptying OwnedValue edges in release is what keeps that safe.
 * - Default: enabled = false (no gc_mark installed, finalizer only deletes
 *   the holder). One process-wide class identity per T is unchanged.
 *
 * This is NOT a general finalizer / close hook. Host and JS still own
 * business lifecycle; ClassStorage only keeps the VM's reference graph
 * honest for natives that root JSValues.
 */
template <class T>
struct ClassJsEdges {
  static constexpr bool enabled = false;

  static void mark(
      JSRuntime* /*rt*/,
      const T& /*self*/,
      JS_MarkFunc* /*mark_func*/) noexcept {}

  static void release(JSRuntime* /*rt*/, T& /*self*/) noexcept {}
};

namespace detail {

template <class T>
struct ClassHolder {
  std::shared_ptr<T> ptr;
};

template <class T>
struct ClassStorage {
  static std::mutex& class_mu() {
    static std::mutex mu;
    return mu;
  }

  static JSClassID& class_id_ref() noexcept {
    static JSClassID id = 0;
    return id;
  }

  /** Canonical class name chosen by the first successful registration. */
  static std::string& canonical_name() {
    static std::string name;
    return name;
  }

  /**
   * Ensure a process-wide class id exists, then return it.
   * May throw std::system_error from mutex lock — not for use in finalizers.
   */
  static JSClassID class_id() {
    ensure_class_id();
    return class_id_ref();
  }

  /** Narrow: registration has already initialized the process-wide id. */
  static JSClassID registered_class_id() noexcept { return class_id_ref(); }

  /** May throw std::system_error from mutex lock. */
  static void ensure_class_id() {
    std::lock_guard<std::mutex> lock(class_mu());
    auto& id = class_id_ref();
    if (id == 0) {
      JS_NewClassID(&id);
    }
  }

  /**
   * Finalizer: optional ClassJsEdges release, clear opaque, delete holder.
   * Uses the raw class id (no lock). noexcept: release + ~T / captured state
   * / shared_ptr deleters form a noexcept destruction chain — they MUST NOT
   * throw (a throw terminates).
   */
  static void finalizer(JSRuntime* rt, JSValue val) noexcept {
    auto* holder =
        static_cast<ClassHolder<T>*>(JS_GetOpaque(val, class_id_ref()));
    JS_SetOpaque(val, nullptr);
    if constexpr (ClassJsEdges<T>::enabled) {
      // ONLY VM edge bookkeeping (JS_FreeValueRT). No JS_Call / business close.
      ClassJsEdges<T>::release(rt, *holder->ptr);
    }
    delete holder;
  }

  /**
   * GC mark: report native-owned JSValue edges. Installed only when
   * ClassJsEdges<T>::enabled. No JS_Call / allocation / blocking.
   */
  static void gc_mark(
      JSRuntime* rt,
      JSValueConst val,
      JS_MarkFunc* mark_func) noexcept {
    if constexpr (ClassJsEdges<T>::enabled) {
      auto* holder =
          static_cast<ClassHolder<T>*>(JS_GetOpaque(val, class_id_ref()));
      ClassJsEdges<T>::mark(rt, *holder->ptr, mark_func);
    }
  }

  /**
   * Ensure T is registered on this runtime under `class_name`.
   * @return 0 on success, -1 on QuickJS registration failure.
   * Does not leave a pending JS exception (caller maps to binding::Error).
   * May throw std::system_error from mutex lock.
   */
  static int ensure_registered(JSRuntime* rt, const char* class_name) {
    ensure_class_id();
    const char* requested = class_name;

    std::lock_guard<std::mutex> lock(class_mu());
    auto& name = canonical_name();
    if (name.empty()) {
      name = requested;
    }

    const JSClassID id = class_id_ref();
    if (JS_IsRegisteredClass(rt, id)) {
      return 0;
    }
    JSClassDef def{};
    def.class_name = name.c_str();
    def.finalizer = &finalizer;
    if constexpr (ClassJsEdges<T>::enabled) {
      def.gc_mark = &gc_mark;
    }
    return JS_NewClass(rt, id, &def);
  }

  static int ensure_registered(JSContext* ctx, const char* class_name) {
    return ensure_registered(JS_GetRuntime(ctx), class_name);
  }

  /**
   * JS_GetOpaque2: throws TypeError when class id mismatches.
   * Returns null and leaves a pending exception on mismatch.
   * Registration is a caller precondition; no mutex is taken on JS call entry.
   */
  static ClassHolder<T>* get_opaque2(JSContext* ctx, JSValueConst obj) {
    return static_cast<ClassHolder<T>*>(
        JS_GetOpaque2(ctx, obj, registered_class_id()));
  }

  /**
   * Wrap shared_ptr into a new class instance.
   * Caller owns the returned value. On failure, holder is deleted and
   * JS_EXCEPTION is returned inside qjs::OwnedValue (pending exception preserved).
   */
  static qjs::OwnedValue wrap(
      JSContext* ctx,
      const char* class_name,
      std::shared_ptr<T> value) {
    if (ensure_registered(ctx, class_name) < 0) {
      if (JS_HasException(ctx)) {
        return qjs::OwnedValue::exception(ctx);
      }
      return qjs::OwnedValue::take(
          ctx,
          throw_internal(
              ctx,
              "ClassStorage: class registration failed"));
    }
    auto holder = std::make_unique<ClassHolder<T>>();
    holder->ptr = std::move(value);
    JSValue obj =
        JS_NewObjectClass(ctx, static_cast<int>(registered_class_id()));
    if (JS_IsException(obj)) {
      return qjs::OwnedValue::take(ctx, obj);
    }
    JS_SetOpaque(obj, holder.release());
    return qjs::OwnedValue::take(ctx, obj);
  }

  static qjs::OwnedValue wrap_with_proto(
      JSContext* ctx,
      JSValueConst proto,
      std::shared_ptr<T> value) {
    auto holder = std::make_unique<ClassHolder<T>>();
    holder->ptr = std::move(value);
    JSValue obj =
        JS_NewObjectProtoClass(ctx, proto, registered_class_id());
    if (JS_IsException(obj)) {
      return qjs::OwnedValue::take(ctx, obj);
    }
    JS_SetOpaque(obj, holder.release());
    return qjs::OwnedValue::take(ctx, obj);
  }
};

/**
 * Unwrap this-value as T. On failure returns unexpected and clears any
 * pending TypeError from GetOpaque2 (binding::Result leaves engine clean).
 */
template <class T>
Result<std::pair<T*, std::shared_ptr<T>>> unwrap_this(
    JSContext* ctx,
    JSValueConst this_val) {
  auto* holder = ClassStorage<T>::get_opaque2(ctx, this_val);
  if (holder == nullptr) {
    // GetOpaque2 throws TypeError — convert to binding error and clear.
    clear_exception(ctx);
    return std::unexpected(
        Error::type("invalid this: wrong class or null opaque"));
  }
  return std::pair<T*, std::shared_ptr<T>>{holder->ptr.get(), holder->ptr};
}

}  // namespace detail
}  // namespace vacps::binding
