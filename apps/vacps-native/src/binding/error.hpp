#pragma once

/**
 * Centralized binding / domain error → QuickJS exception mapping.
 *
 * Strategies:
 * - binding::ErrorKind::type     → JS TypeError
 * - binding::ErrorKind::range    → JS RangeError
 * - binding::ErrorKind::internal → JS InternalError
 * - binding::ErrorKind::error    → JS InternalError (generic message)
 * - vacps::runtime::Error        → TypeError for invalid_state only;
 *                                  ordinary Error path via throw_js_error for
 *                                  native_failure/cancelled (see js_error);
 *                                  InternalError for allocation_failure
 * - vacps::Error                 → InternalError with message (prefer
 *                                  runtime::Error::from_domain at module edge)
 * - unexpected exceptions at synchronous QuickJS C callbacks → terminate
 * - native std::bad_alloc at these helpers → std::terminate
 *   (do not allocate InternalError after native OOM; QuickJS C API OOM is
 *   still JS_EXCEPTION and is not the same path). Scoped to these helpers,
 *   not a claim that every repository boundary has been audited.
 *
 * Sync vs async failure surfaces:
 * - Synchronous binding failures (including async free/method/static argument
 *   decode or bad this at call entry) throw JS exceptions via throw_* and
 *   create no Promise. Missing Runtime::Async* is a registration-time misuse.
 * - After Runtime::Async::promise creates a Promise, start/coroutine/encode
 *   failures reject through Runtime::Async only. Binding never settles
 *   PromiseCapability directly and must not leave a pending JS exception
 *   when returning a runtime::Error encode failure to Runtime::Async.
 *
 * Exception contract:
 * - throw_* never throw C++ exceptions.
 * - throw_* never overwrite an already-pending JS exception; they return
 *   JS_EXCEPTION and leave the existing pending exception untouched.
 * - APIs returning binding::Result must leave the engine with no pending JS
 *   exception (clear after converting a QuickJS failure into Error).
 * - APIs returning qjs::OwnedValue(JS_EXCEPTION) preserve exactly one pending
 *   exception (the one that caused the failure).
 */

#include "app/error.hpp"
#include "qjs/owned_value.hpp"
#include "runtime/error.hpp"
#include "runtime/js_error.hpp"

#include <quickjs.h>

#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace vacps::binding {

enum class ErrorKind {
  type,      // TypeError
  range,     // RangeError
  internal,  // InternalError
  error,     // generic → InternalError
};

struct Error {
  ErrorKind kind{ErrorKind::error};
  std::string message;

  Error() = default;
  explicit Error(std::string msg) : message(std::move(msg)) {}
  Error(ErrorKind k, std::string msg) : kind(k), message(std::move(msg)) {}

  static Error type(std::string msg) {
    return {ErrorKind::type, std::move(msg)};
  }
  static Error range(std::string msg) {
    return {ErrorKind::range, std::move(msg)};
  }
  static Error internal(std::string msg) {
    return {ErrorKind::internal, std::move(msg)};
  }
  static Error from_runtime(const vacps::runtime::Error& e) {
    switch (e.code) {
      case vacps::runtime::Errc::invalid_state:
        // Binding/domain validation → TypeError.
        return type(e.message);
      case vacps::runtime::Errc::allocation_failure:
        return internal(e.message.empty() ? "allocation failed" : e.message);
      case vacps::runtime::Errc::native_failure:
      case vacps::runtime::Errc::cancelled:
        // Prefer throw_js_error / make_js_error_object for structured native
        // failures (ordinary Error + errno code). Fallback kind is internal.
        return Error{ErrorKind::error, e.message};
      default:
        return internal(e.message);
    }
  }
  static Error from_domain(const vacps::Error& e) {
    return Error{ErrorKind::internal, e.message};
  }
};

template <class T>
using Result = std::expected<T, Error>;

using VoidResult = Result<void>;

namespace detail {

inline JSValue throw_kind_cstr(
    JSContext* ctx,
    ErrorKind kind,
    const char* msg) noexcept {
  switch (kind) {
    case ErrorKind::type:
      return JS_ThrowTypeError(ctx, "%s", msg);
    case ErrorKind::range:
      return JS_ThrowRangeError(ctx, "%s", msg);
    case ErrorKind::internal:
    case ErrorKind::error:
    default:
      return JS_ThrowInternalError(ctx, "%s", msg);
  }
}

}  // namespace detail

inline JSValue throw_error_msg(
    JSContext* ctx,
    ErrorKind kind,
    std::string_view message) noexcept {
  // Never overwrite a pending exception (preserve exactly one).
  if (JS_HasException(ctx)) {
    return JS_EXCEPTION;
  }
  const char* msg = "binding error";
  char stack[256];
  std::string owned;
  if (!message.empty()) {
    if (message.size() < sizeof(stack)) {
      std::memcpy(stack, message.data(), message.size());
      stack[message.size()] = '\0';
      msg = stack;
    } else {
      owned.assign(message.data(), message.size());
      msg = owned.c_str();
    }
  }
  return detail::throw_kind_cstr(ctx, kind, msg);
}

inline JSValue throw_error(JSContext* ctx, const Error& e) noexcept {
  return throw_error_msg(ctx, e.kind, e.message);
}

inline JSValue throw_error(
    JSContext* ctx,
    const vacps::runtime::Error& e) noexcept {
  // Structured native/cancelled/JS errors go through throw_js_error so
  // Error.code / errno / operation are preserved. Validation stays TypeError.
  return vacps::runtime::throw_js_error(ctx, e);
}

inline JSValue throw_error(JSContext* ctx, const vacps::Error& e) noexcept {
  return throw_error_msg(ctx, ErrorKind::internal, e.message);
}

inline JSValue throw_type(JSContext* ctx, std::string_view message) noexcept {
  return throw_error_msg(ctx, ErrorKind::type, message);
}

inline JSValue throw_range(JSContext* ctx, std::string_view message) noexcept {
  return throw_error_msg(ctx, ErrorKind::range, message);
}

inline JSValue throw_internal(
    JSContext* ctx,
    std::string_view message) noexcept {
  return throw_error_msg(ctx, ErrorKind::internal, message);
}

/** Clear a pending JS exception (if any) and discard it. */
inline void clear_exception(JSContext* ctx) noexcept {
  const JSValue ex = JS_GetException(ctx);
  JS_FreeValue(ctx, ex);
}

namespace detail {

/**
 * Drop an owned JS value on a noexcept path.
 * An owned JS_EXCEPTION sentinel consumes/clears the pending engine exception
 * before the sentinel is released; no C++ exception escapes.
 */
inline void discard_owned(qjs::OwnedValue v) noexcept {
  if (v.is_exception()) {
    JSContext* ec = v.context();
    clear_exception(ec);
    (void)v.release();
    return;
  }
  v.reset();
}

}  // namespace detail

}  // namespace vacps::binding
