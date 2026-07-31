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
