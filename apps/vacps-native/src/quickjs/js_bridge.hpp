#pragma once

#include "app/error.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/raii/cstring.hpp"
#include "quickjs/raii/value.hpp"

#include <quickjs.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::js {

inline JSValue throw_msg(JSContext* ctx, const char* msg) {
  return JS_ThrowInternalError(ctx, "%s", msg);
}

inline JSValue throw_error(JSContext* ctx, const Error& e) {
  return throw_msg(ctx, e.message.c_str());
}

/**
 * Format a JS exception/rejection value for C++ Error::message.
 * Prefer Error-like objects: name, message, and stack when present.
 */
inline std::string format_js_exception(JSContext* ctx, JSValueConst ex) {
  if (ctx == nullptr) {
    return "js exception (no context)";
  }
  std::string name;
  std::string message;
  std::string stack;
  if (JS_IsObject(ex)) {
    Value name_v = Value::get_property_str(ctx, ex, "name");
    if (!name_v.is_nullish() && !name_v.is_exception()) {
      auto cs = CString::from_value(ctx, name_v.get());
      if (!cs.empty()) name = cs.str();
    } else if (name_v.is_exception()) {
      Value drop{ctx, JS_GetException(ctx)};
      (void)drop;
    }
    Value msg_v = Value::get_property_str(ctx, ex, "message");
    if (!msg_v.is_nullish() && !msg_v.is_exception()) {
      auto cs = CString::from_value(ctx, msg_v.get());
      if (!cs.empty()) message = cs.str();
    } else if (msg_v.is_exception()) {
      Value drop{ctx, JS_GetException(ctx)};
      (void)drop;
    }
    Value stack_v = Value::get_property_str(ctx, ex, "stack");
    if (!stack_v.is_nullish() && !stack_v.is_exception()) {
      auto cs = CString::from_value(ctx, stack_v.get());
      if (!cs.empty()) stack = cs.str();
    } else if (stack_v.is_exception()) {
      Value drop{ctx, JS_GetException(ctx)};
      (void)drop;
    }
  }
  if (message.empty()) {
    auto cs = CString::from_value(ctx, ex);
    if (!cs.empty()) {
      message = cs.str();
    }
  }
  if (message.empty()) {
    message = "js exception (unprintable)";
  }
  std::string out;
  if (!name.empty() && name != "Error") {
    out = name + ": " + message;
  } else {
    out = std::move(message);
  }
  if (!stack.empty()) {
    // QuickJS stack often already starts with "Error: msg\n    at ..."
    if (stack.find(out) == 0) {
      out = std::move(stack);
    } else {
      out.push_back('\n');
      out += stack;
    }
  }
  return out;
}

/** Build a JS Error object (name + message) for Promise rejections. */
inline Value make_js_error(
    JSContext* ctx,
    std::string_view message,
    std::string_view name = "Error") {
  Value err{ctx, JS_NewError(ctx)};
  if (err.is_exception()) {
    return err;
  }
  err.set_property_str("message", Value::new_string(ctx, message));
  err.set_property_str("name", Value::new_string(ctx, name));
  return err;
}

inline Result<std::vector<std::uint8_t>> bytes_from_js(JSContext* ctx, JSValueConst v) {
  if (is_string(v)) {
    auto s = converter<std::string>::from_js(ctx, v);
    if (!s) return std::unexpected(std::move(s.error()));
    return std::vector<std::uint8_t>(s->begin(), s->end());
  }
  size_t size = 0;
  uint8_t* buf = JS_GetArrayBuffer(ctx, &size, v);
  if (buf != nullptr) {
    return std::vector<std::uint8_t>(buf, buf + size);
  }
  size_t byte_offset = 0;
  size_t byte_length = 0;
  size_t bytes_per_element = 0;
  Value ab{
      ctx, JS_GetTypedArrayBuffer(ctx, v, &byte_offset, &byte_length, &bytes_per_element)};
  if (!ab.is_exception() && !ab.is_undefined()) {
    size_t ab_size = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, ab.get());
    if (ab_ptr != nullptr && byte_offset <= ab_size &&
        byte_length <= ab_size - byte_offset) {
      return std::vector<std::uint8_t>(ab_ptr + byte_offset, ab_ptr + byte_offset + byte_length);
    }
  } else if (ab.is_exception()) {
    Value ex{ctx, JS_GetException(ctx)};
    (void)ex;
  }
  return std::unexpected(Error{"expected string, ArrayBuffer, or TypedArray"});
}

inline Value bytes_to_js(JSContext* ctx, const std::vector<std::uint8_t>& bytes) {
  return Value{ctx, JS_NewArrayBufferCopy(ctx, bytes.data(), bytes.size())};
}

/**
 * Copy bytes into a new ArrayBuffer and wrap as Uint8Array (d.ts File/Process/TextEncoder).
 * Prefer this over bare ArrayBuffer when the public API promises Uint8Array.
 *
 * QuickJS `JS_NewTypedArray` is the C entry for `new Uint8Array(...)`. The
 * ArrayBuffer branch always reads argv[1] (byteOffset) and argv[2] (length);
 * calling it with argc==1 leaves those slots uninitialised and throws
 * RangeError "invalid length" / "invalid array index". Pass three args with
 * undefined offset/length so defaults apply (view over the whole buffer).
 */
inline Value to_uint8_array(
    JSContext* ctx,
    const std::uint8_t* data,
    std::size_t len) {
  JSValue ab = JS_NewArrayBufferCopy(ctx, data, len);
  if (JS_IsException(ab)) {
    return Value{ctx, ab};
  }
  JSValueConst args[3] = {ab, JS_UNDEFINED, JS_UNDEFINED};
  JSValue ua = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
  JS_FreeValue(ctx, ab);
  return Value{ctx, ua};
}

inline Value to_uint8_array(JSContext* ctx, const std::vector<std::uint8_t>& bytes) {
  return to_uint8_array(ctx, bytes.data(), bytes.size());
}

inline Value to_uint8_array(JSContext* ctx, std::string_view bytes) {
  return to_uint8_array(
      ctx,
      reinterpret_cast<const std::uint8_t*>(bytes.data()),
      bytes.size());
}

}  // namespace vacps::js
