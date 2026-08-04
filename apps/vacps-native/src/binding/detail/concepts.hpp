#pragma once

/**
 * Binding concepts and callable traits (phase-1).
 */

#include "app/error.hpp"
#include "binding/error.hpp"
#include "runtime/error.hpp"

#include <concepts>
#include <cstddef>
#include <expected>
#include <tuple>
#include <type_traits>

namespace vacps::binding {
class CallbackInfo;
class Env;
}  // namespace vacps::binding

namespace vacps::binding::detail {

template <class T>
concept Decayed = std::same_as<T, std::remove_cvref_t<T>>;

template <class T>
struct is_result : std::false_type {};

template <class T>
struct is_result<Result<T>> : std::true_type {};

template <class T>
struct is_result<vacps::runtime::Result<T>> : std::true_type {};

template <class T>
struct is_result<vacps::Result<T>> : std::true_type {};

template <class T>
inline constexpr bool is_result_v = is_result<std::remove_cvref_t<T>>::value;

template <class T>
struct result_value;

template <class T>
struct result_value<Result<T>> {
  using type = T;
};

template <class T>
struct result_value<vacps::runtime::Result<T>> {
  using type = T;
};

template <class T>
struct result_value<vacps::Result<T>> {
  using type = T;
};

template <class T>
using result_value_t = typename result_value<std::remove_cvref_t<T>>::type;

template <class T>
inline constexpr bool is_callback_info_v =
    std::is_same_v<std::remove_cvref_t<T>, CallbackInfo>;

template <class T>
inline constexpr bool is_env_v =
    std::is_same_v<std::remove_cvref_t<T>, Env>;

// ── callable traits ────────────────────────────────────────────────

template <class F>
struct callable_traits;

template <class R, class... Args>
struct callable_traits<R (*)(Args...)> {
  using return_type = R;
  using args_tuple = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
  static constexpr bool is_member = false;
};

template <class R, class... Args>
struct callable_traits<R (*)(Args...) noexcept>
    : callable_traits<R (*)(Args...)> {};

template <class R, class C, class... Args>
struct callable_traits<R (C::*)(Args...)> {
  using return_type = R;
  using args_tuple = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
  static constexpr bool is_member = true;
  using class_type = C;
};

template <class R, class C, class... Args>
struct callable_traits<R (C::*)(Args...) const>
    : callable_traits<R (C::*)(Args...)> {};

template <class R, class C, class... Args>
struct callable_traits<R (C::*)(Args...) noexcept>
    : callable_traits<R (C::*)(Args...)> {};

template <class R, class C, class... Args>
struct callable_traits<R (C::*)(Args...) const noexcept>
    : callable_traits<R (C::*)(Args...)> {};

template <class F>
struct callable_traits {
 private:
  using op = decltype(&std::remove_cvref_t<F>::operator());
  using base = callable_traits<op>;

 public:
  using return_type = typename base::return_type;
  using args_tuple = typename base::args_tuple;
  static constexpr std::size_t arity = base::arity;
  static constexpr bool is_member = false;
};

template <class Tuple, std::size_t I>
using tuple_elem_t = std::tuple_element_t<I, Tuple>;

}  // namespace vacps::binding::detail
