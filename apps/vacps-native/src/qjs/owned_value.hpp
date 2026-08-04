#pragma once

/**
 * Canonical move-only owner for one JSValue associated with a JSContext.
 *
 * Neutral QuickJS primitive layer (vacps::qjs). Independent of Runtime,
 * Binding, app Error, and conversion policy. Runtime and Binding both use
 * this type directly. There is no binding alias or second owner —
 * qjs::OwnedValue is the sole JSValue owner in the process.
 *
 * Ownership rules:
 * - Constructor (ctx, v) / take() adopt ownership of v (must free or release).
 * - release() transfers ownership to the caller (e.g. return to QuickJS).
 * - Destructor JS_FreeValue on the creating context when still owned.
 * - No copy; use duplicate() for an explicit JS_DupValue.
 * - JS_UNDEFINED / JS_NULL / JS_EXCEPTION tags may be stored; empty() means
 *   no context (nothing to free). JS_EXCEPTION still has a context so that
 *   release() can return it to the engine without double-free.
 * - Instance is_* predicates are false when empty(), so empty and an owned
 *   JS_UNDEFINED sentinel (non-null ctx_) are unambiguous.
 * - Context and owner-thread validity are Narrow caller preconditions. This
 *   primitive does not duplicate those checks in each ownership operation.
 */

#include <quickjs.h>

#include <utility>

namespace vacps::qjs {

// ── Predicates on borrowed JSValueConst (no ownership) ────────────

[[nodiscard]] inline bool is_exception(JSValueConst v) noexcept {
  return JS_IsException(v);
}
[[nodiscard]] inline bool is_undefined(JSValueConst v) noexcept {
  return JS_IsUndefined(v);
}
[[nodiscard]] inline bool is_null(JSValueConst v) noexcept {
  return JS_IsNull(v);
}
[[nodiscard]] inline bool is_nullish(JSValueConst v) noexcept {
  return JS_IsUndefined(v) || JS_IsNull(v);
}
[[nodiscard]] inline bool is_object(JSValueConst v) noexcept {
  return JS_IsObject(v);
}
[[nodiscard]] inline bool is_bool(JSValueConst v) noexcept {
  return JS_IsBool(v);
}
[[nodiscard]] inline bool is_number(JSValueConst v) noexcept {
  return JS_IsNumber(v);
}
[[nodiscard]] inline bool is_string(JSValueConst v) noexcept {
  return JS_IsString(v);
}
[[nodiscard]] inline bool is_array(JSContext* ctx, JSValueConst v) noexcept {
  return JS_IsArray(ctx, v);
}
[[nodiscard]] inline bool is_function(JSContext* ctx, JSValueConst v) noexcept {
  return JS_IsFunction(ctx, v);
}
[[nodiscard]] inline bool is_bigint(JSContext* ctx, JSValueConst v) noexcept {
  return JS_IsBigInt(ctx, v);
}

class OwnedValue {
 public:
  OwnedValue() noexcept = default;

  /**
   * Take ownership of `v`.
   * @param ctx  Non-null when `v` must eventually be freed / returned.
   *             May be non-null with JS_UNDEFINED (tagged primitives are not
   *             refcounted but context stamps the owner thread convention).
   */
  OwnedValue(JSContext* ctx, JSValue v) noexcept : ctx_(ctx), v_(v) {}

  OwnedValue(const OwnedValue&) = delete;
  OwnedValue& operator=(const OwnedValue&) = delete;

  OwnedValue(OwnedValue&& other) noexcept
      : ctx_(std::exchange(other.ctx_, nullptr)), v_(other.v_) {
    other.v_ = JS_UNDEFINED;
  }

  OwnedValue& operator=(OwnedValue&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = std::exchange(other.ctx_, nullptr);
      v_ = other.v_;
      other.v_ = JS_UNDEFINED;
    }
    return *this;
  }

  ~OwnedValue() { reset(); }

  void reset() noexcept {
    if (ctx_ != nullptr) {
      // QuickJS frees only refcounted tags; safe for UNDEFINED/EXCEPTION.
      JS_FreeValue(ctx_, v_);
      ctx_ = nullptr;
      v_ = JS_UNDEFINED;
    }
  }

  [[nodiscard]] OwnedValue duplicate() const {
    if (ctx_ == nullptr) {
      return {};
    }
    return OwnedValue{ctx_, JS_DupValue(ctx_, v_)};
  }

  /** Relinquish ownership; caller must free or return to the engine. */
  [[nodiscard]] JSValue release() noexcept {
    ctx_ = nullptr;
    const JSValue out = v_;
    v_ = JS_UNDEFINED;
    return out;
  }

  [[nodiscard]] JSContext* context() const noexcept { return ctx_; }
  [[nodiscard]] JSValueConst get() const noexcept { return v_; }
  [[nodiscard]] bool empty() const noexcept { return ctx_ == nullptr; }

  // Instance predicates: empty() is never a typed value. An owned
  // JS_UNDEFINED sentinel (non-null ctx_) is the only is_undefined() true.
  [[nodiscard]] bool is_exception() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_exception(v_);
  }
  [[nodiscard]] bool is_undefined() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_undefined(v_);
  }
  [[nodiscard]] bool is_null() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_null(v_);
  }
  [[nodiscard]] bool is_nullish() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_nullish(v_);
  }
  [[nodiscard]] bool is_object() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_object(v_);
  }
  [[nodiscard]] bool is_bool() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_bool(v_);
  }
  [[nodiscard]] bool is_number() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_number(v_);
  }
  [[nodiscard]] bool is_string() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_string(v_);
  }
  [[nodiscard]] bool is_array() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_array(ctx_, v_);
  }
  [[nodiscard]] bool is_function() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_function(ctx_, v_);
  }
  [[nodiscard]] bool is_bigint() const noexcept {
    return ctx_ != nullptr && vacps::qjs::is_bigint(ctx_, v_);
  }

  /** Adopt a raw JSValue that is already owned by the caller. */
  [[nodiscard]] static OwnedValue take(JSContext* ctx, JSValue v) {
    return OwnedValue{ctx, v};
  }

  [[nodiscard]] static OwnedValue undefined(JSContext* ctx) {
    return OwnedValue{ctx, JS_UNDEFINED};
  }

  [[nodiscard]] static OwnedValue nullish(JSContext* ctx) {
    return OwnedValue{ctx, JS_NULL};
  }

  /** Owned JS_EXCEPTION sentinel (exception already pending on ctx). */
  [[nodiscard]] static OwnedValue exception(JSContext* ctx) {
    return OwnedValue{ctx, JS_EXCEPTION};
  }

  /** Own the result of JS_GetPropertyStr (or free on failure path). */
  [[nodiscard]] static OwnedValue get_property_str(
      JSContext* ctx,
      JSValueConst obj,
      const char* prop) {
    return OwnedValue{ctx, JS_GetPropertyStr(ctx, obj, prop)};
  }

 private:
  JSContext* ctx_{nullptr};
  JSValue v_{JS_UNDEFINED};
};

}  // namespace vacps::qjs
