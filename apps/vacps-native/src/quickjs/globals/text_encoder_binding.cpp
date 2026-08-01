#include "quickjs/globals/text_encoder_binding.hpp"

#include "quickjs/js_bridge.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/raii/cstring.hpp"
#include "quickjs/raii/value.hpp"
#include "text/encoder.hpp"

#include <quickjs.h>

#include <cstdint>
#include <span>
#include <string>
#include <utility>

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
  // d.ts: encode(input?: string): Uint8Array
  return to_uint8_array(ctx, *bytes).release();
}

/**
 * Writable Uint8 view for encodeInto destination (Uint8Array / byte TypedArray).
 * Pointer remains valid while `ab_holder` is alive.
 */
struct MutableU8View {
  Value ab_holder;
  std::uint8_t* data{nullptr};
  std::size_t size{0};
};

Result<MutableU8View> mutable_u8_dest(JSContext* ctx, JSValueConst dest) {
  // Prefer TypedArray (Uint8Array); fall back to bare ArrayBuffer.
  size_t byte_offset = 0;
  size_t byte_length = 0;
  size_t bytes_per_element = 0;
  Value ab{
      ctx,
      JS_GetTypedArrayBuffer(ctx, dest, &byte_offset, &byte_length, &bytes_per_element)};
  if (!ab.is_exception() && !ab.is_undefined() && !ab.is_null()) {
    if (bytes_per_element != 1) {
      return std::unexpected(
          Error{"TextEncoder.encodeInto: destination must be a byte TypedArray"});
    }
    size_t ab_size = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, ab.get());
    if (ab_ptr == nullptr || byte_offset > ab_size ||
        byte_length > ab_size - byte_offset) {
      return std::unexpected(Error{"TextEncoder.encodeInto: detached or invalid buffer"});
    }
    return MutableU8View{std::move(ab), ab_ptr + byte_offset, byte_length};
  }
  if (ab.is_exception()) {
    Value ex{ctx, JS_GetException(ctx)};
    (void)ex;
  }

  size_t ab_size = 0;
  uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, dest);
  if (ab_ptr == nullptr) {
    return std::unexpected(
        Error{"TextEncoder.encodeInto: destination must be Uint8Array or ArrayBuffer"});
  }
  return MutableU8View{Value{}, ab_ptr, ab_size};
}

/**
 * TextEncoder.encodeInto(source, destination) → { read, written }.
 * Never writes a partial multi-byte UTF-8 sequence into destination.
 */
JSValue js_text_encoder_encode_into(
    JSContext* ctx,
    JSValueConst /*this_val*/,
    int argc,
    JSValueConst* argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "TextEncoder.encodeInto(source, destination)");
  }
  auto source = converter<std::string>::from_js(ctx, argv[0]);
  if (!source) {
    return JS_ThrowTypeError(ctx, "TextEncoder.encodeInto: source must be a string");
  }
  auto dest = mutable_u8_dest(ctx, argv[1]);
  if (!dest) {
    return JS_ThrowTypeError(ctx, "%s", dest.error().message.c_str());
  }
  auto r = vacps::text::Encoder::encode_into(
      *source, std::span<std::uint8_t>(dest->data, dest->size));
  if (!r) {
    return JS_ThrowTypeError(ctx, "%s", r.error().message.c_str());
  }
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) return obj;
  JS_SetPropertyStr(
      ctx, obj, "read", JS_NewUint32(ctx, static_cast<uint32_t>(r->read)));
  JS_SetPropertyStr(
      ctx, obj, "written", JS_NewUint32(ctx, static_cast<uint32_t>(r->written)));
  return obj;
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
  JS_SetPropertyStr(
      ctx,
      proto.get(),
      "encodeInto",
      JS_NewCFunction(ctx, js_text_encoder_encode_into, "encodeInto", 2));
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
