#pragma once

#include "storage/db.hpp"
#include "app/error.hpp"
#include "quickjs/convert.hpp"
#include "quickjs/host.hpp"
#include "quickjs/value.hpp"

#include <quickjs.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vacps::js {

inline JSValue throw_msg(JSContext* ctx, const char* msg) {
  return JS_ThrowInternalError(ctx, "%s", msg);
}

inline JSValue throw_error(JSContext* ctx, const Error& e) {
  return throw_msg(ctx, e.message.c_str());
}

/** Parse JS array of bind params → SqlValue list. */
inline Result<std::vector<SqlValue>> sql_params_from_js(JSContext* ctx, JSValueConst v) {
  if (is_nullish(v)) {
    return std::vector<SqlValue>{};
  }
  if (!is_array(ctx, v)) {
    return std::unexpected(Error{"params must be an array"});
  }

  Value len_v = Value::get_property_str(ctx, v, "length");
  auto len_r = converter<std::int32_t>::from_js(ctx, len_v.get());
  if (!len_r || *len_r < 0) {
    return std::unexpected(Error{"invalid params length"});
  }
  const auto len = static_cast<std::uint32_t>(*len_r);

  std::vector<SqlValue> out;
  out.reserve(len);
  for (std::uint32_t i = 0; i < len; ++i) {
    Value el = Value::get_property_uint32(ctx, v, i);
    if (el.is_exception()) {
      return std::unexpected(Error{"params element exception"});
    }
    if (el.is_nullish()) {
      out.push_back(sql_null());
    } else if (el.is_bool()) {
      auto b = converter<bool>::from_js(ctx, el.get());
      if (!b) {
        return std::unexpected(std::move(b.error()));
      }
      out.push_back(sql_int(*b ? 1 : 0));
    } else if (el.is_number() || el.is_bigint()) {
      auto d = converter<double>::from_js(ctx, el.get());
      if (!d) {
        return std::unexpected(std::move(d.error()));
      }
      if (*d == static_cast<double>(static_cast<std::int64_t>(*d))) {
        out.push_back(sql_int(static_cast<std::int64_t>(*d)));
      } else {
        out.push_back(sql_real(*d));
      }
    } else if (el.is_string()) {
      auto s = converter<std::string>::from_js(ctx, el.get());
      if (!s) {
        return std::unexpected(std::move(s.error()));
      }
      out.push_back(sql_text(std::move(*s)));
    } else {
      size_t size = 0;
      uint8_t* buf = JS_GetArrayBuffer(ctx, &size, el.get());
      if (buf != nullptr) {
        out.push_back(sql_blob(std::vector<std::uint8_t>(buf, buf + size)));
        continue;
      }
      size_t byte_offset = 0;
      size_t byte_length = 0;
      size_t bytes_per_element = 0;
      Value ab{
          ctx,
          JS_GetTypedArrayBuffer(ctx, el.get(), &byte_offset, &byte_length, &bytes_per_element)};
      if (!ab.is_exception() && !ab.is_undefined()) {
        size_t ab_size = 0;
        uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, ab.get());
        if (ab_ptr != nullptr && byte_offset <= ab_size &&
            byte_length <= ab_size - byte_offset) {
          out.push_back(sql_blob(std::vector<std::uint8_t>(
              ab_ptr + byte_offset, ab_ptr + byte_offset + byte_length)));
          continue;
        }
      } else if (ab.is_exception()) {
        Value ex{ctx, JS_GetException(ctx)};
        (void)ex;
      }
      return std::unexpected(Error{"unsupported bind param type"});
    }
  }
  return out;
}

inline Value sql_value_to_js(JSContext* ctx, const SqlValue& v) {
  return std::visit(
      [&](const auto& x) -> Value {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return Value{ctx, JS_NULL};
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return converter<std::int64_t>::to_js(ctx, x);
        } else if constexpr (std::is_same_v<T, double>) {
          return converter<double>::to_js(ctx, x);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return converter<std::string>::to_js(ctx, x);
        } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
          return Value{ctx, JS_NewArrayBufferCopy(ctx, x.data(), x.size())};
        } else {
          return Value{ctx, JS_UNDEFINED};
        }
      },
      v);
}

inline Value query_result_to_js(JSContext* ctx, const QueryResult& qr) {
  auto arr = Value::new_array(ctx);
  if (arr.is_exception()) {
    return arr;
  }
  for (std::uint32_t r = 0; r < qr.rows.size(); ++r) {
    auto obj = Value::new_object(ctx);
    if (obj.is_exception()) {
      return obj;
    }
    const auto& row = qr.rows[r];
    for (std::size_t c = 0; c < qr.columns.size() && c < row.size(); ++c) {
      obj.set_property_str(qr.columns[c].c_str(), sql_value_to_js(ctx, row[c]));
    }
    arr.set_property_uint32(r, std::move(obj));
  }
  return arr;
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

}  // namespace vacps::js
