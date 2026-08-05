#pragma once

/**
 * Runtime error model.
 * Expected operational failures use std::expected. Unexpected exceptions at
 * C ABI / noexcept owner handlers may terminate; there is no catch-all
 * barrier that maps programmer failures to healthy runtime continuation.
 * Worker entry stores exception_ptr; non-allocation async exceptions map via
 * error_from_exception_ptr. Native std::bad_alloc in that helper is
 * fail-fast (terminate) — not a runtime::Error / Promise rejection.
 * QuickJS C API allocation failure remains JS_EXCEPTION and is distinct from
 * native OOM. Errc::allocation_failure is retained for explicitly modeled
 * paths outside this helper.
 */

#include "app/error.hpp"

#include <cerrno>
#include <exception>
#include <expected>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace vacps::runtime {

enum class Errc {
  invalid_state,
  runtime_stopping,
  cancelled,
  js_exception,
  native_failure,
  allocation_failure,
  internal_error,
};

struct Error {
  Errc code{Errc::internal_error};
  std::string message;
  /** Optional operation name (e.g. "open", "read") from domain I/O. */
  std::string operation;
  /** Optional POSIX errno / system code; 0 = unavailable. */
  int system_code{0};

  static Error invalid_state(std::string message) {
    return {Errc::invalid_state, std::move(message), {}, 0};
  }
  static Error stopping(std::string message = "runtime is stopping") {
    return {Errc::runtime_stopping, std::move(message), {}, 0};
  }
  static Error cancelled(std::string message = "operation cancelled") {
    return {Errc::cancelled, std::move(message), {}, 0};
  }
  static Error cancelled_op(std::string operation) {
    return {
        Errc::cancelled,
        operation.empty() ? std::string{"operation cancelled"}
                          : operation + ": operation cancelled",
        std::move(operation),
        // ECANCELED is 125 on Linux; keep portable via errno.h at call sites
        // when available. 0 here is fine; code ERR_CANCELLED is authoritative.
        0};
  }
  static Error js(std::string message) {
    return {Errc::js_exception, std::move(message), {}, 0};
  }
  static Error native(std::string message) {
    return {Errc::native_failure, std::move(message), {}, 0};
  }
  static Error native_io(std::string message, std::string operation, int system_code) {
    return {
        Errc::native_failure,
        std::move(message),
        std::move(operation),
        system_code};
  }
  static Error allocation(std::string message = "allocation failed") {
    return {Errc::allocation_failure, std::move(message), {}, 0};
  }
  static Error internal(std::string message) {
    return {Errc::internal_error, std::move(message), {}, 0};
  }

  /**
   * Map domain vacps::Error into runtime Error, preserving operation/system_code.
   * ECANCELED → cancelled; otherwise native_failure.
   */
  static Error from_domain(const vacps::Error& e) {
#if defined(__linux__)
    if (e.system_code == ECANCELED) {
      Error out{
          Errc::cancelled,
          e.message.empty() ? std::string{"operation cancelled"} : e.message,
          e.operation,
          e.system_code};
      return out;
    }
#endif
    return {
        Errc::native_failure,
        e.message,
        e.operation,
        e.system_code};
  }

  static Error from_domain(vacps::Error&& e) {
#if defined(__linux__)
    if (e.system_code == ECANCELED) {
      Error out{
          Errc::cancelled,
          e.message.empty() ? std::string{"operation cancelled"}
                            : std::move(e.message),
          std::move(e.operation),
          e.system_code};
      return out;
    }
#endif
    return {
        Errc::native_failure,
        std::move(e.message),
        std::move(e.operation),
        e.system_code};
  }
};

/**
 * Map captured exception_ptr to Error (co_spawn / worker completion handlers).
 * Native std::bad_alloc terminates: do not allocate Error/JS to report OOM.
 * Other std::exception / unknown exceptions map to the supplied code/fallback.
 * Any allocation failure while constructing the returned Error escapes this
 * noexcept function and therefore terminates; there is no secondary
 * allocating fallback path.
 */
inline Error error_from_exception_ptr(
    std::exception_ptr ep,
    Errc code = Errc::internal_error,
    std::string_view fallback = "unknown exception") noexcept {
  if (ep == nullptr) {
    return Error{code, std::string{fallback}, {}, 0};
  }
  try {
    std::rethrow_exception(std::move(ep));
  } catch (const std::bad_alloc&) {
    std::terminate();
  } catch (const std::exception& ex) {
    return Error{code, std::string{ex.what()}, {}, 0};
  } catch (...) {
    return Error{code, std::string{fallback}, {}, 0};
  }
}

template <class T>
using Result = std::expected<T, Error>;

using VoidResult = Result<void>;

inline VoidResult success() {
  return {};
}

}  // namespace vacps::runtime
