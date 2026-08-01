#include "quickjs/globals/url_binding.hpp"

#include "quickjs/globals/url_search_params_binding.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/raii/cstring.hpp"
#include "quickjs/raii/value.hpp"
#include "url/url.hpp"

#include <quickjs.h>

#include <optional>
#include <string>
#include <string_view>

namespace vacps::js {
namespace {

// ── helpers ───────────────────────────────────────────────────────

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

void define_getter(JSContext* ctx, JSValueConst proto, const char* name, JSCFunction* getter) {
  JSValue get = JS_NewCFunction(ctx, getter, name, 0);
  JSAtom atom = JS_NewAtom(ctx, name);
  JS_DefinePropertyGetSet(ctx, proto, atom, get, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
}

void define_getter_setter(
    JSContext* ctx,
    JSValueConst proto,
    const char* name,
    JSCFunction* getter,
    JSCFunction* setter) {
  JSValue get = JS_NewCFunction(ctx, getter, name, 0);
  JSValue set = JS_NewCFunction(ctx, setter, name, 1);
  JSAtom atom = JS_NewAtom(ctx, name);
  JS_DefinePropertyGetSet(
      ctx, proto, atom, get, set, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
}

// ── URL (vacps::url::Url) ─────────────────────────────────────────

/**
 * Opaque for JS URL instances.
 * `search_params_js` caches the live URLSearchParams object so
 * `url.searchParams === url.searchParams`. Held without creating a JS
 * back-ref from SearchParams to URL (C++ owner link is on SearchParams).
 */
struct UrlHandle {
  vacps::url::Url url;
  JSValue search_params_js = JS_UNDEFINED;
};

JSClassID g_url_class_id = 0;

void url_finalizer(JSRuntime* rt, JSValue val) {
  auto* h = static_cast<UrlHandle*>(JS_GetOpaque(val, g_url_class_id));
  if (h == nullptr) {
    return;
  }
  if (!JS_IsUndefined(h->search_params_js)) {
    JS_FreeValueRT(rt, h->search_params_js);
    h->search_params_js = JS_UNDEFINED;
  }
  // Url destructor detaches live SearchParams owner pointer.
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

/** new URL(input, [base]) */
JSValue js_url_constructor(JSContext* ctx, JSValueConst /*new_target*/, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "URL constructor requires at least 1 argument");
  }
  auto input = js_to_string(ctx, argv[0]);
  if (!input) {
    return JS_ThrowTypeError(ctx, "Invalid URL");
  }

  Result<vacps::url::Url> parsed = (argc >= 2 && !is_nullish(argv[1]))
      ? [&]() -> Result<vacps::url::Url> {
          auto base_s = js_to_string(ctx, argv[1]);
          if (!base_s) {
            return std::unexpected(Error{"Invalid base URL"});
          }
          return vacps::url::Url::parse(*input, *base_s);
        }()
      : vacps::url::Url::parse(*input);

  if (!parsed) {
    const auto& msg = parsed.error().message;
    if (msg.find("base") != std::string::npos) {
      return JS_ThrowTypeError(ctx, "Invalid base URL");
    }
    return JS_ThrowTypeError(ctx, "Invalid URL");
  }

  ensure_url_class(ctx);
  auto* handle = new UrlHandle{std::move(*parsed), JS_UNDEFINED};
  Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(g_url_class_id))};
  if (obj.is_exception()) {
    delete handle;
    return obj.release();
  }
  JS_SetOpaque(obj.get(), handle);

  return obj.release();
}

JSValue js_url_get_href(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.href());
}

JSValue js_url_get_protocol(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.protocol());
}

JSValue js_url_get_hostname(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.hostname());
}

JSValue js_url_get_host(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.host());
}

JSValue js_url_get_pathname(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.pathname());
}

JSValue js_url_get_search(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.search());
}

/** URL.search = value — updates Ada and re-parses live searchParams. */
JSValue js_url_set_search(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) {
    h->url.set_search("");
    return JS_UNDEFINED;
  }
  auto s = js_to_string(ctx, argv[0]);
  if (!s) {
    return JS_ThrowTypeError(ctx, "URL.search must be a string");
  }
  h->url.set_search(*s);
  return JS_UNDEFINED;
}

JSValue js_url_get_hash(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.hash());
}

JSValue js_url_get_port(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.port());
}

JSValue js_url_get_origin(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_owned(ctx, h->url.origin());
}

JSValue js_url_get_username(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.username());
}

JSValue js_url_get_password(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return js_string_view(ctx, h->url.password());
}

/**
 * URL.searchParams → same live URLSearchParams for this URL instance.
 * Mutations update url.search / href via domain SearchParams → Url linkage.
 */
JSValue js_url_get_search_params(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = url_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;

  if (JS_IsUndefined(h->search_params_js)) {
    auto live = h->url.search_params();
    JSValue sp = make_search_params_js(ctx, std::move(live));
    if (JS_IsException(sp)) {
      return sp;
    }
    // Cache the single live object (refcount 1 held by handle).
    h->search_params_js = sp;
  }
  return JS_DupValue(ctx, h->search_params_js);
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
    return converter<bool>::to_js(
               ctx, vacps::url::Url::can_parse(*input, *base))
        .release();
  }
  return converter<bool>::to_js(ctx, vacps::url::Url::can_parse(*input)).release();
}

}  // namespace

VoidResult install_url_binding(JSContext* ctx) {
  if (ctx == nullptr) {
    return std::unexpected(Error{"install_url_binding: null context"});
  }

  ensure_url_class(ctx);

  Value proto{ctx, JS_NewObject(ctx)};
  if (proto.is_exception()) {
    return std::unexpected(Error{"install_url_binding: proto"});
  }

  define_getter(ctx, proto.get(), "href", js_url_get_href);
  define_getter(ctx, proto.get(), "protocol", js_url_get_protocol);
  define_getter(ctx, proto.get(), "username", js_url_get_username);
  define_getter(ctx, proto.get(), "password", js_url_get_password);
  define_getter(ctx, proto.get(), "hostname", js_url_get_hostname);
  define_getter(ctx, proto.get(), "host", js_url_get_host);
  define_getter(ctx, proto.get(), "pathname", js_url_get_pathname);
  define_getter_setter(ctx, proto.get(), "search", js_url_get_search, js_url_set_search);
  define_getter(ctx, proto.get(), "hash", js_url_get_hash);
  define_getter(ctx, proto.get(), "port", js_url_get_port);
  define_getter(ctx, proto.get(), "origin", js_url_get_origin);
  define_getter(ctx, proto.get(), "searchParams", js_url_get_search_params);

  JS_SetPropertyStr(ctx, proto.get(), "toString",
                    JS_NewCFunction(ctx, js_url_to_string, "toString", 0));
  JS_SetPropertyStr(ctx, proto.get(), "toJSON",
                    JS_NewCFunction(ctx, js_url_to_json, "toJSON", 0));

  JS_SetClassProto(ctx, g_url_class_id, proto.release());

  JSValue ctor = JS_NewCFunction2(ctx, js_url_constructor, "URL", 1, JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return std::unexpected(Error{"install_url_binding: constructor"});
  }

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
