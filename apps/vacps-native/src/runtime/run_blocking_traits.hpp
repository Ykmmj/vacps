#pragma once

/**
 * Worker callable traits for Runtime::Async::run_blocking (pure C++ only).
 *
 * Contract: Narrow. Worker callables must not capture JSContext*, JSValue,
 * vacps::qjs::OwnedValue, vacps::qjs::ScopedCString, PromiseCapability, or
 * other JS-owning RAII. Only the result type is checked at compile time;
 * captures remain a documented invariant (C++ cannot portably reject
 * arbitrary lambda captures without also rejecting normal pure-C++ state).
 *
 * Expected I/O/domain failure is Result, not an exception. Exceptions from
 * the worker callable are captured as std::exception_ptr and mapped to
 * runtime::Error on the owner thread after resume.
 */

#include "runtime/error.hpp"
#include "qjs/owned_value.hpp"
#include "qjs/scoped_cstring.hpp"

#include <quickjs.h>

#include <concepts>
#include <expected>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace vacps::runtime {

template <class T>
concept RuntimeResult =
    requires { typename std::remove_cvref_t<T>::value_type; } &&
    std::same_as<
        std::remove_cvref_t<T>,
        std::expected<typename std::remove_cvref_t<T>::value_type, Error>>;

/** Callable for thread-pool run_blocking: () or (stop_token). */
template <class Fn>
concept BlockingCallable =
    std::invocable<std::decay_t<Fn>&> ||
    std::invocable<std::decay_t<Fn>&, std::stop_token>;

namespace detail {

template <class T>
inline constexpr bool is_runtime_result_v =
    RuntimeResult<std::remove_cvref_t<T>>;

template <class T>
struct IsJsThreadConfined : std::bool_constant<
    std::is_same_v<std::remove_cvref_t<T>, JSValue> ||
    std::is_same_v<std::remove_cvref_t<T>, JSValueConst> ||
    std::is_same_v<std::remove_cvref_t<T>, vacps::qjs::OwnedValue> ||
    std::is_same_v<std::remove_cvref_t<T>, vacps::qjs::ScopedCString> ||
    std::is_same_v<std::remove_cvref_t<T>, JSContext*> ||
    std::is_same_v<std::remove_cvref_t<T>, JSRuntime*>> {};

template <class T>
inline constexpr bool is_js_thread_confined_v = IsJsThreadConfined<T>::value;

template <class R>
struct WorkerValue {
  using type = std::remove_cvref_t<R>;
};

template <class T>
struct WorkerValue<std::expected<T, Error>> {
  using type = T;
};

template <>
struct WorkerValue<void> {
  using type = void;
};

template <class R>
using worker_value_t = typename WorkerValue<std::remove_cvref_t<R>>::type;

template <class Fn>
struct WorkerRawResult {
  using type = std::invoke_result_t<Fn&>;
};

template <class Fn>
  requires std::invocable<Fn&, std::stop_token>
struct WorkerRawResult<Fn> {
  using type = std::invoke_result_t<Fn&, std::stop_token>;
};

template <class Fn>
using worker_raw_result_t = typename WorkerRawResult<Fn>::type;

template <class Fn>
using worker_value_result_t = worker_value_t<worker_raw_result_t<Fn>>;

/**
 * Invoke the pure-C++ blocking callable. Does not catch exceptions — the
 * noexcept worker wrapper stores std::current_exception() as data.
 * Pre-start cancellation is checked by the worker wrapper before calling.
 */
template <BlockingCallable Fn>
auto invoke_blocking(Fn& fn, std::stop_token stop)
    -> Result<worker_value_result_t<Fn>> {
  using Raw = worker_raw_result_t<Fn>;
  using Value = worker_value_result_t<Fn>;

  static_assert(
      !is_js_thread_confined_v<Value>,
      "run_blocking worker result must not be JS-thread-confined");

  if constexpr (std::invocable<Fn&, std::stop_token>) {
    if constexpr (std::is_void_v<std::invoke_result_t<Fn&, std::stop_token>>) {
      std::invoke(fn, stop);
      return {};
    } else {
      auto value = std::invoke(fn, stop);
      if constexpr (RuntimeResult<decltype(value)>) {
        return value;
      } else {
        return Result<Value>{std::move(value)};
      }
    }
  } else {
    (void)stop;
    if constexpr (std::is_void_v<Raw>) {
      std::invoke(fn);
      return {};
    } else {
      auto value = std::invoke(fn);
      if constexpr (RuntimeResult<decltype(value)>) {
        return value;
      } else {
        return Result<Value>{std::move(value)};
      }
    }
  }
}

}  // namespace detail
}  // namespace vacps::runtime
