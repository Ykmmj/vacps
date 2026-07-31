#include "quickjs/encoding_globals.hpp"

#include "quickjs/convert.hpp"
#include "quickjs/cstring.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/value.hpp"

#include <quickjs.h>
#include <simdutf.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::js {
namespace {

// ── TextEncoder ───────────────────────────────────────────────────

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
  return JS_NewString(ctx, "utf-8");
}

/**
 * TextEncoder.encode(input = "") → Uint8Array (UTF-8).
 * String → UTF-8 via QuickJS (JS_ToCString is UTF-8); simdutf validates.
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
  // JS_ToCString (via converter) yields UTF-8; simdutf validates the byte sequence.
  if (!input.empty() &&
      !simdutf::validate_utf8(input.data(), input.size())) {
    return JS_ThrowTypeError(ctx, "TextEncoder.encode: invalid UTF-8 from string");
  }
  return JS_NewArrayBufferCopy(
      ctx,
      reinterpret_cast<const uint8_t*>(input.data()),
      input.size());
}

// ── TextDecoder ───────────────────────────────────────────────────

struct TextDecoderHandle {
  bool fatal{false};
  bool ignore_bom{false};
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

/**
 * Decode UTF-8 bytes to a well-formed UTF-8 string using simdutf.
 * fatal: throw on invalid sequences; otherwise insert U+FFFD and continue.
 */
Result<std::string> utf8_bytes_to_js_utf8(
    const char* data,
    std::size_t len,
    bool fatal,
    bool ignore_bom) {
  std::size_t start = 0;
  if (ignore_bom && len >= 3 &&
      static_cast<unsigned char>(data[0]) == 0xEF &&
      static_cast<unsigned char>(data[1]) == 0xBB &&
      static_cast<unsigned char>(data[2]) == 0xBF) {
    start = 3;
  }
  data += start;
  len -= start;

  if (len == 0) {
    return std::string{};
  }

  if (simdutf::validate_utf8(data, len)) {
    return std::string{data, len};
  }
  if (fatal) {
    return std::unexpected(Error{"The encoded data was not valid"});
  }

  // Replacement mode: walk with convert_utf8_to_utf16le_with_errors, emit U+FFFD.
  std::vector<char16_t> utf16;
  utf16.reserve(len);
  std::size_t pos = 0;
  while (pos < len) {
    const char* chunk = data + pos;
    const std::size_t remain = len - pos;
    const std::size_t budget = simdutf::utf16_length_from_utf8(chunk, remain);
    std::vector<char16_t> tmp(budget == 0 ? 1 : budget);
    const simdutf::result res =
        simdutf::convert_utf8_to_utf16le_with_errors(chunk, remain, tmp.data());
    if (res.error == simdutf::error_code::SUCCESS) {
      utf16.insert(utf16.end(), tmp.data(), tmp.data() + res.count);
      break;
    }
    // res.count = index of first invalid byte in chunk
    const std::size_t err_at = res.count;
    if (err_at > 0) {
      const std::size_t written =
          simdutf::convert_utf8_to_utf16le(chunk, err_at, tmp.data());
      utf16.insert(utf16.end(), tmp.data(), tmp.data() + written);
      pos += err_at;
    }
    utf16.push_back(u'\uFFFD');
    // Skip at least the bad leading byte.
    if (pos < len) {
      ++pos;
    } else {
      break;
    }
  }

  const std::size_t out_budget =
      simdutf::utf8_length_from_utf16le(utf16.data(), utf16.size());
  std::string out(out_budget, '\0');
  const std::size_t written =
      simdutf::convert_utf16le_to_utf8(utf16.data(), utf16.size(), out.data());
  out.resize(written);
  return out;
}

/** new TextDecoder(label = "utf-8", options = {}) */
JSValue js_text_decoder_constructor(
    JSContext* ctx,
    JSValueConst /*new_target*/,
    int argc,
    JSValueConst* argv) {
  if (argc >= 1 && !is_nullish(argv[0])) {
    auto label = converter<std::string>::from_js(ctx, argv[0]);
    if (!label) {
      return JS_ThrowTypeError(ctx, "TextDecoder: label must be a string");
    }
    // Only UTF-8 (ASCII case-insensitive labels per Encoding Standard subset).
    std::string lower = *label;
    for (char& c : lower) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    // trim
    while (!lower.empty() && (lower.front() == ' ' || lower.front() == '\t')) {
      lower.erase(lower.begin());
    }
    while (!lower.empty() && (lower.back() == ' ' || lower.back() == '\t')) {
      lower.pop_back();
    }
    if (lower != "utf-8" && lower != "utf8" && lower != "unicode-1-1-utf-8") {
      return JS_ThrowRangeError(
          ctx, "TextDecoder: only utf-8 is supported (got '%s')", label->c_str());
    }
  }

  auto* handle = new TextDecoderHandle{};
  if (argc >= 2 && is_object(argv[1])) {
    Value fatal_v = Value::get_property_str(ctx, argv[1], "fatal");
    if (!fatal_v.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, fatal_v.get());
      if (b) handle->fatal = *b;
    }
    Value bom_v = Value::get_property_str(ctx, argv[1], "ignoreBOM");
    if (!bom_v.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, bom_v.get());
      if (b) handle->ignore_bom = *b;
    }
  }

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
    JSValueConst /*this_val*/,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  return JS_NewString(ctx, "utf-8");
}

JSValue js_text_decoder_get_fatal(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = decoder_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->fatal).release();
}

JSValue js_text_decoder_get_ignore_bom(
    JSContext* ctx,
    JSValueConst this_val,
    int /*argc*/,
    JSValueConst* /*argv*/) {
  auto* h = decoder_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->ignore_bom).release();
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

  // stream option ignored (no streaming decoder state for v1).
  auto out = utf8_bytes_to_js_utf8(
      reinterpret_cast<const char*>(bytes.data()),
      bytes.size(),
      h->fatal,
      h->ignore_bom);
  if (!out) {
    return JS_ThrowTypeError(ctx, "%s", out.error().message.c_str());
  }
  return JS_NewStringLen(ctx, out->data(), out->size());
}

void define_getter(
    JSContext* ctx,
    JSValueConst obj,
    const char* name,
    JSCFunction* getter) {
  JSValue get = JS_NewCFunction(ctx, getter, name, 0);
  JS_DefinePropertyGetSet(
      ctx,
      obj,
      JS_NewAtom(ctx, name),
      get,
      JS_UNDEFINED,
      JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

}  // namespace

VoidResult install_encoding_globals(JSContext* ctx) {
  if (ctx == nullptr) {
    return std::unexpected(Error{"install_encoding_globals: null context"});
  }

  ensure_text_encoder_class(ctx);
  ensure_text_decoder_class(ctx);

  // ── TextEncoder ──
  {
    Value proto{ctx, JS_NewObject(ctx)};
    if (proto.is_exception()) {
      return std::unexpected(Error{"install_encoding_globals: TextEncoder proto"});
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
      return std::unexpected(Error{"install_encoding_globals: TextEncoder constructor"});
    }
    JSValue class_proto = JS_GetClassProto(ctx, g_text_encoder_class_id);
    JS_DefinePropertyValueStr(
        ctx, ctor, "prototype", class_proto, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(
        ctx, global, "TextEncoder", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
  }

  // ── TextDecoder ──
  {
    Value proto{ctx, JS_NewObject(ctx)};
    if (proto.is_exception()) {
      return std::unexpected(Error{"install_encoding_globals: TextDecoder proto"});
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
      return std::unexpected(Error{"install_encoding_globals: TextDecoder constructor"});
    }
    JSValue class_proto = JS_GetClassProto(ctx, g_text_decoder_class_id);
    JS_DefinePropertyValueStr(
        ctx, ctor, "prototype", class_proto, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(
        ctx, global, "TextDecoder", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
  }

  return {};
}

}  // namespace vacps::js
