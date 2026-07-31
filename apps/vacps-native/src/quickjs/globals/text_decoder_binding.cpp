#include "quickjs/globals/text_decoder_binding.hpp"

#include "quickjs/js_bridge.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/raii/cstring.hpp"
#include "quickjs/raii/value.hpp"
#include "text/decoder.hpp"

#include <quickjs.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::js {
namespace {

// ── TextDecoder (vacps::text::Decoder) ────────────────────────────

struct TextDecoderHandle {
  vacps::text::Decoder decoder;
};

JSClassID g_text_decoder_class_id = 0;

void text_decoder_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<TextDecoderHandle*>(JS_GetOpaque(val, g_text_decoder_class_id));
  delete h;
}

JSClassDef g_text_decoder_class = {
    "TextDecoder",
    .finalizer = text_decoder_finalizer,
};

void ensure_text_decoder_class(JSContext* ctx) {
  if (g_text_decoder_class_id == 0) {
    JS_NewClassID(&g_text_decoder_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_text_decoder_class_id)) {
    JS_NewClass(rt, g_text_decoder_class_id, &g_text_decoder_class);
  }
}

TextDecoderHandle* decoder_from_this(JSContext* ctx, JSValueConst this_val) {
  auto* h =
      static_cast<TextDecoderHandle*>(JS_GetOpaque2(ctx, this_val, g_text_decoder_class_id));
  if (h == nullptr) {
    JS_ThrowTypeError(ctx, "TextDecoder method requires a TextDecoder instance");
    return nullptr;
  }
  return h;
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

/** new TextDecoder(label = "utf-8", options = {}) */
JSValue js_text_decoder_constructor(
    JSContext* ctx,
    JSValueConst /*new_target*/,
    int argc,
    JSValueConst* argv) {
  std::string_view label = "utf-8";
  std::string label_owned;
  if (argc >= 1 && !is_nullish(argv[0])) {
    auto s = converter<std::string>::from_js(ctx, argv[0]);
    if (!s) {
      return JS_ThrowTypeError(ctx, "TextDecoder: label must be a string");
    }
    label_owned = std::move(*s);
    label = label_owned;
  }

  vacps::text::DecoderOptions opts{};
  if (argc >= 2 && is_object(argv[1])) {
    Value fatal_v = Value::get_property_str(ctx, argv[1], "fatal");
    if (!fatal_v.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, fatal_v.get());
      if (b) opts.fatal = *b;
    }
    Value bom_v = Value::get_property_str(ctx, argv[1], "ignoreBOM");
    if (!bom_v.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, bom_v.get());
      if (b) opts.ignore_bom = *b;
    }
  }

  auto decoder = vacps::text::Decoder::create(label, opts);
  if (!decoder) {
    return JS_ThrowRangeError(
        ctx,
        "TextDecoder: only utf-8 / utf-16le / utf-16be are supported (got '%.*s')",
        static_cast<int>(label.size()),
        label.data());
  }

  auto* handle = new TextDecoderHandle{std::move(*decoder)};

  ensure_text_decoder_class(ctx);
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(g_text_decoder_class_id));
  if (JS_IsException(obj)) {
    delete handle;
    return obj;
  }
  JS_SetOpaque(obj, handle);
  return obj;
}

JSValue js_text_decoder_get_encoding(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = decoder_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto enc = h->decoder.encoding();
  return JS_NewStringLen(ctx, enc.data(), enc.size());
}

JSValue js_text_decoder_get_fatal(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = decoder_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->decoder.fatal()).release();
}

JSValue js_text_decoder_get_ignore_bom(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = decoder_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->decoder.ignore_bom()).release();
}

/** TextDecoder.decode(input?, options?) → string */
JSValue js_text_decoder_decode(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* h = decoder_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;

  std::vector<std::uint8_t> bytes;
  if (argc >= 1 && !is_nullish(argv[0])) {
    auto b = bytes_from_js(ctx, argv[0]);
    if (!b) {
      return JS_ThrowTypeError(
          ctx, "TextDecoder.decode: expected ArrayBuffer, TypedArray, or string");
    }
    bytes = std::move(*b);
  }

  bool stream = false;
  if (argc >= 2 && is_object(argv[1])) {
    Value stream_v = Value::get_property_str(ctx, argv[1], "stream");
    if (!stream_v.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, stream_v.get());
      if (b) stream = *b;
    }
  }

  auto out = h->decoder.decode(bytes, stream);
  if (!out) {
    return JS_ThrowTypeError(ctx, "%s", out.error().message.c_str());
  }
  return JS_NewStringLen(ctx, out->data(), out->size());
}

}  // namespace

VoidResult install_text_decoder_binding(JSContext* ctx) {
  if (ctx == nullptr) {
    return std::unexpected(Error{"install_text_decoder_binding: null context"});
  }

  ensure_text_decoder_class(ctx);

  Value proto{ctx, JS_NewObject(ctx)};
  if (proto.is_exception()) {
    return std::unexpected(Error{"install_text_decoder_binding: proto"});
  }
  define_getter(ctx, proto.get(), "encoding", js_text_decoder_get_encoding);
  define_getter(ctx, proto.get(), "fatal", js_text_decoder_get_fatal);
  define_getter(ctx, proto.get(), "ignoreBOM", js_text_decoder_get_ignore_bom);
  JS_SetPropertyStr(
      ctx,
      proto.get(),
      "decode",
      JS_NewCFunction(ctx, js_text_decoder_decode, "decode", 1));
  JS_SetClassProto(ctx, g_text_decoder_class_id, proto.release());

  JSValue ctor =
      JS_NewCFunction2(ctx, js_text_decoder_constructor, "TextDecoder", 1, JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return std::unexpected(Error{"install_text_decoder_binding: constructor"});
  }
  JSValue class_proto = JS_GetClassProto(ctx, g_text_decoder_class_id);
  JS_DefinePropertyValueStr(
      ctx, ctor, "prototype", class_proto, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

  JSValue global = JS_GetGlobalObject(ctx);
  JS_DefinePropertyValueStr(
      ctx, global, "TextDecoder", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, global);

  return {};
}

}  // namespace vacps::js
