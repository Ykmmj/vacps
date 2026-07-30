#include "quickjs/url_globals.hpp"

#include "quickjs/convert.hpp"
#include "quickjs/cstring.hpp"
#include "quickjs/value.hpp"

#include <ada.h>
#include <quickjs.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vacps::js {
namespace {

struct UrlHandle {
  ada::url_aggregator url;
};

JSClassID g_url_class_id = 0;

void url_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<UrlHandle*>(JS_GetOpaque(val, g_url_class_id));
  delete h;
}

JSClassDef g_url_class = {
    "URL",
    .finalizer = url_finalizer,
};

void ensure_url_class(JSContext* ctx) {
  if (g_url_class_id == 0) {
    JS_NewClassID(&g_url_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_url_class_id)) {
    JS_NewClass(rt, g_url_class_id, &g_url_class);
  }
}

UrlHandle* url_from_this(JSContext* ctx, JSValueConst this_val) {
  auto* h = static_cast<UrlHandle*>(JS_GetOpaque2(ctx, this_val, g_url_class_id));
  if (h == nullptr) {
    JS_ThrowTypeError(ctx, "URL method requires a URL instance");
    return nullptr;
  }
  return h;
}

JSValue js_string_view(JSContext* ctx, std::string_view sv) {
  return JS_NewStringLen(ctx, sv.data(), static_cast<size_t>(sv.size()));
}

JSValue js_string_owned(JSContext* ctx, const std::string& s) {
  return JS_NewStringLen(ctx, s.data(), s.size());
}

std::optional<std::string> js_to_string(JSContext* ctx, JSValueConst v) {
  auto s = converter<std::string>::from_js(ctx, v);
  if (!s) return std::nullopt;
  return std::move(*s);
}

/** new URL(input, [base]) */
JSValue js_url_constructor(JSContext* ctx, JSValueConst /*new_target*/, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "URL constructor requires at least 1 argument");
  }
  auto input = js_to_string(ctx, argv[0]);
  if (!input) {
    return JS_ThrowTypeError(ctx, "Invalid URL");
  }

  std::optional<ada::url_aggregator> base_holder;
  const ada::url_aggregator* base_ptr = nullptr;
  if (argc >= 2 && !is_nullish(argv[1])) {
    auto base_s = js_to_string(ctx, argv[1]);
    if (!base_s) {
      return JS_ThrowTypeError(ctx, "Invalid base URL");
    }
    auto base = ada::parse<ada::url_aggregator>(*base_s);
    if (!base) {
      return JS_ThrowTypeError(ctx, "Invalid base URL");
    }
    base_holder = std::move(*base);
    base_ptr = &*base_holder;
  }

  auto parsed = base_ptr ? ada::parse<ada::url_aggregator>(*input, base_ptr)
                         : ada::parse<ada::url_aggregator>(*input);
  if (!parsed) {
    return JS_ThrowTypeError(ctx, "Invalid URL");
  }

  ensure_url_class(ctx);
  auto* handle = new UrlHandle{std::move(*parsed)};
  Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(g_url_class_id))};
  if (obj.is_exception()) {
    delete handle;
    return obj.release();
  }
  JS_SetOpaque(obj.get(), handle);

  // Prototype methods/getters live on class prototype (set at install).
  return obj.release();
}

// QuickJS getters are normal C functions: (ctx, this, argc, argv).
JSValue js_url_get_href(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_href());
}

JSValue js_url_get_protocol(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_protocol());
}

JSValue js_url_get_hostname(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_hostname());
}

JSValue js_url_get_host(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_host());
}

JSValue js_url_get_pathname(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_pathname());
}

JSValue js_url_get_search(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_search());
}

JSValue js_url_get_hash(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_hash());
}

JSValue js_url_get_port(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.get_port());
}

JSValue js_url_get_origin(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_owned(ctx, h->url.get_origin());
}

JSValue js_url_to_string(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  return js_url_get_href(ctx, this_val, argc, argv);
}

JSValue js_url_to_json(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  return js_url_get_href(ctx, this_val, argc, argv);
}

/** URL.canParse(input, [base]) — WHATWG static */
JSValue js_url_can_parse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return converter<bool>::to_js(ctx, false).release();
  }
  auto input = js_to_string(ctx, argv[0]);
  if (!input) {
    return converter<bool>::to_js(ctx, false).release();
  }
  if (argc >= 2 && !is_nullish(argv[1])) {
    auto base = js_to_string(ctx, argv[1]);
    if (!base) {
      return converter<bool>::to_js(ctx, false).release();
    }
    std::string_view base_sv{*base};
    return converter<bool>::to_js(ctx, ada::can_parse(*input, &base_sv)).release();
  }
  return converter<bool>::to_js(ctx, ada::can_parse(*input)).release();
}

void define_getter(JSContext* ctx, JSValueConst proto, const char* name, JSCFunction* getter) {
  JSValue get = JS_NewCFunction(ctx, getter, name, 0);
  JSAtom atom = JS_NewAtom(ctx, name);
  JS_DefinePropertyGetSet(ctx, proto, atom, get, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
}

}  // namespace

VoidResult install_url_global(JSContext* ctx) {
  if (ctx == nullptr) {
    return std::unexpected(Error{"install_url_global: null context"});
  }

  ensure_url_class(ctx);

  // Prototype with getters + toString/toJSON
  Value proto{ctx, JS_NewObject(ctx)};
  if (proto.is_exception()) {
    return std::unexpected(Error{"install_url_global: proto"});
  }

  define_getter(ctx, proto.get(), "href", js_url_get_href);
  define_getter(ctx, proto.get(), "protocol", js_url_get_protocol);
  define_getter(ctx, proto.get(), "hostname", js_url_get_hostname);
  define_getter(ctx, proto.get(), "host", js_url_get_host);
  define_getter(ctx, proto.get(), "pathname", js_url_get_pathname);
  define_getter(ctx, proto.get(), "search", js_url_get_search);
  define_getter(ctx, proto.get(), "hash", js_url_get_hash);
  define_getter(ctx, proto.get(), "port", js_url_get_port);
  define_getter(ctx, proto.get(), "origin", js_url_get_origin);

  JS_SetPropertyStr(ctx, proto.get(), "toString",
                    JS_NewCFunction(ctx, js_url_to_string, "toString", 0));
  JS_SetPropertyStr(ctx, proto.get(), "toJSON",
                    JS_NewCFunction(ctx, js_url_to_json, "toJSON", 0));

  JS_SetClassProto(ctx, g_url_class_id, proto.release());

  // Constructor function
  JSValue ctor = JS_NewCFunction2(ctx, js_url_constructor, "URL", 1, JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return std::unexpected(Error{"install_url_global: constructor"});
  }

  // URL.prototype already set via class; link ctor.prototype for JS convention
  JSValue class_proto = JS_GetClassProto(ctx, g_url_class_id);
  JS_DefinePropertyValueStr(ctx, ctor, "prototype", class_proto,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(ctx, ctor, "canParse",
                            JS_NewCFunction(ctx, js_url_can_parse, "canParse", 1),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

  JSValue global = JS_GetGlobalObject(ctx);
  JS_DefinePropertyValueStr(ctx, global, "URL", ctor,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, global);

  return {};
}

}  // namespace vacps::js
