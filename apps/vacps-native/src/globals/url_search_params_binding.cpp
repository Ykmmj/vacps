#include "globals/url_search_params_binding.hpp"

#include "binding/class.hpp"
#include "binding/coerce.hpp"
#include "binding/function.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;

/** Resolve globalThis.Symbol.iterator → atom (caller frees atom). */
bool symbol_iterator_atom(JSContext* ctx, JSAtom* out) {
  JSValue global = JS_GetGlobalObject(ctx);
  if (JS_IsException(global)) {
    binding::clear_exception(ctx);
    return false;
  }
  JSValue Symbol = JS_GetPropertyStr(ctx, global, "Symbol");
  JS_FreeValue(ctx, global);
  if (JS_IsException(Symbol)) {
    binding::clear_exception(ctx);
    return false;
  }
  JSValue iter = JS_GetPropertyStr(ctx, Symbol, "iterator");
  JS_FreeValue(ctx, Symbol);
  if (JS_IsException(iter)) {
    binding::clear_exception(ctx);
    return false;
  }
  JSAtom atom = JS_ValueToAtom(ctx, iter);
  JS_FreeValue(ctx, iter);
  if (atom == JS_ATOM_NULL) {
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
    }
    return false;
  }
  *out = atom;
  return true;
}

/** Build `{ value, done }` for iterator protocol. Takes ownership of `value`. */
qjs::OwnedValue make_iter_result(
    binding::Env env,
    qjs::OwnedValue value,
    bool done) {
  JSContext* c = env.context();
  qjs::OwnedValue obj = env.new_object();
  if (obj.is_exception()) {
    value.reset();
    return obj;
  }
  JSValue raw_value = value.release();
  if (JS_DefinePropertyValueStr(
          c, obj.get(), "value", raw_value, JS_PROP_C_W_E) < 0) {
    binding::clear_exception(c);
    return qjs::OwnedValue::take(
        c, binding::throw_internal(c, "iterator result value"));
  }
  if (JS_DefinePropertyValueStr(
          c, obj.get(), "done", JS_NewBool(c, done ? 1 : 0), JS_PROP_C_W_E) <
      0) {
    binding::clear_exception(c);
    return qjs::OwnedValue::take(
        c, binding::throw_internal(c, "iterator result done"));
  }
  return obj;
}

// ── URLSearchParams Iterator (binding-local pure C++ state) ────────

enum class SpIterKind : std::uint8_t { Keys, Values, Entries };

struct SearchParamsIteratorState {
  std::shared_ptr<vacps::url::SearchParams> params;
  std::size_t index = 0;
  SpIterKind kind = SpIterKind::Entries;
};

using IterBuilder = binding::ClassBuilder<SearchParamsIteratorState>;
using SpBuilder = binding::ClassBuilder<vacps::url::SearchParams>;

qjs::OwnedValue wrap_iterator(
    binding::Env env,
    std::shared_ptr<vacps::url::SearchParams> params,
    SpIterKind kind) {
  auto state = std::make_shared<SearchParamsIteratorState>();
  state->params = std::move(params);
  state->index = 0;
  state->kind = kind;
  return IterBuilder{env, "URLSearchParams Iterator"}.wrap(std::move(state));
}

binding::VoidResult install_sp_iterator_class(binding::Env env) {
  IterBuilder cls{env, "URLSearchParams Iterator"};
  cls.method(
      "next",
      [](const binding::CallbackInfo& info,
         SearchParamsIteratorState& self) -> qjs::OwnedValue {
        auto pair = self.params->at(self.index);
        if (!pair) {
          return make_iter_result(
              info.env(), info.env().undefined(), /*done=*/true);
        }
        ++self.index;

        binding::Env e = info.env();
        JSContext* c = e.context();
        qjs::OwnedValue value;
        switch (self.kind) {
          case SpIterKind::Keys:
            value = e.string(pair->first);
            break;
          case SpIterKind::Values:
            value = e.string(pair->second);
            break;
          case SpIterKind::Entries: {
            value = e.new_array();
            if (value.is_exception()) {
              return value;
            }
            JSValue k = JS_NewStringLen(
                c, pair->first.data(), pair->first.size());
            JSValue v = JS_NewStringLen(
                c, pair->second.data(), pair->second.size());
            if (JS_IsException(k) || JS_IsException(v)) {
              JS_FreeValue(c, k);
              JS_FreeValue(c, v);
              return qjs::OwnedValue::take(
                  c, binding::throw_internal(c, "entries strings"));
            }
            // SetPropertyUint32 takes ownership of k/v.
            if (JS_SetPropertyUint32(c, value.get(), 0, k) < 0) {
              JS_FreeValue(c, v);
              binding::clear_exception(c);
              return qjs::OwnedValue::take(
                  c, binding::throw_internal(c, "entries array"));
            }
            if (JS_SetPropertyUint32(c, value.get(), 1, v) < 0) {
              binding::clear_exception(c);
              return qjs::OwnedValue::take(
                  c, binding::throw_internal(c, "entries array"));
            }
            break;
          }
        }
        if (value.is_exception()) {
          return value;
        }
        return make_iter_result(e, std::move(value), /*done=*/false);
      },
      0);

  auto ctor = cls.commit();
  if (!ctor) {
    return std::unexpected(ctor.error());
  }
  // Not installed on globalThis — only used as iterator instances.
  ctor->reset();

  // Escape hatch: @@iterator returns the same JS receiver.
  JSContext* c = env.context();
  JSClassID cid = IterBuilder::class_id();
  JSValue proto = JS_GetClassProto(c, cid);
  if (JS_IsException(proto)) {
    binding::clear_exception(c);
    return std::unexpected(
        binding::Error::internal("URLSearchParams Iterator proto"));
  }

  JSAtom iter_atom = JS_ATOM_NULL;
  if (symbol_iterator_atom(c, &iter_atom)) {
    qjs::OwnedValue self_fn = binding::create_function(
        env,
        "[Symbol.iterator]",
        [](const binding::CallbackInfo& info) -> qjs::OwnedValue {
          auto u = IterBuilder::unwrap(info.context(), info.this_raw());
          if (!u) {
            return qjs::OwnedValue::take(
                info.context(),
                binding::throw_error(info.context(), u.error()));
          }
          return qjs::OwnedValue{
              info.context(), JS_DupValue(info.context(), info.this_raw())};
        },
        0);
    if (self_fn.is_exception()) {
      binding::clear_exception(c);
      (void)self_fn.release();
      JS_FreeAtom(c, iter_atom);
      JS_FreeValue(c, proto);
      return std::unexpected(binding::Error::internal(
          "URLSearchParams Iterator Symbol.iterator"));
    }
    if (JS_DefinePropertyValue(
            c,
            proto,
            iter_atom,
            self_fn.release(),
            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
      binding::clear_exception(c);
      JS_FreeAtom(c, iter_atom);
      JS_FreeValue(c, proto);
      return std::unexpected(binding::Error::internal(
          "URLSearchParams Iterator Symbol.iterator define"));
    }
    JS_FreeAtom(c, iter_atom);
  }
  JS_FreeValue(c, proto);
  return {};
}

binding::VoidResult install_search_params_class(binding::Env env) {
  SpBuilder cls{env, "URLSearchParams"};

  cls.constructor(
      [](const binding::CallbackInfo& info)
          -> binding::Result<std::shared_ptr<vacps::url::SearchParams>> {
        std::optional<std::string> init;
        if (info.length() >= 1 && !info[0].is_nullish()) {
          init = binding::try_coerce_string(info.context(), info[0].get());
          if (!init) {
            return std::unexpected(binding::Error::type(
                "URLSearchParams: init must be a string"));
          }
        }
        if (init) {
          return std::make_shared<vacps::url::SearchParams>(*init);
        }
        return std::make_shared<vacps::url::SearchParams>();
      },
      1);

  cls.method(
      "append",
      [](const binding::CallbackInfo& info,
         vacps::url::SearchParams& self) -> binding::VoidResult {
        if (info.length() < 2) {
          return std::unexpected(binding::Error::type(
              "URLSearchParams.append requires name and value"));
        }
        auto key = binding::try_coerce_string(info.context(), info[0].get());
        auto value = binding::try_coerce_string(info.context(), info[1].get());
        if (!key || !value) {
          return std::unexpected(binding::Error::type(
              "URLSearchParams.append: name and value must be strings"));
        }
        self.append(*key, *value);
        return {};
      },
      2);

  cls.method(
      "set",
      [](const binding::CallbackInfo& info,
         vacps::url::SearchParams& self) -> binding::VoidResult {
        if (info.length() < 2) {
          return std::unexpected(binding::Error::type(
              "URLSearchParams.set requires name and value"));
        }
        auto key = binding::try_coerce_string(info.context(), info[0].get());
        auto value = binding::try_coerce_string(info.context(), info[1].get());
        if (!key || !value) {
          return std::unexpected(binding::Error::type(
              "URLSearchParams.set: name and value must be strings"));
        }
        self.set(*key, *value);
        return {};
      },
      2);

  cls.method(
      "get",
      [](const binding::CallbackInfo& info,
         vacps::url::SearchParams& self) -> qjs::OwnedValue {
        if (info.length() < 1) {
          return qjs::OwnedValue::take(
              info.context(),
              binding::throw_type(
                  info.context(), "URLSearchParams.get requires name"));
        }
        auto key = binding::try_coerce_string(info.context(), info[0].get());
        if (!key) {
          return qjs::OwnedValue::take(
              info.context(),
              binding::throw_type(
                  info.context(),
                  "URLSearchParams.get: name must be a string"));
        }
        auto v = self.get(*key);
        if (!v) {
          return info.env().null_value();
        }
        return info.env().string(*v);
      },
      1);

  cls.method(
      "getAll",
      [](const binding::CallbackInfo& info,
         vacps::url::SearchParams& self) -> qjs::OwnedValue {
        if (info.length() < 1) {
          return qjs::OwnedValue::take(
              info.context(),
              binding::throw_type(
                  info.context(), "URLSearchParams.getAll requires name"));
        }
        auto key = binding::try_coerce_string(info.context(), info[0].get());
        if (!key) {
          return qjs::OwnedValue::take(
              info.context(),
              binding::throw_type(
                  info.context(),
                  "URLSearchParams.getAll: name must be a string"));
        }
        std::vector<std::string> all = self.get_all(*key);
        qjs::OwnedValue arr = info.env().new_array();
        if (arr.is_exception()) {
          return arr;
        }
        JSContext* c = info.context();
        for (uint32_t i = 0; i < static_cast<uint32_t>(all.size()); ++i) {
          JSValue item =
              JS_NewStringLen(c, all[i].data(), all[i].size());
          if (JS_IsException(item)) {
            return qjs::OwnedValue::take(c, item);
          }
          if (JS_SetPropertyUint32(c, arr.get(), i, item) < 0) {
            binding::clear_exception(c);
            return qjs::OwnedValue::take(
                c, binding::throw_internal(c, "getAll item"));
          }
        }
        return arr;
      },
      1);

  cls.method(
      "has",
      [](const binding::CallbackInfo& info,
         vacps::url::SearchParams& self) -> binding::Result<bool> {
        if (info.length() < 1) {
          return std::unexpected(
              binding::Error::type("URLSearchParams.has requires name"));
        }
        auto key = binding::try_coerce_string(info.context(), info[0].get());
        if (!key) {
          return std::unexpected(binding::Error::type(
              "URLSearchParams.has: name must be a string"));
        }
        if (info.length() >= 2 && !info[1].is_nullish()) {
          auto value = binding::try_coerce_string(info.context(), info[1].get());
          if (!value) {
            return std::unexpected(binding::Error::type(
                "URLSearchParams.has: value must be a string"));
          }
          return self.has(*key, *value);
        }
        return self.has(*key);
      },
      1);

  // JS `delete` — domain uses remove.
  cls.method(
      "delete",
      [](const binding::CallbackInfo& info,
         vacps::url::SearchParams& self) -> binding::VoidResult {
        if (info.length() < 1) {
          return std::unexpected(
              binding::Error::type("URLSearchParams.delete requires name"));
        }
        auto key = binding::try_coerce_string(info.context(), info[0].get());
        if (!key) {
          return std::unexpected(binding::Error::type(
              "URLSearchParams.delete: name must be a string"));
        }
        if (info.length() >= 2 && !info[1].is_nullish()) {
          auto value = binding::try_coerce_string(info.context(), info[1].get());
          if (!value) {
            return std::unexpected(binding::Error::type(
                "URLSearchParams.delete: value must be a string"));
          }
          self.remove(*key, *value);
        } else {
          self.remove(*key);
        }
        return {};
      },
      1);

  cls.method(
      "sort",
      [](vacps::url::SearchParams& self) { self.sort(); },
      0);

  cls.method(
      "toString",
      [](vacps::url::SearchParams& self) { return self.to_string(); },
      0);

  cls.readonly(
      "size",
      [](const vacps::url::SearchParams& self) {
        return static_cast<std::uint32_t>(self.size());
      });

  cls.method(
      "entries",
      [](const binding::CallbackInfo& info,
         std::shared_ptr<vacps::url::SearchParams> self) -> qjs::OwnedValue {
        return wrap_iterator(info.env(), std::move(self), SpIterKind::Entries);
      },
      0);

  cls.method(
      "keys",
      [](const binding::CallbackInfo& info,
         std::shared_ptr<vacps::url::SearchParams> self) -> qjs::OwnedValue {
        return wrap_iterator(info.env(), std::move(self), SpIterKind::Keys);
      },
      0);

  cls.method(
      "values",
      [](const binding::CallbackInfo& info,
         std::shared_ptr<vacps::url::SearchParams> self) -> qjs::OwnedValue {
        return wrap_iterator(info.env(), std::move(self), SpIterKind::Values);
      },
      0);

  /**
   * forEach(callback[, thisArg]) — WHATWG order: callback(value, name, this).
   * Domain reads are plain C++; JS_Call is the intentional escape hatch.
   */
  cls.method(
      "forEach",
      [](const binding::CallbackInfo& info,
         std::shared_ptr<vacps::url::SearchParams> self) -> qjs::OwnedValue {
        JSContext* c = info.context();
        if (info.length() < 1 || !JS_IsFunction(c, info[0].get())) {
          return qjs::OwnedValue::take(
              c,
              binding::throw_type(
                  c, "URLSearchParams.forEach requires a function"));
        }
        JSValueConst callback = info[0].get();
        JSValueConst this_arg =
            (info.length() >= 2) ? info[1].get() : JS_UNDEFINED;

        std::size_t i = 0;
        for (;;) {
          auto pair = self->at(i);
          if (!pair) {
            break;
          }

          JSValue args[3];
          args[0] = JS_NewStringLen(
              c, pair->second.data(), pair->second.size());
          args[1] =
              JS_NewStringLen(c, pair->first.data(), pair->first.size());
          args[2] = info.this_raw();  // borrowed
          if (JS_IsException(args[0]) || JS_IsException(args[1])) {
            JS_FreeValue(c, args[0]);
            JS_FreeValue(c, args[1]);
            return qjs::OwnedValue::exception(c);
          }
          JSValue ret = JS_Call(c, callback, this_arg, 3, args);
          JS_FreeValue(c, args[0]);
          JS_FreeValue(c, args[1]);
          if (JS_IsException(ret)) {
            return qjs::OwnedValue::take(c, ret);
          }
          JS_FreeValue(c, ret);
          ++i;
        }
        return info.env().undefined();
      },
      1);

  auto ctor = cls.commit();
  if (!ctor) {
    return std::unexpected(ctor.error());
  }

  // Escape hatch: alias committed prototype.entries → Symbol.iterator.
  JSContext* c = env.context();
  JSClassID cid = SpBuilder::class_id();
  JSValue proto = JS_GetClassProto(c, cid);
  if (JS_IsException(proto)) {
    binding::clear_exception(c);
    ctor->reset();
    return std::unexpected(
        binding::Error::internal("URLSearchParams proto for Symbol.iterator"));
  }

  JSAtom iter_atom = JS_ATOM_NULL;
  if (symbol_iterator_atom(c, &iter_atom)) {
    JSValue entries_fn = JS_GetPropertyStr(c, proto, "entries");
    if (JS_IsException(entries_fn)) {
      binding::clear_exception(c);
      JS_FreeAtom(c, iter_atom);
      JS_FreeValue(c, proto);
      ctor->reset();
      return std::unexpected(
          binding::Error::internal("URLSearchParams.entries for Symbol.iterator"));
    }
    if (JS_DefinePropertyValue(
            c,
            proto,
            iter_atom,
            entries_fn,
            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
      binding::clear_exception(c);
      JS_FreeAtom(c, iter_atom);
      JS_FreeValue(c, proto);
      ctor->reset();
      return std::unexpected(binding::Error::internal(
          "URLSearchParams Symbol.iterator define"));
    }
    JS_FreeAtom(c, iter_atom);
  }
  JS_FreeValue(c, proto);

  // Escape hatch: install constructor on globalThis.
  JSValue global = JS_GetGlobalObject(c);
  if (JS_IsException(global)) {
    binding::clear_exception(c);
    ctor->reset();
    return std::unexpected(
        binding::Error::internal("URLSearchParams GetGlobalObject"));
  }
  JSValue raw_ctor = ctor->release();
  if (JS_DefinePropertyValueStr(
          c,
          global,
          "URLSearchParams",
          raw_ctor,
          JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    binding::clear_exception(c);
    JS_FreeValue(c, global);
    return std::unexpected(
        binding::Error::internal("URLSearchParams global define"));
  }
  JS_FreeValue(c, global);
  return {};
}

}  // namespace

JSValue make_search_params(
    JSContext* ctx,
    std::shared_ptr<vacps::url::SearchParams> params) {
  // Same ClassBuilder storage / class id as new URLSearchParams.
  binding::Env env{ctx};
  qjs::OwnedValue obj =
      binding::ClassBuilder<vacps::url::SearchParams>{env, "URLSearchParams"}
          .wrap(std::move(params));
  return obj.release();
}

VoidResult install_url_search_params_binding(JSContext* ctx) {
  try {
    binding::Env env{ctx};

    if (auto r = install_sp_iterator_class(env); !r) {
      if (JS_HasException(ctx)) {
        binding::clear_exception(ctx);
      }
      return std::unexpected(Error{std::move(r.error().message)});
    }
    if (auto r = install_search_params_class(env); !r) {
      if (JS_HasException(ctx)) {
        binding::clear_exception(ctx);
      }
      return std::unexpected(Error{std::move(r.error().message)});
    }
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
      return std::unexpected(
          Error{"install_url_search_params_binding: pending exception"});
    }
    return {};
  } catch (const std::exception& ex) {
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
    }
    return std::unexpected(Error{ex.what()});
  } catch (...) {
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
    }
    return std::unexpected(
        Error{"install_url_search_params_binding: unknown failure"});
  }
}

}  // namespace vacps::js
