#pragma once

/**
 * ValueRef — non-owning view of a JSValueConst (argument or this-value).
 *
 * Never frees `v_`. Lifetime is tied to the active JS callback frame
 * (borrowed argv / this) or other caller-guaranteed span.
 */

#include "binding/convert.hpp"
#include "binding/env.hpp"
#include "binding/error.hpp"

#include <quickjs.h>

namespace vacps::binding {

class ValueRef {
 public:
  ValueRef(Env env, JSValueConst v) noexcept : env_(env), v_(v) {}

  [[nodiscard]] Env env() const noexcept { return env_; }
  [[nodiscard]] JSContext* context() const noexcept { return env_.context(); }
  [[nodiscard]] JSValueConst get() const noexcept { return v_; }

  [[nodiscard]] bool is_undefined() const noexcept {
    return JS_IsUndefined(v_);
  }
  [[nodiscard]] bool is_null() const noexcept { return JS_IsNull(v_); }
  [[nodiscard]] bool is_nullish() const noexcept {
    return JS_IsUndefined(v_) || JS_IsNull(v_);
  }
  [[nodiscard]] bool is_bool() const noexcept { return JS_IsBool(v_); }
  [[nodiscard]] bool is_number() const noexcept { return JS_IsNumber(v_); }
  [[nodiscard]] bool is_string() const noexcept { return JS_IsString(v_); }
  [[nodiscard]] bool is_object() const noexcept { return JS_IsObject(v_); }
  [[nodiscard]] bool is_exception() const noexcept {
    return JS_IsException(v_);
  }

  template <class T>
  [[nodiscard]] Result<T> as() const {
    return Converter<T>::from_js(env_, v_);
  }

 private:
  Env env_;
  JSValueConst v_;
};

}  // namespace vacps::binding
