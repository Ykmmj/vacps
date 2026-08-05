#pragma once

/**
 * ClassBuilder<T> — register a JS class whose opaque is a heap ClassHolder
 * storing std::shared_ptr<T>.
 *
 * - constructor: (CallbackInfo|Args...) → shared_ptr<T> | T | Result<...>
 * - method: see detail/invoke.hpp method forms
 * - async_method / static_async_function: Promise members via Runtime::Async
 *   (see detail/async_traits.hpp / async_invoke.hpp); method Self ownership is
 *   frame-local (shared_ptr<T> in the method coroutine); custom Encode is
 *   rejected for Task<void>
 * - readonly: getter installed via JS_DefinePropertyGetSet (no setter)
 * - accessor: getter+setter via JS_DefinePropertyGetSet (both create_function)
 *
 * Finalizer contract (see detail/class_storage.hpp): deleting ClassHolder drops
 * one shared_ptr<T> and may run ~T / captured state. Those destructors must be
 * non-blocking and must not call QuickJS. Long-lived resources need explicit
 * close/host ownership; async method frames retain shared_ptr<T>. The finalizer
 * does not perform business cleanup or JS beyond deleting the holder.
 *
 * Type/class id checks use JS_GetOpaque2 (TypeError on mismatch → cleared into
 * binding::Error on unwrap Result paths).
 *
 * Class identity: one JSClassID and one canonical class_name per T process-wide.
 * Every ClassBuilder<T> uses the same class_name. This is a Narrow caller
 * precondition and is not dynamically checked.
 *
 * Staging contract:
 * - method/readonly/accessor/static_function/async_method/static_async_function
 *   absorb create_* failures immediately (clear pending JS exception, drop
 *   sentinel, record native Error).
 * - Once a staging error exists, further staging calls do not touch QuickJS.
 * - commit() returns the recorded Error with the engine clean.
 *
 * Constructor semantics (phase-1, intentionally partial):
 * - Implemented via JS_NewCFunctionData + JS_SetConstructorBit.
 * - On [[Construct]], QuickJS passes new_target as this_val to the data
 *   callback. We require JS_IsConstructor(this_val) and reject plain calls
 *   (Counter() without new) with TypeError.
 * - Instance prototype is taken from new_target.prototype when it is an
 *   object (subclass-friendly); otherwise the registered class proto is used.
 * - Not claimed: full JS constructor exotic object semantics, new.target
 *   reflection inside user ctors, or species protocol.
 *
 * commit() returns the constructor function (caller-owned) or unexpected
 * Error with no pending JS exception.
 */

#include "binding/async_function.hpp"
#include "binding/callback_info.hpp"
#include "binding/convert.hpp"
#include "binding/detail/async_invoke.hpp"
#include "binding/detail/async_traits.hpp"
#include "binding/detail/class_storage.hpp"
#include "binding/detail/concepts.hpp"
#include "binding/detail/invoke.hpp"
#include "binding/detail/native_slot.hpp"
#include "binding/env.hpp"
#include "binding/error.hpp"
#include "binding/function.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace vacps::binding {

template <class T>
class ClassBuilder {
 public:
  using Storage = detail::ClassStorage<T>;

  ClassBuilder(Env ctx, const char* class_name)
      : ctx_(ctx), class_name_(class_name) {}

  ClassBuilder(const ClassBuilder&) = delete;
  ClassBuilder& operator=(const ClassBuilder&) = delete;
  ClassBuilder(ClassBuilder&&) noexcept = default;
  ClassBuilder& operator=(ClassBuilder&&) noexcept = default;

  [[nodiscard]] Env env() const noexcept { return ctx_; }
  [[nodiscard]] const char* class_name() const noexcept {
    return class_name_.c_str();
  }
  /** May throw std::system_error if class-id mutex lock fails. */
  [[nodiscard]] static JSClassID class_id() { return Storage::class_id(); }

  /**
   * Constructor body. Supported returns:
   *   std::shared_ptr<T>, T,
   *   binding::Result / vacps::Result / runtime::Result of those.
   * Signature: same as free functions (CallbackInfo and/or Args...).
   * Move-only callables are accepted and stored directly in the final
   * NativeSlot; ClassBuilder does not add an intermediate erased callable.
   */
  template <class Fn>
  ClassBuilder& constructor(Fn fn, int length = 0) {
    if (staging_error_) {
      return *this;
    }
    qjs::OwnedValue ctor =
        make_constructor_function(std::move(fn), length);
    if (ctor.is_exception()) {
      clear_exception(ctx_.context());
      (void)ctor.release();
      staging_error_ =
          Error::internal("ClassBuilder: constructor create failed");
      return *this;
    }
    constructor_ = std::move(ctor);
    return *this;
  }

  template <class Fn>
  ClassBuilder& method(const char* name, Fn fn, int length = 0) {
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    qjs::OwnedValue fn_val = make_method_function(std::move(fn), length, name);
    (void)absorb_method(methods_, entry_name, std::move(fn_val));
    return *this;
  }

  /**
   * Async instance method → Promise. Forms:
   *   (Self, Args...) -> Task<R>
   *   (stop_token, Self, Args...) -> Task<R>
   * Self is T& / const T& / T* / const T* / shared_ptr<T> by value /
   * const shared_ptr<T>&. T&&, T by value, non-const shared_ptr<T>&, and
   * shared_ptr<T>&& are rejected at compile time.
   *
   * Registration requires non-null Runtime::Async*. Call-entry unwraps this
   * and decodes Args before Runtime::Async::promise. A non-null shared_ptr<T> is
   * moved into the method coroutine frame (frame-local ownership; GC-safe
   * for T& / T* across await). Default encode: Converter<R>, with
   * class-aware wrap for shared_ptr<T> / T.
   *
   * @param length JS function.length. Default -1 uses decoded argv arity
   *        (stop_token and Self are not counted).
   */
  template <class Fn>
  ClassBuilder& async_method(const char* name, Fn fn, int length = -1) {
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    qjs::OwnedValue fn_val =
        make_async_method_function(std::move(fn), length, name);
    (void)absorb_method(methods_, entry_name, std::move(fn_val));
    return *this;
  }

  /**
   * async_method with a custom owner-thread Encode (runtime::JsEncode).
   * Encode is distinguished from length by not being convertible to int.
   * Not available for Task<void> (always resolves undefined).
   */
  template <class Fn, class Encode>
    requires detail::is_async_encode_arg_v<Encode>
  ClassBuilder& async_method(
      const char* name,
      Fn fn,
      Encode encode,
      int length = -1) {
    using F = std::decay_t<Fn>;
    using R = typename detail::async_method_traits<T, F>::result_type;
    static_assert(
        !std::is_void_v<R>,
        "async_method: custom Encode is not supported for Task<void> "
        "(void results always resolve to undefined)");
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    qjs::OwnedValue fn_val = make_async_method_function(
        std::move(fn), std::move(encode), length, name);
    (void)absorb_method(methods_, entry_name, std::move(fn_val));
    return *this;
  }

  /**
   * Readonly accessor on the prototype (getter only).
   * Getter forms match method forms (T& / CallbackInfo / ...).
   * Created through create_function (direct owner-thread QuickJS turn).
   */
  template <class Fn>
  ClassBuilder& readonly(const char* name, Fn fn) {
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    qjs::OwnedValue getter = make_method_function(std::move(fn), 0, name);
    if (!absorb_getter(entry_name, std::move(getter))) {
      return *this;
    }
    return *this;
  }

  /**
   * Read/write accessor on the prototype (getter + setter).
   * Same staging, context, ownership, and exception-cleanliness rules as
   * readonly. Getter length 0, setter length 1; enumerable + configurable.
   * Both are created through create_function (direct owner-thread QuickJS turn).
   * No generic symbol support in this slice (string names only).
   * Getter/setter forms match method forms (T& / CallbackInfo / ...).
   */
  template <class GetterFn, class SetterFn>
  ClassBuilder& accessor(const char* name, GetterFn getter_fn, SetterFn setter_fn) {
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};

    qjs::OwnedValue getter = make_method_function(std::move(getter_fn), 0, name);
    if (getter.is_exception()) {
      clear_exception(ctx_.context());
      (void)getter.release();
      staging_error_ = Error::internal(
          "ClassBuilder: bad accessor getter " +
          (entry_name.empty() ? std::string{"<unnamed>"} : entry_name));
      return *this;
    }
    qjs::OwnedValue setter = make_method_function(std::move(setter_fn), 1, name);
    if (setter.is_exception()) {
      detail::discard_owned(std::move(getter));
      clear_exception(ctx_.context());
      (void)setter.release();
      staging_error_ = Error::internal(
          "ClassBuilder: bad accessor setter " +
          (entry_name.empty() ? std::string{"<unnamed>"} : entry_name));
      return *this;
    }
    accessors_.push_back(
        Accessor{entry_name, std::move(getter), std::move(setter)});
    return *this;
  }

  /** Static function on the constructor object. */
  template <class Fn>
  ClassBuilder& static_function(const char* name, Fn fn, int length = 0) {
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    qjs::OwnedValue fn_val = create_function(ctx_, name, std::move(fn), length);
    (void)absorb_method(statics_, entry_name, std::move(fn_val));
    return *this;
  }

  /**
   * Static async factory / function on the constructor object → Promise.
   * Same free-function callable forms as create_async_function.
   * Default encode is class-aware: Task<shared_ptr<T>> / Task<T> wrap as
   * instances of this class (Store.open / File.open style); other results
   * use Converter.
   *
   * Staging / commit / error cleanliness match static_function.
   */
  template <class Fn>
  ClassBuilder& static_async_function(const char* name, Fn fn, int length = -1) {
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    qjs::OwnedValue fn_val =
        make_static_async_function(std::move(fn), length, name);
    (void)absorb_method(statics_, entry_name, std::move(fn_val));
    return *this;
  }

  /**
   * static_async_function with a custom owner-thread Encode.
   * Not available for Task<void> (always resolves undefined).
   */
  template <class Fn, class Encode>
    requires detail::is_async_encode_arg_v<Encode>
  ClassBuilder& static_async_function(
      const char* name,
      Fn fn,
      Encode encode,
      int length = -1) {
    using F = std::decay_t<Fn>;
    using R = typename detail::async_callable_traits<F>::result_type;
    static_assert(
        !std::is_void_v<R>,
        "static_async_function: custom Encode is not supported for Task<void> "
        "(void results always resolve to undefined)");
    if (staging_error_) {
      return *this;
    }
    const std::string entry_name{name};
    qjs::OwnedValue fn_val = make_static_async_function(
        std::move(fn), std::move(encode), length, name);
    (void)absorb_method(statics_, entry_name, std::move(fn_val));
    return *this;
  }

  /**
   * Register class, install proto + constructor, return caller-owned ctor.
   * On failure returns unexpected and leaves no pending JS exception.
   */
  [[nodiscard]] Result<qjs::OwnedValue> commit() {
    JSContext* c = ctx_.context();

    if (staging_error_) {
      constructor_.reset();
      methods_.clear();
      getters_.clear();
      accessors_.clear();
      statics_.clear();
      return std::unexpected(std::move(*staging_error_));
    }

    if (Storage::ensure_registered(c, class_name_.c_str()) < 0) {
      if (JS_HasException(c)) {
        clear_exception(c);
      }
      return std::unexpected(
          Error::internal("ClassBuilder: JS_NewClass failed"));
    }

    qjs::OwnedValue proto = ctx_.new_object();
    if (proto.is_exception()) {
      clear_exception(c);
      (void)proto.release();
      return std::unexpected(Error::internal("ClassBuilder: proto NewObject"));
    }
    for (auto& m : methods_) {
      JSValue raw = m.fn.release();
      // SetPropertyStr takes ownership of raw (including on failure).
      if (JS_SetPropertyStr(c, proto.get(), m.name.c_str(), raw) < 0) {
        clear_exception(c);
        return std::unexpected(
            Error::internal("ClassBuilder: method SetProperty " + m.name));
      }
    }

    for (auto& g : getters_) {
      JSAtom atom = JS_NewAtom(c, g.name.c_str());
      if (atom == JS_ATOM_NULL) {
        if (JS_HasException(c)) {
          clear_exception(c);
        }
        return std::unexpected(Error::internal("ClassBuilder: NewAtom failed"));
      }
      JSValue getter = g.getter.release();
      // Takes ownership of getter; setter JS_UNDEFINED = readonly.
      const int def = JS_DefinePropertyGetSet(
          c,
          proto.get(),
          atom,
          getter,
          JS_UNDEFINED,
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(c, atom);
      if (def < 0) {
        clear_exception(c);
        return std::unexpected(
            Error::internal("ClassBuilder: DefinePropertyGetSet " + g.name));
      }
    }

    for (auto& a : accessors_) {
      JSAtom atom = JS_NewAtom(c, a.name.c_str());
      if (atom == JS_ATOM_NULL) {
        if (JS_HasException(c)) {
          clear_exception(c);
        }
        return std::unexpected(Error::internal("ClassBuilder: NewAtom failed"));
      }
      JSValue getter = a.getter.release();
      JSValue setter = a.setter.release();
      // Takes ownership of getter and setter.
      const int def = JS_DefinePropertyGetSet(
          c,
          proto.get(),
          atom,
          getter,
          setter,
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(c, atom);
      if (def < 0) {
        clear_exception(c);
        return std::unexpected(Error::internal(
            "ClassBuilder: DefinePropertyGetSet accessor " + a.name));
      }
    }

    qjs::OwnedValue ctor = constructor_.empty()
                               ? make_unavailable_constructor()
                               : std::move(constructor_);
    if (ctor.is_exception()) {
      clear_exception(c);
      (void)ctor.release();
      return std::unexpected(
          Error::internal("ClassBuilder: constructor create failed"));
    }
    const JSClassID cid = Storage::registered_class_id();

    // SetClassProto takes ownership of its argument.
    JS_SetClassProto(c, cid, JS_DupValue(c, proto.get()));
    if (JS_SetConstructor(c, ctor.get(), proto.get()) < 0) {
      clear_exception(c);
      return std::unexpected(Error::internal("ClassBuilder: SetConstructor"));
    }

    for (auto& s : statics_) {
      JSValue raw = s.fn.release();
      if (JS_SetPropertyStr(c, ctor.get(), s.name.c_str(), raw) < 0) {
        clear_exception(c);
        return std::unexpected(
            Error::internal("ClassBuilder: static SetProperty " + s.name));
      }
    }

    methods_.clear();
    getters_.clear();
    accessors_.clear();
    statics_.clear();
    return ctor;
  }

  /** Wrap an existing shared_ptr as a JS instance of this class. */
  [[nodiscard]] qjs::OwnedValue wrap(std::shared_ptr<T> value) const {
    return Storage::wrap(
        ctx_.context(), class_name_.c_str(), std::move(value));
  }

  /** Unwrap instance; binding TypeError on class mismatch (engine clean). */
  [[nodiscard]] static Result<std::shared_ptr<T>> unwrap(
      JSContext* ctx,
      JSValueConst obj) {
    auto r = detail::unwrap_this<T>(ctx, obj);
    if (!r) {
      return std::unexpected(std::move(r.error()));
    }
    return std::move(r->second);
  }

 private:
  template <class Err>
  static Error map_ctor_error(Err&& e) {
    using E = std::remove_cvref_t<Err>;
    if constexpr (std::is_same_v<E, Error>) {
      return std::forward<Err>(e);
    } else if constexpr (std::is_same_v<E, vacps::runtime::Error>) {
      return Error::from_runtime(e);
    } else if constexpr (std::is_same_v<E, vacps::Error>) {
      return Error::from_domain(e);
    } else {
      return Error::internal("constructor failed");
    }
  }

  template <class V>
  static Result<std::shared_ptr<T>> normalize_ctor_value(V&& v) {
    using Raw = std::remove_cvref_t<V>;
    if constexpr (std::is_same_v<Raw, std::shared_ptr<T>>) {
      return std::forward<V>(v);
    } else if constexpr (std::is_same_v<Raw, T>) {
      return std::make_shared<T>(std::forward<V>(v));
    } else {
      static_assert(
          std::is_same_v<Raw, std::shared_ptr<T>> || std::is_same_v<Raw, T>,
          "Class constructor must return shared_ptr<T>, T, or Result of those");
      return std::unexpected(Error::internal("bad constructor return type"));
    }
  }

  template <class Fn, class R>
  static Result<std::shared_ptr<T>> finish_ctor_return(R&& value) {
    if constexpr (detail::is_result_v<std::remove_cvref_t<R>>) {
      if (!value) {
        return std::unexpected(map_ctor_error(std::move(value.error())));
      }
      return normalize_ctor_value(std::move(*value));
    } else {
      return normalize_ctor_value(std::forward<R>(value));
    }
  }

  template <class Fn, std::size_t... I>
  static Result<std::shared_ptr<T>> invoke_ctor_args(
      const CallbackInfo& info,
      Fn& fn,
      std::index_sequence<I...>) {
    using Traits = detail::callable_traits<std::remove_cvref_t<Fn>>;
    using ArgsTuple = typename Traits::args_tuple;
    auto decoded =
        detail::decode_args<std::tuple_element_t<I, ArgsTuple>...>(info, 0);
    if (!decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    return finish_ctor_return<Fn>(std::apply(fn, std::move(*decoded)));
  }

  template <class Fn>
  static Result<std::shared_ptr<T>> invoke_ctor(
      const CallbackInfo& info,
      Fn& fn) {
    using Traits = detail::callable_traits<std::remove_cvref_t<Fn>>;
    if constexpr (Traits::arity == 0) {
      return finish_ctor_return<Fn>(fn());
    } else {
      return invoke_ctor_args(
          info, fn, std::make_index_sequence<Traits::arity>{});
    }
  }

  template <class Fn>
  qjs::OwnedValue make_method_function(Fn fn, int length, const char* name) {
    return create_function(
        ctx_,
        name,
        [fn = std::move(fn)](const CallbackInfo& info) mutable -> qjs::OwnedValue {
          auto u = detail::unwrap_this<T>(info.context(), info.this_raw());
          if (!u) {
            return qjs::OwnedValue::take(
                info.context(), throw_error(info.context(), u.error()));
          }
          return detail::dispatch_method<T>(
              info, *u->first, u->second, fn);
        },
        length);
  }

  template <class Fn>
  qjs::OwnedValue make_async_method_function(
      Fn fn,
      int length,
      const char* name) {
    using F = std::decay_t<Fn>;
    using R = typename detail::async_method_traits<T, F>::result_type;
    if constexpr (std::is_void_v<R>) {
      return detail::make_async_method_function<T>(
          ctx_,
          name,
          std::move(fn),
          detail::void_encode_unused{},
          length);
    } else {
      return detail::make_async_method_function<T>(
          ctx_,
          name,
          std::move(fn),
          detail::class_async_result_encode<T>{class_name_},
          length);
    }
  }

  template <class Fn, class Encode>
  qjs::OwnedValue make_async_method_function(
      Fn fn,
      Encode encode,
      int length,
      const char* name) {
    using F = std::decay_t<Fn>;
    using R = typename detail::async_method_traits<T, F>::result_type;
    static_assert(
        !std::is_void_v<R>,
        "async_method: custom Encode is not supported for Task<void>");
    return detail::make_async_method_function<T>(
        ctx_, name, std::move(fn), std::move(encode), length);
  }

  template <class Fn>
  qjs::OwnedValue make_static_async_function(
      Fn fn,
      int length,
      const char* name) {
    using F = std::decay_t<Fn>;
    using R = typename detail::async_callable_traits<F>::result_type;
    if constexpr (std::is_void_v<R>) {
      return detail::make_async_free_function(
          ctx_,
          name,
          std::move(fn),
          detail::void_encode_unused{},
          length);
    } else {
      return detail::make_async_free_function(
          ctx_,
          name,
          std::move(fn),
          detail::class_async_result_encode<T>{class_name_},
          length);
    }
  }

  template <class Fn, class Encode>
  qjs::OwnedValue make_static_async_function(
      Fn fn,
      Encode encode,
      int length,
      const char* name) {
    using F = std::decay_t<Fn>;
    using R = typename detail::async_callable_traits<F>::result_type;
    static_assert(
        !std::is_void_v<R>,
        "static_async_function: custom Encode is not supported for Task<void>");
    return detail::make_async_free_function(
        ctx_, name, std::move(fn), std::move(encode), length);
  }

  template <class Fn>
  qjs::OwnedValue make_constructor_function(Fn fn, int length) {
    const std::string name = class_name_;
    qjs::OwnedValue function = create_function(
        ctx_,
        name.c_str(),
        [fn = std::move(fn), name](
            const CallbackInfo& info) mutable -> qjs::OwnedValue {
          JSContext* c = info.context();
          // Phase-1 construct check: on [[Construct]], CFunctionData receives
          // new_target as this_val. Reject plain calls where this is not a
          // constructor (covers Counter() without new for ordinary call sites).
          if (!JS_IsConstructor(c, info.this_raw())) {
            return qjs::OwnedValue::take(
                c,
                throw_type(
                    c, (name + " constructor must be called with new").c_str()));
          }

          auto native = invoke_ctor(info, fn);
          if (!native) {
            return qjs::OwnedValue::take(c, throw_error(c, native.error()));
          }

          // Prefer new_target.prototype for subclass instances.
          JSValue proto = JS_GetPropertyStr(c, info.this_raw(), "prototype");
          if (JS_IsException(proto)) {
            return qjs::OwnedValue::take(c, proto);
          }
          if (!JS_IsObject(proto)) {
            JS_FreeValue(c, proto);
            proto = JS_GetClassProto(c, Storage::registered_class_id());
            if (JS_IsException(proto)) {
              return qjs::OwnedValue::take(c, proto);
            }
          }

          qjs::OwnedValue obj =
              Storage::wrap_with_proto(c, proto, std::move(*native));
          JS_FreeValue(c, proto);
          return obj;
        },
        length);

    if (!function.is_exception()) {
      JS_SetConstructorBit(ctx_.context(), function.get(), 1);
    }
    return function;
  }

  qjs::OwnedValue make_unavailable_constructor() {
    const std::string name = class_name_;
    qjs::OwnedValue function = create_function(
        ctx_,
        name.c_str(),
        [name](const CallbackInfo& info) -> qjs::OwnedValue {
          JSContext* c = info.context();
          return qjs::OwnedValue::take(
              c,
              throw_type(
                  c,
                  (name + " cannot be invoked without a constructor").c_str()));
        },
        0);
    if (!function.is_exception()) {
      JS_SetConstructorBit(ctx_.context(), function.get(), 1);
    }
    return function;
  }

  struct Method {
    std::string name;
    qjs::OwnedValue fn;
  };
  struct Getter {
    std::string name;
    qjs::OwnedValue getter;
  };
  struct Accessor {
    std::string name;
    qjs::OwnedValue getter;
    qjs::OwnedValue setter;
  };

  bool absorb_getter(std::string name, qjs::OwnedValue getter) {
    if (staging_error_) {
      detail::discard_owned(std::move(getter));
      return false;
    }
    if (getter.is_exception()) {
      clear_exception(ctx_.context());
      (void)getter.release();
      staging_error_ = Error::internal(
          "ClassBuilder: bad getter " +
          (name.empty() ? std::string{"<unnamed>"} : name));
      return false;
    }
    getters_.push_back(Getter{std::move(name), std::move(getter)});
    return true;
  }

  bool absorb_method(
      std::vector<Method>& into,
      std::string name,
      qjs::OwnedValue fn_val) {
    if (staging_error_) {
      detail::discard_owned(std::move(fn_val));
      return false;
    }
    if (fn_val.is_exception()) {
      clear_exception(ctx_.context());
      (void)fn_val.release();
      staging_error_ = Error::internal(
          "ClassBuilder: bad method " +
          (name.empty() ? std::string{"<unnamed>"} : name));
      return false;
    }
    into.push_back(Method{std::move(name), std::move(fn_val)});
    return true;
  }

  Env ctx_;
  std::string class_name_;
  qjs::OwnedValue constructor_;
  std::vector<Method> methods_;
  std::vector<Getter> getters_;
  std::vector<Accessor> accessors_;
  std::vector<Method> statics_;
  std::optional<Error> staging_error_;
};

}  // namespace vacps::binding
