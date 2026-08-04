#pragma once

/**
 * Argument decode + return encode for free functions and class methods.
 *
 * Free function forms:
 *   f()
 *   f(CallbackInfo)
 *   f(CallbackInfo, Args...)
 *   f(Env, Args...)
 *   f(Args...)                      // argv[0..]
 *
 * Method forms (ClassBuilder):
 *   m(T&) / m(T*) / m(std::shared_ptr<T>)
 *   m(T&, Args...)
 *   m(CallbackInfo)                 // body unwraps this if needed
 *   m(CallbackInfo, T&, Args...)
 *
 * decode_args constructs the result tuple compositionally (via optional slots)
 * so Converter targets need not be default-constructible. Move-only decoded
 * types are supported.
 */

#include "binding/callback_info.hpp"
#include "binding/convert.hpp"
#include "binding/detail/concepts.hpp"
#include "binding/error.hpp"
#include "qjs/owned_value.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vacps::binding::detail {

template <class T>
struct is_shared_ptr : std::false_type {};

template <class T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};

template <class T>
inline constexpr bool is_shared_ptr_v =
    is_shared_ptr<std::remove_cvref_t<T>>::value;

template <class T, class Self>
inline constexpr bool is_self_param_v = [] {
  using R = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<R, Self>) {
    return true;
  } else if constexpr (std::is_same_v<R, Self*> ||
                       std::is_same_v<R, const Self*>) {
    return true;
  } else if constexpr (is_shared_ptr_v<R>) {
    return std::is_same_v<typename R::element_type, Self>;
  } else {
    return false;
  }
}();

template <class Self, class Param>
auto bind_self(Self& self, const std::shared_ptr<Self>& held) {
  using R = std::remove_cvref_t<Param>;
  if constexpr (std::is_pointer_v<R>) {
    (void)held;
    return &self;
  } else if constexpr (is_shared_ptr_v<R>) {
    return held;
  } else {
    (void)held;
    return std::ref(self);
  }
}

/**
 * Decode Args... into a tuple without default-constructing elements.
 * Injects CallbackInfo / Env; remaining parameters consume argv
 * sequentially from argv_base.
 *
 * Missing trailing JS arguments are decoded as borrowed JS_UNDEFINED via
 * CallbackInfo::arg (no blanket argc precheck). Converters decide whether
 * undefined is acceptable. Callers that require a hard arity floor use
 * CallbackInfo::check_argc explicitly.
 */
template <class... Args>
Result<std::tuple<std::decay_t<Args>...>> decode_args(
    const CallbackInfo& info,
    int argv_base) {
  if constexpr (sizeof...(Args) == 0) {
    return std::tuple<>{};
  } else {
    std::tuple<std::optional<std::decay_t<Args>>...> slots{};
    Error err;
    bool failed = false;
    int argv_i = argv_base;

    auto fill = [&]<std::size_t I>() {
      if (failed) {
        return;
      }
      using Raw = std::decay_t<std::tuple_element_t<I, std::tuple<Args...>>>;
      if constexpr (is_callback_info_v<Raw>) {
        std::get<I>(slots).emplace(info);
      } else if constexpr (is_env_v<Raw>) {
        std::get<I>(slots).emplace(info.env());
      } else {
        auto r = info.arg<Raw>(argv_i);
        ++argv_i;
        if (!r) {
          err = std::move(r.error());
          failed = true;
          return;
        }
        std::get<I>(slots).emplace(std::move(*r));
      }
    };

    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (fill.template operator()<I>(), ...);
    }(std::index_sequence_for<Args...>{});

    if (failed) {
      return std::unexpected(std::move(err));
    }

    return std::apply(
        [](auto&&... opt) -> std::tuple<std::decay_t<Args>...> {
          return std::tuple<std::decay_t<Args>...>{std::move(*opt)...};
        },
        std::move(slots));
  }
}

template <class Fn, class Tuple, std::size_t... I>
qjs::OwnedValue apply_encoded(
    Env env,
    Fn&& fn,
    Tuple&& tup,
    std::index_sequence<I...>) {
  return invoke_and_encode(
      env, std::forward<Fn>(fn), std::get<I>(std::forward<Tuple>(tup))...);
}

template <class Fn>
qjs::OwnedValue dispatch_free(const CallbackInfo& info, Fn&& fn) {
  using Traits = callable_traits<std::remove_cvref_t<Fn>>;
  using ArgsTuple = typename Traits::args_tuple;

  return [&]<std::size_t... I>(std::index_sequence<I...>) -> qjs::OwnedValue {
    if constexpr (sizeof...(I) == 0) {
      return invoke_and_encode(info.env(), std::forward<Fn>(fn));
    } else {
      auto decoded =
          decode_args<std::tuple_element_t<I, ArgsTuple>...>(info, 0);
      if (!decoded) {
        return qjs::OwnedValue::take(
            info.context(), throw_error(info.context(), decoded.error()));
      }
      return apply_encoded(
          info.env(),
          std::forward<Fn>(fn),
          std::move(*decoded),
          std::index_sequence_for<std::tuple_element_t<I, ArgsTuple>...>{});
    }
  }(std::make_index_sequence<Traits::arity>{});
}

template <class T, class Fn>
qjs::OwnedValue dispatch_method(
    const CallbackInfo& info,
    T& self,
    const std::shared_ptr<T>& held,
    Fn&& fn) {
  using Traits = callable_traits<std::remove_cvref_t<Fn>>;
  using ArgsTuple = typename Traits::args_tuple;

  if constexpr (Traits::arity == 0) {
    return invoke_and_encode(info.env(), std::forward<Fn>(fn));
  } else {
    using Arg0 = std::tuple_element_t<0, ArgsTuple>;
    using Arg0Raw = std::remove_cvref_t<Arg0>;

    if constexpr (is_callback_info_v<Arg0Raw>) {
      // (CallbackInfo, ...)
      if constexpr (Traits::arity == 1) {
        return invoke_and_encode(info.env(), std::forward<Fn>(fn), info);
      } else {
        using Arg1 = std::tuple_element_t<1, ArgsTuple>;
        using Arg1Raw = std::remove_cvref_t<Arg1>;
        if constexpr (is_self_param_v<Arg1Raw, T>) {
          // (CallbackInfo, Self, Args...)
          return [&]<std::size_t... I>(
                     std::index_sequence<I...>) -> qjs::OwnedValue {
            if constexpr (sizeof...(I) == 0) {
              auto s = bind_self<T, Arg1>(self, held);
              return invoke_and_encode(
                  info.env(), std::forward<Fn>(fn), info, s);
            } else {
              auto decoded =
                  decode_args<std::tuple_element_t<I + 2, ArgsTuple>...>(
                      info, 0);
              if (!decoded) {
                return qjs::OwnedValue::take(
                    info.context(),
                    throw_error(info.context(), decoded.error()));
              }
              auto s = bind_self<T, Arg1>(self, held);
              return apply_encoded(
                  info.env(),
                  [&](auto&&... rest) {
                    return std::invoke(
                        std::forward<Fn>(fn),
                        info,
                        s,
                        std::forward<decltype(rest)>(rest)...);
                  },
                  std::move(*decoded),
                  std::make_index_sequence<sizeof...(I)>{});
            }
          }(std::make_index_sequence<Traits::arity - 2>{});
        } else {
          // (CallbackInfo, Args...) no self
          return [&]<std::size_t... I>(
                     std::index_sequence<I...>) -> qjs::OwnedValue {
            auto decoded =
                decode_args<std::tuple_element_t<I, ArgsTuple>...>(info, 0);
            if (!decoded) {
              return qjs::OwnedValue::take(
                  info.context(),
                  throw_error(info.context(), decoded.error()));
            }
            return apply_encoded(
                info.env(),
                std::forward<Fn>(fn),
                std::move(*decoded),
                std::index_sequence_for<
                    std::tuple_element_t<I, ArgsTuple>...>{});
          }(std::make_index_sequence<Traits::arity>{});
        }
      }
    } else if constexpr (is_self_param_v<Arg0Raw, T>) {
      // (Self, Args...)
      if constexpr (Traits::arity == 1) {
        auto s = bind_self<T, Arg0>(self, held);
        return invoke_and_encode(info.env(), std::forward<Fn>(fn), s);
      } else {
        return [&]<std::size_t... I>(std::index_sequence<I...>) -> qjs::OwnedValue {
          auto decoded =
              decode_args<std::tuple_element_t<I + 1, ArgsTuple>...>(info, 0);
          if (!decoded) {
            return qjs::OwnedValue::take(
                info.context(), throw_error(info.context(), decoded.error()));
          }
          auto s = bind_self<T, Arg0>(self, held);
          return apply_encoded(
              info.env(),
              [&](auto&&... rest) {
                return std::invoke(
                    std::forward<Fn>(fn),
                    s,
                    std::forward<decltype(rest)>(rest)...);
              },
              std::move(*decoded),
              std::make_index_sequence<Traits::arity - 1>{});
        }(std::make_index_sequence<Traits::arity - 1>{});
      }
    } else {
      static_assert(
          is_callback_info_v<Arg0Raw> || is_self_param_v<Arg0Raw, T>,
          "Class method first parameter must be T&/T*/shared_ptr<T> or "
          "CallbackInfo");
      return info.env().undefined();
    }
  }
}

}  // namespace vacps::binding::detail
