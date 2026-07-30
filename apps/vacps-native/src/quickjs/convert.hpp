#pragma once

#include "app/error.hpp"
#include "quickjs/cstring.hpp"
#include "quickjs/value.hpp"

#include <quickjs.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vacps::js {

template <class T>
struct converter;

template <>
struct converter<bool> {
  static Result<bool> from_js(JSContext* ctx, JSValueConst v) {
    (void)ctx;
    if (!is_bool(v)) {
      return std::unexpected(Error{"JS value is not boolean"});
    }
    return JS_ToBool(ctx, v) != 0;
  }

  static Value to_js(JSContext* ctx, bool b) {
    return Value{ctx, JS_NewBool(ctx, b ? 1 : 0)};
  }
};

template <>
struct converter<std::int32_t> {
  static Result<std::int32_t> from_js(JSContext* ctx, JSValueConst v) {
    std::int32_t out = 0;
    if (JS_ToInt32(ctx, &out, v) != 0) {
      return std::unexpected(Error{"JS_ToInt32 failed"});
    }
    return out;
  }

  static Value to_js(JSContext* ctx, std::int32_t n) {
    return Value{ctx, JS_NewInt32(ctx, n)};
  }
};

template <>
struct converter<std::int64_t> {
  static Result<std::int64_t> from_js(JSContext* ctx, JSValueConst v) {
    std::int64_t out = 0;
    if (JS_ToInt64(ctx, &out, v) != 0) {
      return std::unexpected(Error{"JS_ToInt64 failed"});
    }
    return out;
  }

  static Value to_js(JSContext* ctx, std::int64_t n) {
    return Value{ctx, JS_NewInt64(ctx, n)};
  }
};

template <>
struct converter<double> {
  static Result<double> from_js(JSContext* ctx, JSValueConst v) {
    double out = 0;
    if (JS_ToFloat64(ctx, &out, v) != 0) {
      return std::unexpected(Error{"JS_ToFloat64 failed"});
    }
    return out;
  }

  static Value to_js(JSContext* ctx, double n) {
    return Value{ctx, JS_NewFloat64(ctx, n)};
  }
};

template <>
struct converter<std::string> {
  static Result<std::string> from_js(JSContext* ctx, JSValueConst v) {
    auto cs = CString::from_value(ctx, v);
    if (cs.empty()) {
      return std::unexpected(Error{"JS_ToCString failed"});
    }
    return cs.str();
  }

  static Value to_js(JSContext* ctx, std::string_view s) {
    return Value{ctx, JS_NewStringLen(ctx, s.data(), s.size())};
  }
};

template <class T>
struct converter<std::optional<T>> {
  static Result<std::optional<T>> from_js(JSContext* ctx, JSValueConst v) {
    if (is_nullish(v)) {
      return std::optional<T>{};
    }
    auto inner = converter<T>::from_js(ctx, v);
    if (!inner) {
      return std::unexpected(std::move(inner.error()));
    }
    return std::optional<T>{std::move(*inner)};
  }

  static Value to_js(JSContext* ctx, const std::optional<T>& o) {
    if (!o) {
      return Value{ctx, JS_NULL};
    }
    return converter<T>::to_js(ctx, *o);
  }
};

}  // namespace vacps::js
