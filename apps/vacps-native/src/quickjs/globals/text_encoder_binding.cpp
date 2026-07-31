#include "quickjs/globals/text_encoder_binding.hpp"

#include "quickjs/raii/convert.hpp"
#include "quickjs/raii/cstring.hpp"
#include "quickjs/raii/value.hpp"
#include "text/encoder.hpp"

#include <quickjs.h>

#include <string>

namespace vacps::js {
namespace {

// ── TextEncoder (vacps::text::Encoder) ────────────────────────────

JSClassID g_text_encoder_class_id = 0;

void text_encoder_finalizer(JSRuntime* /*rt*/, JSValue /*val*/) {
  // Stateless class; no opaque.
}

JSClassDef g_text_encoder_class = {
    "TextEncoder",
    .finalizer = text_encoder_finalizer,
};

void ensure_text_encoder_class(JSContext* ctx) {
  if (g_text_encoder_class_id == 0) {
    JS_NewClassID(&g_text_encoder_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_text_encoder_class_id)) {
    JS_NewClass(rt, g_text_encoder_class_id, &g_text_encoder_class);
  }
}

void define_getter(
    JSContext* ctx,
    JSValueConst obj,
    const char* name,
    JSCFunction* getter) {
  JSValue get = JS_NewCFunction(ctx, getter, name, 0);
  JSAtom atom = JS_NewAtom(ctx, name);
  JS_DefinePropertyGetSet(
      ctx,
      obj,
      atom,
      get,
      JS_UNDEFINED,
      JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
}

/** new TextEncoder() */
JSValue js_text_encoder_constructor(
    JSContext* ctx,
    JSValueConst /*new_target*/,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  ensure_text_encoder_class(ctx);
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(g_text_encoder_class_id));
  if (JS_IsException(obj)) return obj;
  return obj;
}

JSValue js_text_encoder_get_encoding(
    JSContext* ctx,
    JSValueConst /*this_val*/,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  return JS_NewStringLen(
      ctx,
      vacps::text::Encoder::encoding().data(),
      vacps::text::Encoder::encoding().size());
}

/**
 * TextEncoder.encode(input = "") → Uint8Array (UTF-8).
 * Domain Encoder validates via simdutf.
 */
JSValue js_text_encoder_encode(
    JSContext* ctx,
    JSValueConst /*this_val*/,
    int argc,
    JSValueConst* argv) {
  std::string input;
  if (argc >= 1 && !is_nullish(argv[0])) {
    auto s = converter<std::string>::from_js(ctx, argv[0]);
    if (!s) {
      return JS_ThrowTypeError(ctx, "TextEncoder.encode: input must be a string");
    }
    input = std::move(*s);
  }
  auto bytes = vacps::text::Encoder::encode(input);
  if (!bytes) {
    return JS_ThrowTypeError(ctx, "%s", bytes.error().message.c_str());
  }
  return JS_NewArrayBufferCopy(
      ctx,
      bytes->data(),
      bytes->size());
}

}  // namespace

VoidResult install_text_encoder_binding(JSContext* ctx) {
  if (ctx == nullptr) {
    return std::unexpected(Error{"install_text_encoder_binding: null context"});
  }

  ensure_text_encoder_class(ctx);

  Value proto{ctx, JS_NewObject(ctx)};
  if (proto.is_exception()) {
    return std::unexpected(Error{"install_text_encoder_binding: proto"});
  }
  define_getter(ctx, proto.get(), "encoding", js_text_encoder_get_encoding);
  JS_SetPropertyStr(
      ctx,
      proto.get(),
      "encode",
      JS_NewCFunction(ctx, js_text_encoder_encode, "encode", 1));
  JS_SetClassProto(ctx, g_text_encoder_class_id, proto.release());

  JSValue ctor =
      JS_NewCFunction2(ctx, js_text_encoder_constructor, "TextEncoder", 0, JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return std::unexpected(Error{"install_text_encoder_binding: constructor"});
  }
  JSValue class_proto = JS_GetClassProto(ctx, g_text_encoder_class_id);
  JS_DefinePropertyValueStr(
      ctx, ctor, "prototype", class_proto, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

  JSValue global = JS_GetGlobalObject(ctx);
  JS_DefinePropertyValueStr(
      ctx, global, "TextEncoder", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, global);

  return {};
}

}  // namespace vacps::js
