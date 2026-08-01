#include "quickjs/globals/url_search_params_binding.hpp"

#include "quickjs/raii/convert.hpp"
#include "quickjs/raii/cstring.hpp"
#include "quickjs/raii/value.hpp"

#include <quickjs.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

// ── helpers ───────────────────────────────────────────────────────

JSValue js_string_owned(JSContext* ctx, const std::string& s) {
  return JS_NewStringLen(ctx, s.data(), s.size());
}

std::optional<std::string> js_to_string(JSContext* ctx, JSValueConst v) {
  auto s = converter<std::string>::from_js(ctx, v);
  if (!s) return std::nullopt;
  return std::move(*s);
}

void define_getter(JSContext* ctx, JSValueConst proto, const char* name, JSCFunction* getter) {
  JSValue get = JS_NewCFunction(ctx, getter, name, 0);
  JSAtom atom = JS_NewAtom(ctx, name);
  JS_DefinePropertyGetSet(ctx, proto, atom, get, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
}

/** Build `{ value, done }` for iterator protocol. Takes ownership of `value`. */
JSValue make_iter_result(JSContext* ctx, JSValue value, bool done) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    JS_FreeValue(ctx, value);
    return obj;
  }
  if (JS_DefinePropertyValueStr(
          ctx, obj, "value", value, JS_PROP_C_W_E) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  if (JS_DefinePropertyValueStr(
          ctx, obj, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

/** Resolve globalThis.Symbol.iterator → atom (caller frees atom). */
bool symbol_iterator_atom(JSContext* ctx, JSAtom* out) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue Symbol = JS_GetPropertyStr(ctx, global, "Symbol");
  JS_FreeValue(ctx, global);
  if (JS_IsException(Symbol)) {
    return false;
  }
  JSValue iter = JS_GetPropertyStr(ctx, Symbol, "iterator");
  JS_FreeValue(ctx, Symbol);
  if (JS_IsException(iter)) {
    return false;
  }
  JSAtom atom = JS_ValueToAtom(ctx, iter);
  JS_FreeValue(ctx, iter);
  if (atom == JS_ATOM_NULL) {
    return false;
  }
  *out = atom;
  return true;
}

// ── URLSearchParams (vacps::url::SearchParams) ────────────────────

struct SearchParamsHandle {
  std::shared_ptr<vacps::url::SearchParams> params;
};

JSClassID g_search_params_class_id = 0;

void search_params_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<SearchParamsHandle*>(JS_GetOpaque(val, g_search_params_class_id));
  delete h;
}

JSClassDef g_search_params_class = {
    "URLSearchParams",
    .finalizer = search_params_finalizer,
};

void ensure_search_params_class(JSContext* ctx) {
  if (g_search_params_class_id == 0) {
    JS_NewClassID(&g_search_params_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_search_params_class_id)) {
    JS_NewClass(rt, g_search_params_class_id, &g_search_params_class);
  }
}

SearchParamsHandle* search_params_from_this(JSContext* ctx, JSValueConst this_val) {
  auto* h = static_cast<SearchParamsHandle*>(
      JS_GetOpaque2(ctx, this_val, g_search_params_class_id));
  if (h == nullptr) {
    JS_ThrowTypeError(ctx, "URLSearchParams method requires a URLSearchParams instance");
    return nullptr;
  }
  return h;
}

// ── URLSearchParams Iterator ──────────────────────────────────────

enum class SpIterKind : std::uint8_t { Keys, Values, Entries };

struct SearchParamsIterHandle {
  std::shared_ptr<vacps::url::SearchParams> params;
  std::size_t index = 0;
  SpIterKind kind = SpIterKind::Entries;
};

JSClassID g_sp_iter_class_id = 0;

void sp_iter_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<SearchParamsIterHandle*>(JS_GetOpaque(val, g_sp_iter_class_id));
  delete h;
}

JSClassDef g_sp_iter_class = {
    "URLSearchParams Iterator",
    .finalizer = sp_iter_finalizer,
};

void ensure_sp_iter_class(JSContext* ctx) {
  if (g_sp_iter_class_id == 0) {
    JS_NewClassID(&g_sp_iter_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_sp_iter_class_id)) {
    JS_NewClass(rt, g_sp_iter_class_id, &g_sp_iter_class);
  }
}

SearchParamsIterHandle* sp_iter_from_this(JSContext* ctx, JSValueConst this_val) {
  auto* h = static_cast<SearchParamsIterHandle*>(
      JS_GetOpaque2(ctx, this_val, g_sp_iter_class_id));
  if (h == nullptr) {
    JS_ThrowTypeError(ctx, "URLSearchParams Iterator method requires an iterator instance");
    return nullptr;
  }
  return h;
}

JSValue make_sp_iterator(
    JSContext* ctx,
    std::shared_ptr<vacps::url::SearchParams> params,
    SpIterKind kind) {
  ensure_sp_iter_class(ctx);
  auto* handle = new SearchParamsIterHandle{std::move(params), 0, kind};
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(g_sp_iter_class_id));
  if (JS_IsException(obj)) {
    delete handle;
    return obj;
  }
  JS_SetOpaque(obj, handle);
  return obj;
}

JSValue js_sp_iter_next(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = sp_iter_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;

  auto pair = h->params->at(h->index);
  if (!pair) {
    return make_iter_result(ctx, JS_UNDEFINED, true);
  }
  ++h->index;

  JSValue value = JS_UNDEFINED;
  switch (h->kind) {
    case SpIterKind::Keys:
      value = js_string_owned(ctx, pair->first);
      break;
    case SpIterKind::Values:
      value = js_string_owned(ctx, pair->second);
      break;
    case SpIterKind::Entries: {
      value = JS_NewArray(ctx);
      if (JS_IsException(value)) return value;
      JSValue k = js_string_owned(ctx, pair->first);
      JSValue v = js_string_owned(ctx, pair->second);
      if (JS_IsException(k) || JS_IsException(v)) {
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
      }
      JS_SetPropertyUint32(ctx, value, 0, k);
      JS_SetPropertyUint32(ctx, value, 1, v);
      break;
    }
  }
  if (JS_IsException(value)) return value;
  return make_iter_result(ctx, value, false);
}

/** Iterator is iterable: @@iterator returns this. */
JSValue js_sp_iter_self(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  if (!sp_iter_from_this(ctx, this_val)) return JS_EXCEPTION;
  return JS_DupValue(ctx, this_val);
}

void install_sp_iter_proto(JSContext* ctx) {
  ensure_sp_iter_class(ctx);
  // Only install proto once (class registered once per runtime).
  JSValue existing = JS_GetClassProto(ctx, g_sp_iter_class_id);
  if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
    // Class proto is set; if already configured with next, skip rebuild.
    JSValue next = JS_GetPropertyStr(ctx, existing, "next");
    const bool has_next = JS_IsFunction(ctx, next);
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, existing);
    if (has_next) {
      return;
    }
  } else {
    JS_FreeValue(ctx, existing);
  }

  Value proto{ctx, JS_NewObject(ctx)};
  if (proto.is_exception()) {
    return;
  }
  JS_SetPropertyStr(ctx, proto.get(), "next",
                    JS_NewCFunction(ctx, js_sp_iter_next, "next", 0));

  JSAtom iter_atom = JS_ATOM_NULL;
  if (symbol_iterator_atom(ctx, &iter_atom)) {
    JS_DefinePropertyValue(
        ctx,
        proto.get(),
        iter_atom,
        JS_NewCFunction(ctx, js_sp_iter_self, "[Symbol.iterator]", 0),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, iter_atom);
  }

  JS_SetClassProto(ctx, g_sp_iter_class_id, proto.release());
}

/**
 * new URLSearchParams([init])
 * init: omitted / null / undefined → empty; string → parse (leading '?' ok).
 * Sequence/record init is not implemented (domain has string init only).
 */
JSValue js_search_params_constructor(
    JSContext* ctx,
    JSValueConst /*new_target*/,
    int argc,
    JSValueConst* argv) {
  auto params = std::make_shared<vacps::url::SearchParams>();
  if (argc >= 1 && !is_nullish(argv[0])) {
    auto s = js_to_string(ctx, argv[0]);
    if (!s) {
      return JS_ThrowTypeError(ctx, "URLSearchParams: init must be a string");
    }
    params = std::make_shared<vacps::url::SearchParams>(*s);
  }
  return make_search_params_js(ctx, std::move(params));
}

JSValue js_search_params_append(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.append requires name and value");
  }
  auto key = js_to_string(ctx, argv[0]);
  auto value = js_to_string(ctx, argv[1]);
  if (!key || !value) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.append: name and value must be strings");
  }
  h->params->append(*key, *value);
  return JS_UNDEFINED;
}

JSValue js_search_params_set(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.set requires name and value");
  }
  auto key = js_to_string(ctx, argv[0]);
  auto value = js_to_string(ctx, argv[1]);
  if (!key || !value) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.set: name and value must be strings");
  }
  h->params->set(*key, *value);
  return JS_UNDEFINED;
}

JSValue js_search_params_get(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.get requires name");
  }
  auto key = js_to_string(ctx, argv[0]);
  if (!key) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.get: name must be a string");
  }
  auto v = h->params->get(*key);
  if (!v) return JS_NULL;
  return js_string_owned(ctx, *v);
}

JSValue js_search_params_get_all(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.getAll requires name");
  }
  auto key = js_to_string(ctx, argv[0]);
  if (!key) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.getAll: name must be a string");
  }
  auto all = h->params->get_all(*key);
  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr)) return arr;
  for (uint32_t i = 0; i < static_cast<uint32_t>(all.size()); ++i) {
    JSValue item = js_string_owned(ctx, all[i]);
    if (JS_IsException(item)) {
      JS_FreeValue(ctx, arr);
      return item;
    }
    JS_SetPropertyUint32(ctx, arr, i, item);
  }
  return arr;
}

JSValue js_search_params_has(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.has requires name");
  }
  auto key = js_to_string(ctx, argv[0]);
  if (!key) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.has: name must be a string");
  }
  bool found = false;
  if (argc >= 2 && !is_nullish(argv[1])) {
    auto value = js_to_string(ctx, argv[1]);
    if (!value) {
      return JS_ThrowTypeError(ctx, "URLSearchParams.has: value must be a string");
    }
    found = h->params->has(*key, *value);
  } else {
    found = h->params->has(*key);
  }
  return converter<bool>::to_js(ctx, found).release();
}

/** JS `delete` — C++ uses remove. */
JSValue js_search_params_delete(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.delete requires name");
  }
  auto key = js_to_string(ctx, argv[0]);
  if (!key) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.delete: name must be a string");
  }
  if (argc >= 2 && !is_nullish(argv[1])) {
    auto value = js_to_string(ctx, argv[1]);
    if (!value) {
      return JS_ThrowTypeError(ctx, "URLSearchParams.delete: value must be a string");
    }
    h->params->remove(*key, *value);
  } else {
    h->params->remove(*key);
  }
  return JS_UNDEFINED;
}

JSValue js_search_params_sort(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  h->params->sort();
  return JS_UNDEFINED;
}

JSValue js_search_params_to_string(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_owned(ctx, h->params->to_string());
}

JSValue js_search_params_get_size(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return JS_NewUint32(ctx, static_cast<uint32_t>(h->params->size()));
}

JSValue js_search_params_entries(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  install_sp_iter_proto(ctx);
  return make_sp_iterator(ctx, h->params, SpIterKind::Entries);
}

JSValue js_search_params_keys(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  install_sp_iter_proto(ctx);
  return make_sp_iterator(ctx, h->params, SpIterKind::Keys);
}

JSValue js_search_params_values(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  install_sp_iter_proto(ctx);
  return make_sp_iterator(ctx, h->params, SpIterKind::Values);
}

/**
 * forEach(callback[, thisArg])
 * WHATWG order: callback(value, name, searchParams).
 */
JSValue js_search_params_for_each(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = search_params_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "URLSearchParams.forEach requires a function");
  }
  JSValueConst callback = argv[0];
  JSValueConst this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

  // Index walk re-reads size each step so appends during forEach are visible;
  // deletions may skip (acceptable WHATWG-adjacent subset).
  std::size_t i = 0;
  while (i < h->params->size()) {
    auto pair = h->params->at(i);
    if (!pair) {
      break;
    }
    JSValue args[3];
    args[0] = js_string_owned(ctx, pair->second);  // value
    args[1] = js_string_owned(ctx, pair->first);   // name
    args[2] = this_val;                            // parent (borrowed)
    if (JS_IsException(args[0]) || JS_IsException(args[1])) {
      JS_FreeValue(ctx, args[0]);
      JS_FreeValue(ctx, args[1]);
      return JS_EXCEPTION;
    }
    JSValue ret = JS_Call(ctx, callback, this_arg, 3, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(ret)) {
      return ret;
    }
    JS_FreeValue(ctx, ret);
    ++i;
  }
  return JS_UNDEFINED;
}

}  // namespace

JSValue make_search_params_js(
    JSContext* ctx,
    std::shared_ptr<vacps::url::SearchParams> params) {
  ensure_search_params_class(ctx);
  auto* handle = new SearchParamsHandle{std::move(params)};
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(g_search_params_class_id));
  if (JS_IsException(obj)) {
    delete handle;
    return obj;
  }
  JS_SetOpaque(obj, handle);
  return obj;
}

VoidResult install_url_search_params_binding(JSContext* ctx) {
  if (ctx == nullptr) {
    return std::unexpected(Error{"install_url_search_params_binding: null context"});
  }

  ensure_search_params_class(ctx);
  install_sp_iter_proto(ctx);

  Value proto{ctx, JS_NewObject(ctx)};
  if (proto.is_exception()) {
    return std::unexpected(Error{"install_url_search_params_binding: proto"});
  }

  JS_SetPropertyStr(ctx, proto.get(), "append",
                    JS_NewCFunction(ctx, js_search_params_append, "append", 2));
  JS_SetPropertyStr(ctx, proto.get(), "set",
                    JS_NewCFunction(ctx, js_search_params_set, "set", 2));
  JS_SetPropertyStr(ctx, proto.get(), "get",
                    JS_NewCFunction(ctx, js_search_params_get, "get", 1));
  JS_SetPropertyStr(ctx, proto.get(), "getAll",
                    JS_NewCFunction(ctx, js_search_params_get_all, "getAll", 1));
  JS_SetPropertyStr(ctx, proto.get(), "has",
                    JS_NewCFunction(ctx, js_search_params_has, "has", 1));
  JS_SetPropertyStr(ctx, proto.get(), "delete",
                    JS_NewCFunction(ctx, js_search_params_delete, "delete", 1));
  JS_SetPropertyStr(ctx, proto.get(), "sort",
                    JS_NewCFunction(ctx, js_search_params_sort, "sort", 0));
  JS_SetPropertyStr(ctx, proto.get(), "toString",
                    JS_NewCFunction(ctx, js_search_params_to_string, "toString", 0));
  define_getter(ctx, proto.get(), "size", js_search_params_get_size);

  // entries + @@iterator share the same function object.
  JSValue entries_fn =
      JS_NewCFunction(ctx, js_search_params_entries, "entries", 0);
  JS_SetPropertyStr(ctx, proto.get(), "entries", JS_DupValue(ctx, entries_fn));
  JS_SetPropertyStr(ctx, proto.get(), "keys",
                    JS_NewCFunction(ctx, js_search_params_keys, "keys", 0));
  JS_SetPropertyStr(ctx, proto.get(), "values",
                    JS_NewCFunction(ctx, js_search_params_values, "values", 0));
  JS_SetPropertyStr(ctx, proto.get(), "forEach",
                    JS_NewCFunction(ctx, js_search_params_for_each, "forEach", 1));

  // @@iterator → same as entries (for-of yields [name, value]).
  JSAtom iter_atom = JS_ATOM_NULL;
  if (symbol_iterator_atom(ctx, &iter_atom)) {
    JS_DefinePropertyValue(
        ctx,
        proto.get(),
        iter_atom,
        entries_fn,  // takes remaining ref from NewCFunction
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, iter_atom);
  } else {
    JS_FreeValue(ctx, entries_fn);
  }

  JS_SetClassProto(ctx, g_search_params_class_id, proto.release());

  JSValue ctor = JS_NewCFunction2(
      ctx, js_search_params_constructor, "URLSearchParams", 1, JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return std::unexpected(Error{"install_url_search_params_binding: constructor"});
  }
  JSValue class_proto = JS_GetClassProto(ctx, g_search_params_class_id);
  JS_DefinePropertyValueStr(
      ctx, ctor, "prototype", class_proto, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

  JSValue global = JS_GetGlobalObject(ctx);
  JS_DefinePropertyValueStr(
      ctx, global, "URLSearchParams", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, global);
  return {};
}

}  // namespace vacps::js
