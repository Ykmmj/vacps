#include "globals/text_encoder_binding.hpp"

#include "binding/class.hpp"
#include "binding/coerce.hpp"
#include "qjs/owned_value.hpp"
#include "text/encoder.hpp"

#include <quickjs.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;

using EncoderBuilder = binding::ClassBuilder<vacps::text::Encoder>;

/**
 * Local ownership-safe Uint8Array builder.
 * Does not touch Converter<vector<uint8_t>> (that path yields ArrayBuffer).
 *
 * Ownership:
 * - ArrayBufferCopy is held in qjs::OwnedValue until NewTypedArray retains it.
 * - ab is freed after the TypedArray is created (engine holds its own ref).
 * - Returned qjs::OwnedValue owns the TypedArray; caller transfers or frees it.
 */
qjs::OwnedValue bytes_to_uint8_array(
    JSContext* ctx,
    const std::uint8_t* data,
    std::size_t len) {
  qjs::OwnedValue ab{ctx, JS_NewArrayBufferCopy(ctx, data, len)};
  if (ab.is_exception()) {
    return ab;
  }
  // JS_NewTypedArray ArrayBuffer branch reads argv[1]/argv[2]; pass undefined
  // so QuickJS applies default byteOffset/byteLength over the whole buffer.
  JSValueConst args[3] = {ab.get(), JS_UNDEFINED, JS_UNDEFINED};
  JSValue ua = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
  ab.reset();
  return qjs::OwnedValue::take(ctx, ua);
}

qjs::OwnedValue bytes_to_uint8_array(
    JSContext* ctx,
    const std::vector<std::uint8_t>& bytes) {
  return bytes_to_uint8_array(ctx, bytes.data(), bytes.size());
}

/**
 * Borrow a mutable byte destination for encodeInto.
 *
 * Ownership / lifetime:
 * - ab_holder keeps the underlying ArrayBuffer JSValue alive for the stack
 *   frame (TypedArray: GetTypedArrayBuffer return; ArrayBuffer: Dup of dest).
 * - data/size are valid only while ab_holder (and the call argv) live.
 * - Never capture data into shared_ptr, heap, async, or async run_blocking.
 */
struct BorrowedByteDest {
  qjs::OwnedValue ab_holder;
  std::uint8_t* data{nullptr};
  std::size_t size{0};
};

binding::Result<BorrowedByteDest> borrow_byte_dest(
    JSContext* ctx,
    JSValueConst dest) {
  size_t byte_offset = 0;
  size_t byte_length = 0;
  size_t bytes_per_element = 0;
  qjs::OwnedValue ab{
      ctx,
      JS_GetTypedArrayBuffer(
          ctx, dest, &byte_offset, &byte_length, &bytes_per_element)};
  if (!ab.is_exception() && !ab.is_undefined() && !ab.is_null()) {
    if (bytes_per_element != 1) {
      ab.reset();
      return std::unexpected(binding::Error::type(
          "TextEncoder.encodeInto: destination must be a byte TypedArray"));
    }
    size_t ab_size = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, ab.get());
    if (ab_ptr == nullptr || byte_offset > ab_size ||
        byte_length > ab_size - byte_offset) {
      ab.reset();
      return std::unexpected(binding::Error::type(
          "TextEncoder.encodeInto: detached or invalid buffer"));
    }
    return BorrowedByteDest{std::move(ab), ab_ptr + byte_offset, byte_length};
  }
  if (ab.is_exception()) {
    binding::clear_exception(ctx);
    (void)ab.release();
  } else {
    ab.reset();
  }

  // Raw ArrayBuffer: Dup so the buffer JSValue is held for this stack frame.
  size_t ab_size = 0;
  uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, dest);
  if (ab_ptr == nullptr) {
    return std::unexpected(binding::Error::type(
        "TextEncoder.encodeInto: destination must be Uint8Array or "
        "ArrayBuffer"));
  }
  return BorrowedByteDest{
      qjs::OwnedValue{ctx, JS_DupValue(ctx, dest)}, ab_ptr, ab_size};
}

binding::Result<qjs::OwnedValue> encode_into_result_object(
    JSContext* ctx,
    vacps::text::EncodeIntoResult r) {
  qjs::OwnedValue obj{ctx, JS_NewObject(ctx)};
  if (obj.is_exception()) {
    binding::clear_exception(ctx);
    (void)obj.release();
    return std::unexpected(
        binding::Error::internal("TextEncoder.encodeInto: result object"));
  }
  if (JS_SetPropertyStr(
          ctx,
          obj.get(),
          "read",
          JS_NewUint32(ctx, static_cast<uint32_t>(r.read))) < 0) {
    binding::clear_exception(ctx);
    return std::unexpected(
        binding::Error::internal("TextEncoder.encodeInto: set read"));
  }
  if (JS_SetPropertyStr(
          ctx,
          obj.get(),
          "written",
          JS_NewUint32(ctx, static_cast<uint32_t>(r.written))) < 0) {
    binding::clear_exception(ctx);
    return std::unexpected(
        binding::Error::internal("TextEncoder.encodeInto: set written"));
  }
  return obj;
}

binding::VoidResult install_text_encoder_class(binding::Env env) {
  EncoderBuilder cls{env, "TextEncoder"};

  cls.constructor(
      []() -> binding::Result<std::shared_ptr<vacps::text::Encoder>> {
        try {
          return std::make_shared<vacps::text::Encoder>();
        } catch (const std::bad_alloc&) {
          return std::unexpected(binding::Error::internal("allocation failed"));
        }
      },
      0);

  cls.readonly(
      "encoding",
      [](vacps::text::Encoder& /*self*/) {
        return std::string{vacps::text::Encoder::encoding()};
      });

  cls.method(
      "encode",
      [](const binding::CallbackInfo& info,
         vacps::text::Encoder& /*self*/) -> binding::Result<qjs::OwnedValue> {
        JSContext* c = info.context();

        // missing / nullish → empty string; non-nullish → WebIDL ToString.
        std::string input;
        if (info.length() >= 1 && !info[0].is_nullish()) {
          auto s = binding::try_coerce_string(c, info[0].get());
          if (!s) {
            return std::unexpected(binding::Error::type(
                "TextEncoder.encode: input must be a string"));
          }
          input = std::move(*s);
        }

        auto bytes = vacps::text::Encoder::encode(input);
        if (!bytes) {
          // Domain failures → TypeError (not InternalError via from_domain).
          return std::unexpected(
              binding::Error::type(std::move(bytes.error().message)));
        }

        qjs::OwnedValue ua = bytes_to_uint8_array(c, *bytes);
        if (ua.is_exception()) {
          binding::clear_exception(c);
          (void)ua.release();
          return std::unexpected(binding::Error::internal(
              "TextEncoder.encode: Uint8Array allocation failed"));
        }
        return ua;
      },
      1);

  cls.method(
      "encodeInto",
      [](const binding::CallbackInfo& info,
         vacps::text::Encoder& /*self*/) -> binding::Result<qjs::OwnedValue> {
        JSContext* c = info.context();
        if (info.length() < 2) {
          return std::unexpected(binding::Error::type(
              "TextEncoder.encodeInto(source, destination)"));
        }

        auto source = binding::try_coerce_string(c, info[0].get());
        if (!source) {
          return std::unexpected(binding::Error::type(
              "TextEncoder.encodeInto: source must be a string"));
        }

        // Synchronous borrow only — dest_hold stays on this stack frame.
        auto dest = borrow_byte_dest(c, info[1].get());
        if (!dest) {
          return std::unexpected(std::move(dest.error()));
        }

        // Call domain while ab_holder keeps the buffer pointer valid.
        // Do not store dest->data beyond this synchronous stack frame.
        auto encoded = vacps::text::Encoder::encode_into(
            *source,
            std::span<std::uint8_t>(dest->data, dest->size));
        // Drop buffer borrow immediately after the write.
        dest->ab_holder.reset();
        dest->data = nullptr;
        dest->size = 0;

        if (!encoded) {
          return std::unexpected(
              binding::Error::type(std::move(encoded.error().message)));
        }
        return encode_into_result_object(c, *encoded);
      },
      2);

  auto ctor = cls.commit();
  if (!ctor) {
    return std::unexpected(ctor.error());
  }

  // Escape hatch: install constructor on globalThis (Define takes ownership).
  JSContext* c = env.context();
  JSValue global = JS_GetGlobalObject(c);
  if (JS_IsException(global)) {
    binding::clear_exception(c);
    ctor->reset();
    return std::unexpected(
        binding::Error::internal("TextEncoder GetGlobalObject"));
  }
  JSValue raw_ctor = ctor->release();
  if (JS_DefinePropertyValueStr(
          c,
          global,
          "TextEncoder",
          raw_ctor,
          JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    binding::clear_exception(c);
    JS_FreeValue(c, global);
    return std::unexpected(
        binding::Error::internal("TextEncoder global define"));
  }
  JS_FreeValue(c, global);
  return {};
}

}  // namespace

VoidResult install_text_encoder_binding(JSContext* ctx) {
  try {
    binding::Env env{ctx};
    if (auto r = install_text_encoder_class(env); !r) {
      if (JS_HasException(ctx)) {
        binding::clear_exception(ctx);
      }
      return std::unexpected(Error{std::move(r.error().message)});
    }
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
      return std::unexpected(
          Error{"install_text_encoder_binding: pending exception"});
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
        Error{"install_text_encoder_binding: unknown failure"});
  }
}

}  // namespace vacps::js
