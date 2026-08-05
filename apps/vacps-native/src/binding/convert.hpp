#pragma once

/**
 * Converter<T> — JS ↔ C++ for phase-1 primitives and Result types.
 *
 * Specialize Converter<T> in namespace vacps::binding for additional types.
 *
 * Ownership:
 * - from_js borrows JSValueConst (never frees / retains the argument).
 * - to_js returns qjs::OwnedValue (caller owns).
 * - from_js clears any pending QuickJS exception raised by coercion APIs and
 *   returns binding::Error instead (engine left clean on failure — Result
 *   contract).
 * - string_view is to_js-only (from_js would dangle).
 *
 * Numeric policy (strict):
 * - Integers require a JS Number (not BigInt, not coercible string/object).
 * - Value must be finite and an integer (trunc(d) == d).
 * - Value must lie in the exact target range.
 * - int64/uint64 Number paths additionally require the JS safe-integer range
 *   (+/-(2^53-1)). Larger magnitudes are RangeError; BigInt is out of scope.
 * - double accepts any JS Number (including NaN / ±Infinity).
 * - float accepts NaN / ±Infinity, but rejects finite doubles that overflow
 *   the float range (no silent conversion to ±Infinity).
 *
 * Strings require JS_IsString (no ToString coercion of arbitrary values).
 *
 * qjs::OwnedValue passthrough requires a non-empty value from the same
 * JSContext. That ownership relation is a Narrow caller precondition.
 */

#include "binding/env.hpp"
#include "binding/error.hpp"
#include "qjs/owned_value.hpp"
#include "qjs/scoped_cstring.hpp"
#include "runtime/error.hpp"

#include <quickjs.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vacps::binding {

// Second parameter is an implementation detail for SFINAE specializations
// (e.g. size_t when it does not alias uint32/uint64).
template <class T, class Enable = void>
struct Converter;

namespace detail {

/** JS Number.MAX_SAFE_INTEGER == 2^53 - 1. */
inline constexpr std::int64_t k_max_safe_integer = 9007199254740991LL;

inline Result<double> require_js_number(JSContext* ctx, JSValueConst v) {
  if (!JS_IsNumber(v)) {
    return std::unexpected(Error::type("expected number"));
  }
  double out = 0;
  if (JS_ToFloat64(ctx, &out, v) != 0) {
    clear_exception(ctx);
    return std::unexpected(Error::type("expected number"));
  }
  return out;
}

inline Result<double> require_finite_number(JSContext* ctx, JSValueConst v) {
  auto d = require_js_number(ctx, v);
  if (!d) {
    return d;
  }
  if (!std::isfinite(*d)) {
    return std::unexpected(Error::range("expected finite number"));
  }
  return d;
}

inline Result<double> require_finite_integer_number(
    JSContext* ctx,
    JSValueConst v) {
  auto d = require_finite_number(ctx, v);
  if (!d) {
    return d;
  }
  if (std::trunc(*d) != *d) {
    return std::unexpected(Error::range("expected integer number"));
  }
  return d;
}

inline bool in_safe_integer_range(double d) noexcept {
  return d >= static_cast<double>(-k_max_safe_integer) &&
         d <= static_cast<double>(k_max_safe_integer);
}

inline Result<std::string> string_from_js(JSContext* ctx, JSValueConst v) {
  if (!JS_IsString(v)) {
    return std::unexpected(Error::type("expected string"));
  }
  // ScopedCString owns the QuickJS cstring; std::string allocation failure
  // cannot leak it (destructor runs during stack unwinding).
  auto cs = qjs::ScopedCString::from_value(ctx, v);
  if (cs.empty()) {
    clear_exception(ctx);
    return std::unexpected(Error::type("expected string"));
  }
  // view() preserves embedded NULs via the JS_ToCStringLen length.
  return std::string{cs.view()};
}

inline Result<std::vector<std::uint8_t>> bytes_from_js(
    JSContext* ctx,
    JSValueConst v) {
  if (JS_IsString(v)) {
    auto s = string_from_js(ctx, v);
    if (!s) {
      return std::unexpected(std::move(s.error()));
    }
    return std::vector<std::uint8_t>(s->begin(), s->end());
  }

  // TypedArray first (covers Uint8Array etc.).
  size_t byte_offset = 0;
  size_t byte_length = 0;
  size_t bytes_per_element = 0;
  JSValue ab = JS_GetTypedArrayBuffer(
      ctx, v, &byte_offset, &byte_length, &bytes_per_element);
  if (JS_IsException(ab)) {
    clear_exception(ctx);
    // Fall through to raw ArrayBuffer.
  } else if (!JS_IsUndefined(ab) && !JS_IsNull(ab)) {
    size_t ab_size = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, ab);
    JS_FreeValue(ctx, ab);
    if (ab_ptr != nullptr && byte_offset <= ab_size &&
        byte_length <= ab_size - byte_offset) {
      return std::vector<std::uint8_t>(
          ab_ptr + byte_offset, ab_ptr + byte_offset + byte_length);
    }
    clear_exception(ctx);
    return std::unexpected(
        Error::type("expected string, ArrayBuffer, or TypedArray"));
  } else {
    JS_FreeValue(ctx, ab);
  }

  size_t size = 0;
  uint8_t* buf = JS_GetArrayBuffer(ctx, &size, v);
  if (buf != nullptr) {
    return std::vector<std::uint8_t>(buf, buf + size);
  }
  clear_exception(ctx);
  return std::unexpected(
      Error::type("expected string, ArrayBuffer, or TypedArray"));
}

template <class T>
Result<T> integral_from_js(Env env, JSValueConst v, const char* label) {
  static_assert(std::is_integral_v<T>);
  JSContext* ctx = env.context();
  auto d = require_finite_integer_number(ctx, v);
  if (!d) {
    // Retarget type message when input was not a number.
    if (d.error().kind == ErrorKind::type) {
      return std::unexpected(Error::type(std::string{"expected "} + label));
    }
    return std::unexpected(std::move(d.error()));
  }

  // 64-bit Number paths must stay inside JS safe-integer range.
  if constexpr (sizeof(T) >= 8) {
    if (!in_safe_integer_range(*d)) {
      return std::unexpected(Error::range(
          std::string{label} + " exceeds JS safe integer range"));
    }
  }

  if constexpr (std::is_signed_v<T>) {
    constexpr auto lo = static_cast<double>(std::numeric_limits<T>::min());
    constexpr auto hi = static_cast<double>(std::numeric_limits<T>::max());
    if (*d < lo || *d > hi) {
      return std::unexpected(
          Error::range(std::string{label} + " out of range"));
    }
    return static_cast<T>(*d);
  } else {
    if (*d < 0) {
      return std::unexpected(
          Error::range(std::string{label} + " must be non-negative"));
    }
    constexpr auto hi = static_cast<double>(std::numeric_limits<T>::max());
    if (*d > hi) {
      return std::unexpected(
          Error::range(std::string{label} + " out of range"));
    }
    return static_cast<T>(*d);
  }
}

template <class T>
qjs::OwnedValue integral_to_js(Env env, T n) {
  static_assert(std::is_integral_v<T>);
  if constexpr (std::is_signed_v<T>) {
    if constexpr (sizeof(T) <= 4) {
      return env.int32(static_cast<std::int32_t>(n));
    } else {
      if (n < -k_max_safe_integer || n > k_max_safe_integer) {
        return qjs::OwnedValue::take(
            env.context(),
            throw_range(
                env.context(), "int64 value exceeds JS safe integer range"));
      }
      return env.int64(static_cast<std::int64_t>(n));
    }
  } else {
    if constexpr (sizeof(T) <= 4) {
      return env.uint32(static_cast<std::uint32_t>(n));
    } else {
      if (n > static_cast<std::uint64_t>(k_max_safe_integer)) {
        return qjs::OwnedValue::take(
            env.context(),
            throw_range(
                env.context(), "uint64 value exceeds JS safe integer range"));
      }
      return env.int64(static_cast<std::int64_t>(n));
    }
  }
}

}  // namespace detail

// ── void (to_js only) ──────────────────────────────────────────────

template <>
struct Converter<void> {
  static qjs::OwnedValue to_js(Env env) { return env.undefined(); }
};

// ── bool ───────────────────────────────────────────────────────────

template <>
struct Converter<bool> {
  static Result<bool> from_js(Env env, JSValueConst v) {
    if (!JS_IsBool(v)) {
      return std::unexpected(Error::type("expected boolean"));
    }
    const int b = JS_ToBool(env.context(), v);
    if (b < 0) {
      clear_exception(env.context());
      return std::unexpected(Error::type("expected boolean"));
    }
    return b != 0;
  }

  static qjs::OwnedValue to_js(Env env, bool b) { return env.boolean(b); }
};

// ── integers ───────────────────────────────────────────────────────

template <>
struct Converter<std::int8_t> {
  static Result<std::int8_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::int8_t>(env, v, "int8");
  }
  static qjs::OwnedValue to_js(Env env, std::int8_t n) {
    return detail::integral_to_js(env, n);
  }
};

template <>
struct Converter<std::uint8_t> {
  static Result<std::uint8_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::uint8_t>(env, v, "uint8");
  }
  static qjs::OwnedValue to_js(Env env, std::uint8_t n) {
    return detail::integral_to_js(env, n);
  }
};

template <>
struct Converter<std::int16_t> {
  static Result<std::int16_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::int16_t>(env, v, "int16");
  }
  static qjs::OwnedValue to_js(Env env, std::int16_t n) {
    return detail::integral_to_js(env, n);
  }
};

template <>
struct Converter<std::uint16_t> {
  static Result<std::uint16_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::uint16_t>(env, v, "uint16");
  }
  static qjs::OwnedValue to_js(Env env, std::uint16_t n) {
    return detail::integral_to_js(env, n);
  }
};

template <>
struct Converter<std::int32_t> {
  static Result<std::int32_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::int32_t>(env, v, "int32");
  }

  static qjs::OwnedValue to_js(Env env, std::int32_t n) { return env.int32(n); }
};

template <>
struct Converter<std::uint32_t> {
  static Result<std::uint32_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::uint32_t>(env, v, "uint32");
  }

  static qjs::OwnedValue to_js(Env env, std::uint32_t n) { return env.uint32(n); }
};

template <>
struct Converter<std::int64_t> {
  static Result<std::int64_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::int64_t>(env, v, "int64");
  }

  static qjs::OwnedValue to_js(Env env, std::int64_t n) {
    return detail::integral_to_js(env, n);
  }
};

template <>
struct Converter<std::uint64_t> {
  static Result<std::uint64_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::uint64_t>(env, v, "uint64");
  }
  static qjs::OwnedValue to_js(Env env, std::uint64_t n) {
    return detail::integral_to_js(env, n);
  }
};

// std::size_t may alias uint32_t or uint64_t — only specialize when distinct.
template <class T>
struct Converter<
    T,
    std::enable_if_t<
        std::is_same_v<T, std::size_t> &&
        !std::is_same_v<std::size_t, std::uint32_t> &&
        !std::is_same_v<std::size_t, std::uint64_t>>> {
  static Result<std::size_t> from_js(Env env, JSValueConst v) {
    return detail::integral_from_js<std::size_t>(env, v, "size_t");
  }

  static qjs::OwnedValue to_js(Env env, std::size_t n) {
    if (n <= static_cast<std::size_t>(UINT32_MAX)) {
      return env.uint32(static_cast<std::uint32_t>(n));
    }
    if (n <= static_cast<std::size_t>(detail::k_max_safe_integer)) {
      return env.int64(static_cast<std::int64_t>(n));
    }
    return qjs::OwnedValue::take(
        env.context(),
        throw_range(env.context(), "size_t exceeds JS safe integer range"));
  }
};

// ── double ─────────────────────────────────────────────────────────

template <>
struct Converter<double> {
  static Result<double> from_js(Env env, JSValueConst v) {
    return detail::require_js_number(env.context(), v);
  }

  static qjs::OwnedValue to_js(Env env, double n) { return env.float64(n); }
};

template <>
struct Converter<float> {
  static Result<float> from_js(Env env, JSValueConst v) {
    auto d = Converter<double>::from_js(env, v);
    if (!d) {
      return std::unexpected(std::move(d.error()));
    }
    // Preserve intentional NaN / ±Infinity; reject finite overflow only.
    if (std::isnan(*d) || std::isinf(*d)) {
      return static_cast<float>(*d);
    }
    constexpr double lo =
        static_cast<double>(std::numeric_limits<float>::lowest());
    constexpr double hi =
        static_cast<double>(std::numeric_limits<float>::max());
    if (*d < lo || *d > hi) {
      return std::unexpected(Error::range("float out of range"));
    }
    return static_cast<float>(*d);
  }

  static qjs::OwnedValue to_js(Env env, float n) {
    return env.float64(static_cast<double>(n));
  }
};

// ── string ─────────────────────────────────────────────────────────

template <>
struct Converter<std::string> {
  static Result<std::string> from_js(Env env, JSValueConst v) {
    return detail::string_from_js(env.context(), v);
  }

  static qjs::OwnedValue to_js(Env env, const std::string& s) {
    return env.string(s);
  }

  static qjs::OwnedValue to_js(Env env, std::string&& s) {
    return env.string(s);
  }

  static qjs::OwnedValue to_js(Env env, std::string_view s) {
    return env.string(s);
  }
};

/** string_view only for to_js (from_js would dangle). */
template <>
struct Converter<std::string_view> {
  static qjs::OwnedValue to_js(Env env, std::string_view s) {
    return env.string(s);
  }
};

template <>
struct Converter<const char*> {
  static qjs::OwnedValue to_js(Env env, const char* s) {
    return env.string(std::string_view{s});
  }
};

// ── bytes ──────────────────────────────────────────────────────────

template <>
struct Converter<std::vector<std::uint8_t>> {
  static Result<std::vector<std::uint8_t>> from_js(Env env, JSValueConst v) {
    return detail::bytes_from_js(env.context(), v);
  }

  static qjs::OwnedValue to_js(Env env, const std::vector<std::uint8_t>& bytes) {
    return env.array_buffer(bytes.data(), bytes.size());
  }

  static qjs::OwnedValue to_js(Env env, std::vector<std::uint8_t>&& bytes) {
    return env.array_buffer(bytes.data(), bytes.size());
  }
};

template <>
struct Converter<std::vector<std::byte>> {
  static Result<std::vector<std::byte>> from_js(Env env, JSValueConst v) {
    auto bytes = detail::bytes_from_js(env.context(), v);
    if (!bytes) {
      return std::unexpected(std::move(bytes.error()));
    }
    std::vector<std::byte> out(bytes->size());
    if (!bytes->empty()) {
      std::memcpy(out.data(), bytes->data(), bytes->size());
    }
    return out;
  }

  static qjs::OwnedValue to_js(Env env, const std::vector<std::byte>& bytes) {
    return env.array_buffer(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  }

  static qjs::OwnedValue to_js(Env env, std::vector<std::byte>&& bytes) {
    return env.array_buffer(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  }
};

// ── qjs::OwnedValue passthrough (sole JSValue owner) ───────────────────

template <>
struct Converter<qjs::OwnedValue> {
  static Result<qjs::OwnedValue> from_js(Env env, JSValueConst v) {
    // Dup so caller owns an independent value (argv is borrowed).
    return qjs::OwnedValue{env.context(), JS_DupValue(env.context(), v)};
  }

  static qjs::OwnedValue to_js(Env, qjs::OwnedValue&& v) {
    // Save context before release — argument evaluation order is unspecified.
    JSContext* vc = v.context();
    JSValue raw = v.release();
    return qjs::OwnedValue::take(vc, raw);
  }

  static qjs::OwnedValue to_js(Env, const qjs::OwnedValue& v) {
    return v.duplicate();
  }
};

// ── runtime::Result / vacps::Result / binding::Result (to_js) ──────

template <class T>
struct Converter<vacps::runtime::Result<T>> {
  static qjs::OwnedValue to_js(Env env, vacps::runtime::Result<T>&& r) {
    if (!r) {
      return qjs::OwnedValue::take(
          env.context(), throw_error(env.context(), r.error()));
    }
    if constexpr (std::is_void_v<T>) {
      return env.undefined();
    } else {
      return Converter<std::remove_cvref_t<T>>::to_js(env, std::move(*r));
    }
  }

  static qjs::OwnedValue to_js(Env env, const vacps::runtime::Result<T>& r) {
    if (!r) {
      return qjs::OwnedValue::take(
          env.context(), throw_error(env.context(), r.error()));
    }
    if constexpr (std::is_void_v<T>) {
      return env.undefined();
    } else {
      return Converter<std::remove_cvref_t<T>>::to_js(env, *r);
    }
  }
};

template <class T>
struct Converter<vacps::Result<T>> {
  static qjs::OwnedValue to_js(Env env, vacps::Result<T>&& r) {
    if (!r) {
      return qjs::OwnedValue::take(
          env.context(), throw_error(env.context(), r.error()));
    }
    if constexpr (std::is_void_v<T>) {
      return env.undefined();
    } else {
      return Converter<std::remove_cvref_t<T>>::to_js(env, std::move(*r));
    }
  }

  static qjs::OwnedValue to_js(Env env, const vacps::Result<T>& r) {
    if (!r) {
      return qjs::OwnedValue::take(
          env.context(), throw_error(env.context(), r.error()));
    }
    if constexpr (std::is_void_v<T>) {
      return env.undefined();
    } else {
      return Converter<std::remove_cvref_t<T>>::to_js(env, *r);
    }
  }
};

template <class T>
struct Converter<Result<T>> {
  static qjs::OwnedValue to_js(Env env, Result<T>&& r) {
    if (!r) {
      return qjs::OwnedValue::take(
          env.context(), throw_error(env.context(), r.error()));
    }
    if constexpr (std::is_void_v<T>) {
      return env.undefined();
    } else {
      return Converter<std::remove_cvref_t<T>>::to_js(env, std::move(*r));
    }
  }

  static qjs::OwnedValue to_js(Env env, const Result<T>& r) {
    if (!r) {
      return qjs::OwnedValue::take(
          env.context(), throw_error(env.context(), r.error()));
    }
    if constexpr (std::is_void_v<T>) {
      return env.undefined();
    } else {
      return Converter<std::remove_cvref_t<T>>::to_js(env, *r);
    }
  }
};

// ── encode helpers used by function adapter ────────────────────────

namespace detail {

template <class R>
qjs::OwnedValue encode_return(Env env, R&& value) {
  using T = std::remove_cvref_t<R>;
  if constexpr (std::is_same_v<T, qjs::OwnedValue>) {
    return std::forward<R>(value);
  } else if constexpr (std::is_same_v<T, JSValue>) {
    return qjs::OwnedValue::take(env.context(), std::forward<R>(value));
  } else {
    return Converter<T>::to_js(env, std::forward<R>(value));
  }
}

template <class Fn, class... Args>
qjs::OwnedValue invoke_and_encode(Env env, Fn&& fn, Args&&... args) {
  using R = std::invoke_result_t<Fn, Args...>;
  if constexpr (std::is_void_v<R>) {
    std::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...);
    return env.undefined();
  } else {
    return encode_return(
        env, std::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...));
  }
}

}  // namespace detail

}  // namespace vacps::binding
