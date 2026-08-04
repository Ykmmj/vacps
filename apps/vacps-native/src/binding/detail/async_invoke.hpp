#pragma once

/**
 * Decode + Runtime::Async::promise dispatch for async free functions,
 * static async functions, and ClassBuilder async methods.
 *
 * Registration requires a non-null Runtime::Async* (host/programmer misuse
 * otherwise). Call-entry (synchronous, no Promise yet):
 * - bad this (methods) → throw
 * - JS argument decode failure → throw
 * No per-callback is_accepting / phase gate (Narrow composition contract).
 *
 * After Promise creation:
 * - start / coroutine / encode failures reject through Runtime::Async only.
 * - Binding never settles PromiseCapability directly.
 * - No JSContext/JSValue/qjs::OwnedValue may be parked across suspension.
 *
 * Default encode uses Converter<T> on the owner thread (class-aware wrap for
 * shared_ptr<ClassT> / ClassT when a class_name is supplied). Custom Encode
 * callables matching runtime::JsEncode replace the default path.
 *
 * Move-only user callables and custom encoders are stored in shared_ptr so
 * in-flight Starts survive JS function finalization and repeated calls.
 *
 * Method ownership is frame-local: async_call_method_task(_stop) parameters
 * include a non-null shared_ptr<ClassT>; Self (T& / T* / shared_ptr) is bound
 * inside the coroutine from that owner. Do not rely on Runtime::Async retaining
 * Start after it has produced the Task.
 *
 * Named coroutine helpers only — no immediately-invoked coroutine lambdas.
 */

#include "binding/callback_info.hpp"
#include "binding/convert.hpp"
#include "binding/detail/async_traits.hpp"
#include "binding/detail/class_storage.hpp"
#include "binding/detail/invoke.hpp"
#include "binding/detail/native_slot.hpp"
#include "binding/error.hpp"
#include "qjs/owned_value.hpp"
#include "runtime/error.hpp"
#include "runtime/js_encode.hpp"
#include "runtime/runtime_async.hpp"

#include <quickjs.h>

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vacps::binding::detail {

// ── qjs::OwnedValue helpers ─────────────────────────────────────────────

/**
 * Adopt Converter / wrap output into a runtime encode Result.
 * Clears any pending JS exception so Runtime::Async can reject exactly once.
 */
inline vacps::runtime::Result<vacps::qjs::OwnedValue> adopt_encoded_owned(
    JSContext* ctx,
    qjs::OwnedValue owned) {
  if (owned.is_exception()) {
    clear_exception(ctx);
    (void)owned.release();
    return std::unexpected(
        vacps::runtime::Error::native("failed to encode async result"));
  }
  JSValue raw = owned.release();
  return vacps::qjs::OwnedValue{ctx, raw};
}

// ── default Converter encode T → Result<qjs::OwnedValue> ───────────────

/** Placeholder Encode for Task<void> (promise_void ignores it). */
struct void_encode_unused {};

template <class T>
struct async_result_encode {
  vacps::runtime::Result<vacps::qjs::OwnedValue> operator()(
      JSContext* ctx,
      T&& value) const {
    Env env{ctx};
    qjs::OwnedValue owned =
        Converter<std::remove_cvref_t<T>>::to_js(env, std::forward<T>(value));
    return adopt_encoded_owned(ctx, std::move(owned));
  }

  vacps::runtime::Result<vacps::qjs::OwnedValue> operator()(
      JSContext* ctx,
      const T& value) const {
    Env env{ctx};
    qjs::OwnedValue owned = Converter<std::remove_cvref_t<T>>::to_js(env, value);
    return adopt_encoded_owned(ctx, std::move(owned));
  }
};

/**
 * ClassBuilder-aware default encoder:
 * - shared_ptr<ClassT> / ClassT → ClassStorage::wrap under class_name
 * - everything else → Converter via async_result_encode
 *
 * class_name is owned by value so the encoder outlives ClassBuilder.
 */
template <class ClassT>
struct class_async_result_encode {
  std::string class_name;

  template <class R>
  vacps::runtime::Result<vacps::qjs::OwnedValue> operator()(
      JSContext* ctx,
      R&& value) const {
    using Raw = std::remove_cvref_t<R>;
    if constexpr (std::is_same_v<Raw, std::shared_ptr<ClassT>>) {
      qjs::OwnedValue owned = ClassStorage<ClassT>::wrap(
          ctx, class_name.c_str(), std::forward<R>(value));
      return adopt_encoded_owned(ctx, std::move(owned));
    } else if constexpr (std::is_same_v<Raw, ClassT>) {
      auto held = std::make_shared<ClassT>(std::forward<R>(value));
      qjs::OwnedValue owned = ClassStorage<ClassT>::wrap(
          ctx, class_name.c_str(), std::move(held));
      return adopt_encoded_owned(ctx, std::move(owned));
    } else {
      return async_result_encode<Raw>{}(ctx, std::forward<R>(value));
    }
  }
};

/**
 * shared_ptr-backed Encode wrapper so custom (possibly move-only) encoders
 * stay alive across repeated JS calls and after JS function finalization of
 * in-flight work that already captured this wrapper by value.
 *
 * Contract enforcement for custom Encode: a reported Result error must leave
 * no pending QuickJS exception, and success must return a matching-context
 * OwnedValue. Contract violations fail fast; unexpected throws flow to
 * Runtime::Async's single coroutine completion boundary.
 */
template <class Encode>
struct shared_async_encode {
  std::shared_ptr<Encode> encode;

  template <class V>
  vacps::runtime::Result<vacps::qjs::OwnedValue> operator()(
      JSContext* ctx,
      V&& value) const {
    auto encoded = std::invoke(*encode, ctx, std::forward<V>(value));
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    return adopt_encoded_owned(ctx, std::move(*encoded));
  }
};

// ── named coroutine invoke helpers (not IIFE coroutine lambdas) ────

template <class T, class Fn, class... Args>
vacps::runtime::Task<T> async_call_task(
    std::shared_ptr<Fn> fn,
    Args... args) {
  co_return co_await std::invoke(*fn, std::move(args)...);
}

template <class T, class Fn, class... Args>
vacps::runtime::Task<T> async_call_task_stop(
    std::shared_ptr<Fn> fn,
    std::stop_token stop,
    Args... args) {
  co_return co_await std::invoke(*fn, stop, std::move(args)...);
}

/**
 * Method coroutine helpers. Ownership of ClassT is frame-local: the
 * non-null shared_ptr<ClassT> is a coroutine parameter (lives in the
 * awaitable frame). Self is bound from that owner inside the coroutine so
 * T& / T* remain valid across suspension without relying on Runtime::Async
 * retaining the Start object after it has produced the Task.
 */
template <class T, class ClassT, class Fn, class SelfParam, class... Args>
vacps::runtime::Task<T> async_call_method_task(
    std::shared_ptr<Fn> fn,
    std::shared_ptr<ClassT> owner,
    Args... args) {
  // owner + bound are coroutine frame locals and outlive the inner Task.
  // Pass bound as an lvalue so const shared_ptr<T>& Self does not bind to a
  // temporary (coroutine parameter reference lifetime hazard). T& / T*
  // alias *owner; shared_ptr-by-value Self copies from bound into the user
  // frame.
  auto bound = bind_self<ClassT, SelfParam>(*owner, owner);
  co_return co_await std::invoke(*fn, bound, std::move(args)...);
}

template <class T, class ClassT, class Fn, class SelfParam, class... Args>
vacps::runtime::Task<T> async_call_method_task_stop(
    std::shared_ptr<Fn> fn,
    std::stop_token stop,
    std::shared_ptr<ClassT> owner,
    Args... args) {
  auto bound = bind_self<ClassT, SelfParam>(*owner, owner);
  co_return co_await std::invoke(*fn, stop, bound, std::move(args)...);
}

template <class T, class Fn, class Tuple, bool WithStop>
struct async_start {
  std::shared_ptr<Fn> fn;
  Tuple args;

  auto operator()(std::stop_token stop) {
    return std::apply(
        [&](auto&... packed) {
          if constexpr (WithStop) {
            return async_call_task_stop<
                T,
                Fn,
                std::decay_t<decltype(packed)>...>(
                fn, stop, std::move(packed)...);
          } else {
            (void)stop;
            return async_call_task<T, Fn, std::decay_t<decltype(packed)>...>(
                fn, std::move(packed)...);
          }
        },
        args);
  }
};

/**
 * Method Start: moves shared_ptr<ClassT> into the method coroutine frame.
 * Frame-local ownership (not Start retention by Runtime::Async) keeps T& / T*
 * self valid across await after JS wrapper GC.
 */
template <
    class ClassT,
    class R,
    class Fn,
    class SelfParam,
    class Tuple,
    bool WithStop>
struct async_method_start {
  std::shared_ptr<Fn> fn;
  std::shared_ptr<ClassT> self;
  Tuple args;

  auto operator()(std::stop_token stop) {
    // owner must be non-null: unwrap_this always yields a live shared_ptr.
    return std::apply(
        [&](auto&... packed) {
          if constexpr (WithStop) {
            return async_call_method_task_stop<
                R,
                ClassT,
                Fn,
                SelfParam,
                std::decay_t<decltype(packed)>...>(
                fn, stop, std::move(self), std::move(packed)...);
          } else {
            (void)stop;
            return async_call_method_task<
                R,
                ClassT,
                Fn,
                SelfParam,
                std::decay_t<decltype(packed)>...>(
                fn, std::move(self), std::move(packed)...);
          }
        },
        args);
  }
};

// ── decode argv into pure C++ values (no Env/CallbackInfo injection) ────

template <class Tuple>
auto decode_async_tuple(const CallbackInfo& info) -> Result<Tuple> {
  return [&]<std::size_t... I>(std::index_sequence<I...>) -> Result<Tuple> {
    if constexpr (sizeof...(I) == 0) {
      return Tuple{};
    } else {
      // Traits reject Env/CallbackInfo; every element is from argv.
      return decode_args<std::tuple_element_t<I, Tuple>...>(info, 0);
    }
  }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

// ── settle via Runtime::Async with default or custom encode ──────────

template <class T, class Start, class Encode>
JSValue promise_with_encode(
    vacps::Runtime::Async* ra,
    JSContext* jc,
    Start&& start,
    Encode&& encode) {
  if constexpr (std::is_void_v<T>) {
    (void)encode;
    return ra->promise_void(jc, std::forward<Start>(start));
  } else {
    return ra->template promise<T>(
        jc, std::forward<Start>(start), std::forward<Encode>(encode));
  }
}

// ── top-level JS entry for one async free / static function ────────

template <class Fn, class Encode>
JSValue dispatch_async_free(
    JSContext* jc,
    vacps::Runtime::Async* async,
    const std::shared_ptr<Fn>& held,
    Encode encode,
    JSValueConst /*this_val*/,
    int argc,
    JSValueConst* argv) {
  using Traits = async_callable_traits<Fn>;
  using T = typename Traits::result_type;
  using Decoded = typename Traits::decoded_tuple;
  constexpr bool k_stop = Traits::has_stop_token;

  Env env{jc, async};
  CallbackInfo info{env, JS_UNDEFINED, argc, argv};

  auto decoded = decode_async_tuple<Decoded>(info);
  if (!decoded) {
    return throw_error(jc, decoded.error());
  }

  async_start<T, Fn, Decoded, k_stop> start{held, std::move(*decoded)};
  return promise_with_encode<T>(
      async, jc, std::move(start), std::move(encode));
}

// ── top-level JS entry for one async instance method ───────────────

template <class ClassT, class Fn, class Encode>
JSValue dispatch_async_method(
    JSContext* jc,
    vacps::Runtime::Async* async,
    const std::shared_ptr<Fn>& held,
    Encode encode,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  using Traits = async_method_traits<ClassT, Fn>;
  using R = typename Traits::result_type;
  using Decoded = typename Traits::decoded_tuple;
  using SelfParam = typename Traits::self_param;
  constexpr bool k_stop = Traits::has_stop_token;

  auto unwrapped = unwrap_this<ClassT>(jc, this_val);
  if (!unwrapped) {
    // Wrong `this` class remains a Wide JavaScript boundary error.
    return throw_error(jc, unwrapped.error());
  }
  // Hand to Start → method coroutine frame (survives JS wrapper GC).
  std::shared_ptr<ClassT> self = std::move(unwrapped->second);

  Env env{jc, async};
  CallbackInfo info{env, this_val, argc, argv};

  auto decoded = decode_async_tuple<Decoded>(info);
  if (!decoded) {
    return throw_error(jc, decoded.error());
  }

  async_method_start<ClassT, R, Fn, SelfParam, Decoded, k_stop> start{
      held, std::move(self), std::move(*decoded)};
  return promise_with_encode<R>(
      async, jc, std::move(start), std::move(encode));
}

// ── JS length resolution (decoded arity; stop/self not counted) ────

inline int resolve_js_length(int length, std::size_t decoded_arity) {
  if (length == -1) {
    constexpr auto k_int_max =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(
        decoded_arity > k_int_max ? k_int_max : decoded_arity);
  }
  if (length < 0) {
    return 0;
  }
  return length;
}

/**
 * Build a Promise-returning free/static JS function.
 * Encode is moved into a shared_ptr so repeated calls and in-flight work are
 * safe even when Encode is move-only or captures state.
 */
template <class Fn, class Encode>
[[nodiscard]] qjs::OwnedValue make_async_free_function(
    Env ctx,
    const char* name,
    Fn fn,
    Encode encode,
    int length) {
  using F = std::decay_t<Fn>;
  using E = std::decay_t<Encode>;
  using Traits = async_callable_traits<F>;
  constexpr std::size_t decoded_arity = Traits::decoded_arity;
  // Custom Encode is only meaningful for non-void results; callers must
  // static_assert that before selecting this path. void_encode_unused is the
  // sole Encode accepted for Task<void>.
  if constexpr (std::is_void_v<typename Traits::result_type>) {
    static_assert(
        std::is_same_v<E, void_encode_unused>,
        "async binding: custom Encode is not supported for Task<void> "
        "(void results always resolve to undefined)");
  } else {
    static_assert(
        vacps::runtime::JsEncode<E, typename Traits::result_type>,
        "async binding: Encode must satisfy runtime::JsEncode for the "
        "callable result type");
  }

  const int function_length = resolve_js_length(length, decoded_arity);

  JSContext* c = ctx.context();
  vacps::Runtime::Async* async = ctx.async();

  auto held_fn = std::make_shared<F>(std::move(fn));
  auto held_enc = std::make_shared<E>(std::move(encode));

  auto slot = std::make_unique<NativeSlot>();
  slot->call =
      [held_fn, held_enc, async](
          JSContext* jc,
          JSValueConst this_val,
          int argc,
          JSValueConst* argv) mutable -> JSValue {
        shared_async_encode<E> enc{held_enc};
        return dispatch_async_free<F>(
            jc, async, held_fn, std::move(enc), this_val, argc, argv);
      };

  qjs::OwnedValue func =
      make_cfunction_data(c, function_length, std::move(slot));
  if (!func.is_exception() && name != nullptr) {
    set_function_name(c, func.get(), name);
  }
  return func;
}

/**
 * Build a Promise-returning instance method JS function for ClassT.
 * Unwraps this, retains shared_ptr<ClassT> across the coroutine, and uses
 * Encode for owner-thread result conversion.
 */
template <class ClassT, class Fn, class Encode>
[[nodiscard]] qjs::OwnedValue make_async_method_function(
    Env ctx,
    const char* name,
    Fn fn,
    Encode encode,
    int length) {
  using F = std::decay_t<Fn>;
  using E = std::decay_t<Encode>;
  using Traits = async_method_traits<ClassT, F>;
  constexpr std::size_t decoded_arity = Traits::decoded_arity;
  if constexpr (std::is_void_v<typename Traits::result_type>) {
    static_assert(
        std::is_same_v<E, void_encode_unused>,
        "async_method: custom Encode is not supported for Task<void> "
        "(void results always resolve to undefined)");
  } else {
    static_assert(
        vacps::runtime::JsEncode<E, typename Traits::result_type>,
        "async_method: Encode must satisfy runtime::JsEncode for the "
        "callable result type");
  }

  const int function_length = resolve_js_length(length, decoded_arity);

  JSContext* c = ctx.context();
  vacps::Runtime::Async* async = ctx.async();

  auto held_fn = std::make_shared<F>(std::move(fn));
  auto held_enc = std::make_shared<E>(std::move(encode));

  auto slot = std::make_unique<NativeSlot>();
  slot->call =
      [held_fn, held_enc, async](
          JSContext* jc,
          JSValueConst this_val,
          int argc,
          JSValueConst* argv) mutable -> JSValue {
        shared_async_encode<E> enc{held_enc};
        return dispatch_async_method<ClassT, F>(
            jc, async, held_fn, std::move(enc), this_val, argc, argv);
      };

  qjs::OwnedValue func =
      make_cfunction_data(c, function_length, std::move(slot));
  if (!func.is_exception() && name != nullptr) {
    set_function_name(c, func.get(), name);
  }
  return func;
}

}  // namespace vacps::binding::detail
