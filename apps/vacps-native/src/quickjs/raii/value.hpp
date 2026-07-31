#pragma once

#include <quickjs.h>

#include <cstdint>
#include <string_view>
#include <utility>

namespace vacps::js {

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

/** Move-only JSValue owner. Copy only via duplicate() (explicit JS_DupValue). */
class Value {
 public:
  Value() noexcept = default;

  Value(JSContext* ctx, JSValue v) noexcept : ctx_(ctx), v_(v) {}

  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;

  Value(Value&& other) noexcept
      : ctx_(std::exchange(other.ctx_, nullptr)), v_(other.v_) {
    other.v_ = JS_UNDEFINED;
  }

  Value& operator=(Value&& other) noexcept {
    if (this != &other) {
      reset();
      ctx_ = std::exchange(other.ctx_, nullptr);
      v_ = other.v_;
      other.v_ = JS_UNDEFINED;
    }
    return *this;
  }

  ~Value() { reset(); }

  void reset() noexcept {
    if (ctx_ != nullptr) {
      JS_FreeValue(ctx_, v_);
      ctx_ = nullptr;
      v_ = JS_UNDEFINED;
    }
  }

  [[nodiscard]] Value duplicate() const {
    if (ctx_ == nullptr) {
      return {};
    }
    return Value{ctx_, JS_DupValue(ctx_, v_)};
  }

  /** Relinquish ownership; caller must JS_FreeValue or return to engine. */
  [[nodiscard]] JSValue release() noexcept {
    ctx_ = nullptr;
    const JSValue out = v_;
    v_ = JS_UNDEFINED;
    return out;
  }

  [[nodiscard]] JSContext* context() const noexcept { return ctx_; }
  [[nodiscard]] JSValueConst get() const noexcept { return v_; }
  [[nodiscard]] bool empty() const noexcept { return ctx_ == nullptr; }

  [[nodiscard]] bool is_exception() const noexcept {
    return vacps::js::is_exception(v_);
  }
  [[nodiscard]] bool is_undefined() const noexcept {
    return ctx_ != nullptr && vacps::js::is_undefined(v_);
  }
  [[nodiscard]] bool is_null() const noexcept {
    return ctx_ != nullptr && vacps::js::is_null(v_);
  }
  [[nodiscard]] bool is_nullish() const noexcept {
    return ctx_ != nullptr && vacps::js::is_nullish(v_);
  }
  [[nodiscard]] bool is_object() const noexcept {
    return ctx_ != nullptr && vacps::js::is_object(v_);
  }
  [[nodiscard]] bool is_bool() const noexcept {
    return ctx_ != nullptr && vacps::js::is_bool(v_);
  }
  [[nodiscard]] bool is_number() const noexcept {
    return ctx_ != nullptr && vacps::js::is_number(v_);
  }
  [[nodiscard]] bool is_string() const noexcept {
    return ctx_ != nullptr && vacps::js::is_string(v_);
  }
  [[nodiscard]] bool is_array() const noexcept {
    return ctx_ != nullptr && vacps::js::is_array(ctx_, v_);
  }
  [[nodiscard]] bool is_function() const noexcept {
    return ctx_ != nullptr && vacps::js::is_function(ctx_, v_);
  }
  [[nodiscard]] bool is_bigint() const noexcept {
    return ctx_ != nullptr && vacps::js::is_bigint(ctx_, v_);
  }

  /** Own the result of JS_GetPropertyStr (or free on failure path). */
  [[nodiscard]] static Value get_property_str(
      JSContext* ctx,
      JSValueConst obj,
      const char* prop) {
    return Value{ctx, JS_GetPropertyStr(ctx, obj, prop)};
  }

  [[nodiscard]] static Value get_property_uint32(
      JSContext* ctx,
      JSValueConst obj,
      std::uint32_t idx) {
    return Value{ctx, JS_GetPropertyUint32(ctx, obj, idx)};
  }

  [[nodiscard]] static Value new_object(JSContext* ctx) {
    return Value{ctx, JS_NewObject(ctx)};
  }

  [[nodiscard]] static Value new_array(JSContext* ctx) {
    return Value{ctx, JS_NewArray(ctx)};
  }

  [[nodiscard]] static Value new_string(JSContext* ctx, std::string_view s) {
    return Value{ctx, JS_NewStringLen(ctx, s.data(), s.size())};
  }

  /** Set property, transferring ownership of `child` into `obj`. */
  void set_property_str(const char* prop, Value child) {
    if (ctx_ == nullptr) {
      return;
    }
    JS_SetPropertyStr(ctx_, v_, prop, child.release());
  }

  void set_property_uint32(std::uint32_t idx, Value child) {
    if (ctx_ == nullptr) {
      return;
    }
    JS_SetPropertyUint32(ctx_, v_, idx, child.release());
  }

 private:
  JSContext* ctx_{nullptr};
  JSValue v_{JS_UNDEFINED};
};

}  // namespace vacps::js
