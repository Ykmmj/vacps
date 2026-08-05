#pragma once

/**
 * Compile-time traits for binding async free functions, static async
 * functions, and ClassBuilder async methods.
 *
 * Accepted free / static callables:
 *   (Args...) -> runtime::Task<T>
 *   (std::stop_token, Args...) -> runtime::Task<T>
 *
 * Accepted method callables (ClassBuilder<T>):
 *   (Self, Args...) -> runtime::Task<R>
 *   (std::stop_token, Self, Args...) -> runtime::Task<R>
 * where Self is T& / const T& / T* / const T* / shared_ptr<T> by value /
 * const shared_ptr<T>& (const shared_ptr by value also allowed).
 * Rejected: T&&, T by value, non-const shared_ptr<T>&, shared_ptr<T>&&,
 * and any other unsupported reference category.
 *
 * runtime::Task<T> =
 *   boost::asio::awaitable<runtime::Result<T>, boost::asio::any_io_executor>
 * (including T = void).
 *
 * stop_token is injected by the runtime and is only legal in position zero.
 *
 * Decoded parameters (after optional leading stop_token, and after Self for
 * methods) must be by-value or const lvalue reference. Non-const lvalue and
 * rvalue reference parameters are rejected.
 *
 * Retained params/results must not be JS-thread-confined. User-defined
 * aggregate / lambda captures remain a documented invariant (C++ cannot
 * introspect captures). Generic or overloaded call operators are unsupported.
 */

#include "binding/detail/concepts.hpp"
#include "binding/detail/invoke.hpp"
#include "runtime/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <quickjs.h>

#include <concepts>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <tuple>
#include <type_traits>

namespace vacps::qjs {
class OwnedValue;
class ScopedCString;
}

namespace vacps::runtime {
class PromiseCapability;
}

namespace vacps::binding {
class CallbackInfo;
class Env;
class ValueRef;
}

namespace vacps::binding::detail {

// ── JS-thread-confined types (params / results retained across await) ─

template <class T>
inline constexpr bool is_async_js_confined_v = [] {
  using R = std::remove_cvref_t<T>;
  return std::is_same_v<R, JSValue> || std::is_same_v<R, JSValueConst> ||
         std::is_same_v<R, JSContext*> || std::is_same_v<R, JSRuntime*> ||
         std::is_same_v<R, vacps::qjs::OwnedValue> ||
         std::is_same_v<R, vacps::qjs::ScopedCString> ||
         std::is_same_v<R, Env> || std::is_same_v<R, CallbackInfo> ||
         std::is_same_v<R, ValueRef> ||
         std::is_same_v<R, vacps::runtime::PromiseCapability>;
}();

template <class T>
inline constexpr bool is_stop_token_v =
    std::is_same_v<std::remove_cvref_t<T>, std::stop_token>;

// ── runtime::Task<T> = awaitable<Result<T>, any_io_executor> ───────

template <class R>
struct async_task_traits : std::false_type {};

template <class T>
struct async_task_traits<boost::asio::awaitable<
    vacps::runtime::Result<T>,
    boost::asio::any_io_executor>> : std::true_type {
  using result_type = T;
};

template <class R>
inline constexpr bool is_async_task_v =
    async_task_traits<std::remove_cvref_t<R>>::value;

template <class R>
using async_task_result_t =
    typename async_task_traits<std::remove_cvref_t<R>>::result_type;

/** Safe result_type access when R may not be a Task (avoids hard errors). */
template <class R, bool = is_async_task_v<R>>
struct async_task_result_or_dummy {
  using type = void;
};

template <class R>
struct async_task_result_or_dummy<R, true> {
  using type = async_task_result_t<R>;
};

// ── unique call operator (reject generic / overloaded lambdas) ─────

template <class F>
concept has_unique_call_operator = requires {
  &std::remove_cvref_t<F>::operator();
};

// ── strip leading stop_token from args tuple ───────────────────────

template <class Tuple>
struct strip_leading_stop;

template <>
struct strip_leading_stop<std::tuple<>> {
  using type = std::tuple<>;
  static constexpr bool has_stop = false;
};

template <class A0, class... Rest>
struct strip_leading_stop<std::tuple<A0, Rest...>> {
  static constexpr bool has_stop = is_stop_token_v<A0>;
  using type = std::conditional_t<
      has_stop,
      std::tuple<Rest...>,
      std::tuple<A0, Rest...>>;
};

template <class Tuple>
using strip_leading_stop_t = typename strip_leading_stop<Tuple>::type;

template <class Tuple>
struct decay_tuple;

template <class... Args>
struct decay_tuple<std::tuple<Args...>> {
  using type = std::tuple<std::decay_t<Args>...>;
};

template <class Tuple>
using decay_tuple_t = typename decay_tuple<Tuple>::type;

// ── no stop_token after position 0 ─────────────────────────────────

template <class Tuple>
struct stop_token_only_leading : std::true_type {};

template <class A0, class... Rest>
struct stop_token_only_leading<std::tuple<A0, Rest...>>
    : std::bool_constant<(!is_stop_token_v<Rest> && ...)> {};

// ── decoded parameter reference categories ─────────────────────────
// By-value and const lvalue references are permitted. Non-const lvalue and
// rvalue reference parameters are rejected (lifetime across suspension).

template <class T>
inline constexpr bool is_permitted_async_param_v =
    !std::is_rvalue_reference_v<T> &&
    (!std::is_lvalue_reference_v<T> ||
     std::is_const_v<std::remove_reference_t<T>>);

template <class Tuple>
struct tuple_async_params_ref_ok;

template <class... Args>
struct tuple_async_params_ref_ok<std::tuple<Args...>>
    : std::bool_constant<(is_permitted_async_param_v<Args> && ... && true)> {};

template <class Tuple>
inline constexpr bool tuple_async_params_ref_ok_v =
    tuple_async_params_ref_ok<Tuple>::value;

// ── no JS-confined types in a parameter pack / tuple ───────────────

template <class Tuple>
struct tuple_has_js_confined;

template <class... Args>
struct tuple_has_js_confined<std::tuple<Args...>>
    : std::bool_constant<(is_async_js_confined_v<Args> || ... || false)> {};

template <class Tuple>
inline constexpr bool tuple_has_js_confined_v =
    tuple_has_js_confined<Tuple>::value;

// ── strip leading self parameter for methods ───────────────────────

template <class ClassT, class Tuple>
struct strip_leading_self;

template <class ClassT>
struct strip_leading_self<ClassT, std::tuple<>> {
  static constexpr bool has_self = false;
  using self_param = void;
  using type = std::tuple<>;
};

template <class ClassT, class A0, class... Rest>
struct strip_leading_self<ClassT, std::tuple<A0, Rest...>> {
  // Loose identity match (sync is_self_param_v); category is tightened below.
  static constexpr bool has_self = is_self_param_v<A0, ClassT>;
  using self_param = A0;
  using type = std::conditional_t<
      has_self,
      std::tuple<Rest...>,
      std::tuple<A0, Rest...>>;
};

/**
 * Async method Self category rules (stricter than sync is_self_param_v):
 *   allow: T&, const T&, T*, const T*, shared_ptr<T> by value,
 *          const shared_ptr<T> by value, const shared_ptr<T>&
 *   reject: T&& / const T&&, T by value, non-const shared_ptr<T>&,
 *           shared_ptr<T>&&, pointer/reference combinations, etc.
 */
template <class Param, class ClassT>
inline constexpr bool is_permitted_async_self_v = [] {
  using P = Param;
  using Raw = std::remove_reference_t<P>;
  using Decayed = std::remove_cvref_t<P>;

  if constexpr (std::is_void_v<Decayed>) {
    return false;
  } else if constexpr (std::is_rvalue_reference_v<P>) {
    // T&&, const T&&, shared_ptr<T>&&, const shared_ptr<T>&&, ...
    return false;
  } else if constexpr (std::is_lvalue_reference_v<P> &&
                       std::is_same_v<Decayed, ClassT>) {
    // T& or const T&
    return true;
  } else if constexpr (!std::is_reference_v<P> &&
                       (std::is_same_v<Decayed, ClassT*> ||
                        std::is_same_v<Decayed, const ClassT*>)) {
    // T* or const T* by value
    return true;
  } else if constexpr (is_shared_ptr_v<Decayed>) {
    // Nested: Decayed::element_type is only valid for shared_ptr.
    if constexpr (std::is_same_v<typename Decayed::element_type, ClassT>) {
      if constexpr (!std::is_reference_v<P>) {
        // shared_ptr<T> or const shared_ptr<T> by value
        return true;
      } else if constexpr (std::is_const_v<Raw>) {
        // const shared_ptr<T>&
        return true;
      } else {
        // non-const shared_ptr<T>&
        return false;
      }
    } else {
      // shared_ptr<U> for U != ClassT
      return false;
    }
  } else {
    // T by value, unrelated types, T*&, etc.
    return false;
  }
}();

// ── shared Task / confinement checks ───────────────────────────────

template <class FnDecay>
struct async_callable_base {
  static_assert(
      !std::is_class_v<FnDecay> || has_unique_call_operator<FnDecay>,
      "async binding: generic or overloaded lambdas / callables are not "
      "supported; use a single non-templated operator() returning "
      "runtime::Task<T> (boost::asio::awaitable<runtime::Result<T>, "
      "boost::asio::any_io_executor>)");

  using raw = callable_traits<FnDecay>;
  using return_type = typename raw::return_type;
  using args_tuple = typename raw::args_tuple;

  static_assert(
      is_async_task_v<return_type>,
      "async binding: callable must return runtime::Task<T> "
      "(boost::asio::awaitable<runtime::Result<T>, "
      "boost::asio::any_io_executor>), including T = void; other awaitables "
      "or executor types are not supported");

  using result_type =
      typename async_task_result_or_dummy<return_type>::type;

  static_assert(
      !is_async_js_confined_v<result_type>,
      "async binding: result type must not be JS-thread-confined "
      "(no JSValue/JSValueConst/JSContext*/JSRuntime*/"
      "vacps::qjs::OwnedValue/vacps::qjs::ScopedCString/Env/"
      "CallbackInfo/ValueRef/PromiseCapability)");

  static_assert(
      stop_token_only_leading<args_tuple>::value,
      "async binding: std::stop_token is only legal as the first parameter");
};

// ── free / static async callable traits ────────────────────────────

template <class Fn>
struct async_callable_traits : async_callable_base<std::remove_cvref_t<Fn>> {
  using base = async_callable_base<std::remove_cvref_t<Fn>>;
  using result_type = typename base::result_type;
  using args_tuple = typename base::args_tuple;

  using strip = strip_leading_stop<args_tuple>;
  static constexpr bool has_stop_token = strip::has_stop;
  /** Undecayed parameter types after optional leading stop_token. */
  using raw_decoded_tuple = typename strip::type;
  /** Value types captured by Start (matches decode_args decay). */
  using decoded_tuple = decay_tuple_t<raw_decoded_tuple>;

  static_assert(
      tuple_async_params_ref_ok_v<raw_decoded_tuple>,
      "async binding: parameters must be by-value or const lvalue reference; "
      "non-const lvalue references and rvalue references are not supported "
      "(coroutine parameter lifetime across suspension is too subtle; "
      "take move-only types by value)");

  static_assert(
      !tuple_has_js_confined_v<raw_decoded_tuple>,
      "async binding: parameters retained across suspension must not be "
      "JS-thread-confined (no JSValue/JSValueConst/JSContext*/JSRuntime*/"
      "vacps::qjs::OwnedValue/vacps::qjs::ScopedCString/Env/"
      "CallbackInfo/ValueRef/PromiseCapability). Decode JS args into pure C++ "
      "values before await.");

  static constexpr std::size_t decoded_arity =
      std::tuple_size_v<decoded_tuple>;
};

template <class Fn>
inline constexpr bool is_async_free_callable_v = requires {
  typename async_callable_traits<std::remove_cvref_t<Fn>>::result_type;
};

// ── ClassBuilder method async callable traits ──────────────────────

template <class ClassT, class Fn>
struct async_method_traits : async_callable_base<std::remove_cvref_t<Fn>> {
  using base = async_callable_base<std::remove_cvref_t<Fn>>;
  using result_type = typename base::result_type;
  using args_tuple = typename base::args_tuple;

  using stop_strip = strip_leading_stop<args_tuple>;
  static constexpr bool has_stop_token = stop_strip::has_stop;
  using after_stop = typename stop_strip::type;

  using self_strip = strip_leading_self<ClassT, after_stop>;
  static_assert(
      self_strip::has_self,
      "async_method: after optional stop_token, the next parameter must be "
      "T& / const T& / T* / const T* / std::shared_ptr<T> (by value or "
      "const lvalue reference)");

  using self_param = typename self_strip::self_param;
  static_assert(
      is_permitted_async_self_v<self_param, ClassT>,
      "async_method: self parameter must be T&, const T&, T*, const T*, "
      "std::shared_ptr<T> by value, or const std::shared_ptr<T>&; "
      "T&&, T by value, non-const std::shared_ptr<T>&, std::shared_ptr<T>&&, "
      "and other unsupported reference categories are rejected");

  /** Undecayed JS-decoded parameter types (no stop, no self). */
  using raw_decoded_tuple = typename self_strip::type;
  using decoded_tuple = decay_tuple_t<raw_decoded_tuple>;

  // Self may be T& (non-const lvalue) — that is intentional injection, not a
  // decoded JS arg. Only decoded argv params use the ref rules below.
  static_assert(
      tuple_async_params_ref_ok_v<raw_decoded_tuple>,
      "async_method: JS parameters must be by-value or const lvalue "
      "reference; non-const lvalue references and rvalue references are not "
      "supported");

  static_assert(
      !tuple_has_js_confined_v<raw_decoded_tuple>,
      "async_method: parameters retained across suspension must not be "
      "JS-thread-confined. Decode JS args into pure C++ values before await.");

  // Self itself must not introduce extra JS-confined types (shared_ptr/T& OK).
  static_assert(
      !is_async_js_confined_v<self_param>,
      "async_method: self parameter must not be JS-thread-confined");

  static constexpr std::size_t decoded_arity =
      std::tuple_size_v<decoded_tuple>;
};

template <class ClassT, class Fn>
inline constexpr bool is_async_method_callable_v = requires {
  typename async_method_traits<ClassT, std::remove_cvref_t<Fn>>::result_type;
};

/**
 * True when Encode is intended as a custom async result encoder rather than
 * a JS function.length integer override.
 */
template <class Encode>
inline constexpr bool is_async_encode_arg_v =
    !std::is_convertible_v<std::decay_t<Encode>, int>;

}  // namespace vacps::binding::detail
