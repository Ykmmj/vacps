#pragma once

/**
 * Module-local JS ↔ C++ convert for vacps:fs (ClassBuilder / free functions).
 */

#include "binding/convert.hpp"
#include "fs/file.hpp"
#include "fs/fs.hpp"
#include "fs/open_options.hpp"

#include <quickjs.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js::fs_module {

/**
 * FS write payload: ArrayBuffer / TypedArray only.
 * Rejects strings so other modules can keep the shared vector converter.
 */
struct StrictBytes {
  std::vector<std::uint8_t> data;
};

}  // namespace vacps::js::fs_module

namespace vacps::binding {

// Alias for method signatures in modules/fs.cpp
namespace fs_module = vacps::js::fs_module;

/** Optional trailing number args (missing / null / undefined → nullopt). */
template <>
struct Converter<std::optional<std::uint64_t>> {
  static Result<std::optional<std::uint64_t>> from_js(Env env, JSValueConst v) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
      return std::optional<std::uint64_t>{};
    }
    auto n = Converter<std::uint64_t>::from_js(env, v);
    if (!n) {
      return std::unexpected(std::move(n.error()));
    }
    return std::optional<std::uint64_t>{*n};
  }
};

namespace fs_detail {

[[nodiscard]] inline bool is_nullish(JSValueConst v) noexcept {
  return JS_IsUndefined(v) || JS_IsNull(v);
}

[[nodiscard]] inline Result<qjs::OwnedValue> get_prop(
    Env env,
    JSValueConst obj,
    const char* name) {
  JSContext* ctx = env.context();
  qjs::OwnedValue v = qjs::OwnedValue::get_property_str(ctx, obj, name);
  if (v.is_exception()) {
    clear_exception(ctx);
    (void)v.release();
    return std::unexpected(Error::type(
        std::string{"failed to read property '"} + name + "'"));
  }
  return v;
}

[[nodiscard]] inline Result<void> require_plain_object(
    Env env,
    JSValueConst v,
    const char* label) {
  JSContext* ctx = env.context();
  if (!JS_IsObject(v)) {
    return std::unexpected(
        Error::type(std::string{"expected object ("} + label + ")"));
  }
  const int is_arr = JS_IsArray(ctx, v);
  if (is_arr < 0) {
    clear_exception(ctx);
    return std::unexpected(
        Error::type(std::string{"expected object ("} + label + ")"));
  }
  if (is_arr != 0) {
    return std::unexpected(
        Error::type(std::string{"expected object ("} + label + ")"));
  }
  return {};
}

}  // namespace fs_detail

template <>
struct Converter<vacps::fs::OpenOptions> {
  static Result<vacps::fs::OpenOptions> from_js(Env env, JSValueConst v) {
    using vacps::fs::OpenOptions;
    using vacps::fs::OpenMode;

    // Canonical shape only: { mode: string, permissions?: number }
    if (auto ok = fs_detail::require_plain_object(env, v, "FileOpenOptions");
        !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    auto mode_v = fs_detail::get_prop(env, v, "mode");
    if (!mode_v) {
      return std::unexpected(std::move(mode_v.error()));
    }
    auto mode_s = Converter<std::string>::from_js(env, mode_v->get());
    if (!mode_s) {
      return std::unexpected(std::move(mode_s.error()));
    }
    auto mode = vacps::fs::open_mode_from_string(*mode_s);
    if (!mode) {
      return std::unexpected(Error::type(std::move(mode.error().message)));
    }
    OpenOptions opts;
    opts.mode = *mode;

    auto perm_v = fs_detail::get_prop(env, v, "permissions");
    if (!perm_v) {
      return std::unexpected(std::move(perm_v.error()));
    }
    if (!fs_detail::is_nullish(perm_v->get())) {
      auto p = Converter<std::uint32_t>::from_js(env, perm_v->get());
      if (!p) {
        return std::unexpected(std::move(p.error()));
      }
      // Only mode bits 0777 — reject setuid/setgid/sticky and higher bits.
      if ((*p & ~static_cast<std::uint32_t>(0777)) != 0) {
        return std::unexpected(Error::type(
            "File.open: permissions must be within 0o777"));
      }
      opts.permissions = *p;
    }
    return opts;
  }
};

/**
 * Strict FS write bytes: ArrayBuffer or TypedArray only (no string).
 * Does not use Converter<vector<uint8_t>> which accepts strings.
 */
template <>
struct Converter<vacps::js::fs_module::StrictBytes> {
  static Result<vacps::js::fs_module::StrictBytes> from_js(
      Env env,
      JSValueConst v) {
    JSContext* ctx = env.context();
    if (JS_IsString(v)) {
      return std::unexpected(Error::type(
          "expected ArrayBuffer or TypedArray (string writes are not "
          "supported; use TextEncoder)"));
    }

    size_t byte_offset = 0;
    size_t byte_length = 0;
    size_t bytes_per_element = 0;
    qjs::OwnedValue ab{
        ctx,
        JS_GetTypedArrayBuffer(
            ctx, v, &byte_offset, &byte_length, &bytes_per_element)};
    if (!ab.is_exception()) {
      size_t ab_size = 0;
      uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_size, ab.get());
      if (ab_ptr == nullptr) {
        clear_exception(ctx);
        return std::unexpected(Error::type(
            "expected ArrayBuffer or TypedArray"));
      }
      if (byte_offset > ab_size || byte_length > ab_size - byte_offset) {
        return std::unexpected(Error::range("TypedArray bounds invalid"));
      }
      vacps::js::fs_module::StrictBytes out;
      out.data.assign(ab_ptr + byte_offset, ab_ptr + byte_offset + byte_length);
      return out;
    }
    clear_exception(ctx);
    (void)ab.release();

    size_t size = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &size, v);
    if (buf == nullptr) {
      clear_exception(ctx);
      return std::unexpected(Error::type(
          "expected ArrayBuffer or TypedArray"));
    }
    vacps::js::fs_module::StrictBytes out;
    out.data.assign(buf, buf + size);
    return out;
  }
};

template <>
struct Converter<vacps::fs::MkdirOptions> {
  static Result<vacps::fs::MkdirOptions> from_js(Env env, JSValueConst v) {
    vacps::fs::MkdirOptions opts{};
    if (fs_detail::is_nullish(v)) {
      return opts;
    }
    if (auto ok = fs_detail::require_plain_object(env, v, "MkdirOptions"); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    auto rec = fs_detail::get_prop(env, v, "recursive");
    if (!rec) {
      return std::unexpected(std::move(rec.error()));
    }
    if (!fs_detail::is_nullish(rec->get())) {
      auto b = Converter<bool>::from_js(env, rec->get());
      if (!b) {
        return std::unexpected(std::move(b.error()));
      }
      opts.recursive = *b;
    }
    return opts;
  }
};

template <>
struct Converter<vacps::fs::RemoveOptions> {
  static Result<vacps::fs::RemoveOptions> from_js(Env env, JSValueConst v) {
    vacps::fs::RemoveOptions opts{};
    if (fs_detail::is_nullish(v)) {
      return opts;
    }
    if (auto ok = fs_detail::require_plain_object(env, v, "RemoveOptions");
        !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    auto rec = fs_detail::get_prop(env, v, "recursive");
    if (!rec) {
      return std::unexpected(std::move(rec.error()));
    }
    if (!fs_detail::is_nullish(rec->get())) {
      auto b = Converter<bool>::from_js(env, rec->get());
      if (!b) {
        return std::unexpected(std::move(b.error()));
      }
      opts.recursive = *b;
    }
    return opts;
  }
};

template <>
struct Converter<vacps::fs::RenameOptions> {
  static Result<vacps::fs::RenameOptions> from_js(Env env, JSValueConst v) {
    vacps::fs::RenameOptions opts{};
    if (fs_detail::is_nullish(v)) {
      return opts;
    }
    if (auto ok = fs_detail::require_plain_object(env, v, "RenameOptions");
        !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    auto rep = fs_detail::get_prop(env, v, "replace");
    if (!rep) {
      return std::unexpected(std::move(rep.error()));
    }
    if (!fs_detail::is_nullish(rep->get())) {
      auto b = Converter<bool>::from_js(env, rep->get());
      if (!b) {
        return std::unexpected(std::move(b.error()));
      }
      opts.replace = *b;
    }
    return opts;
  }
};

template <>
struct Converter<vacps::fs::FileStat> {
  static qjs::OwnedValue to_js(Env env, const vacps::fs::FileStat& st) {
    JSContext* ctx = env.context();
    auto obj = env.new_object();
    if (obj.is_exception()) {
      return obj;
    }
    auto set = [&](const char* key, qjs::OwnedValue val) -> bool {
      if (val.is_exception()) {
        return false;
      }
      if (JS_SetPropertyStr(ctx, obj.get(), key, val.release()) < 0) {
        return false;
      }
      return true;
    };
    if (!set("path", Converter<std::string>::to_js(env, st.path))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("type", Converter<std::string>::to_js(env, st.type))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set(
            "size",
            Converter<std::uint64_t>::to_js(env, st.size_bytes))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set(
            "mtimeMs",
            Converter<std::int64_t>::to_js(env, st.modified_at_ms))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("readable", Converter<bool>::to_js(env, st.readable))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("writable", Converter<bool>::to_js(env, st.writable))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("isSymlink", Converter<bool>::to_js(env, st.is_symlink))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return obj;
  }

  static qjs::OwnedValue to_js(Env env, vacps::fs::FileStat&& st) {
    return to_js(env, static_cast<const vacps::fs::FileStat&>(st));
  }
};

template <>
struct Converter<vacps::fs::DirEntry> {
  static qjs::OwnedValue to_js(Env env, const vacps::fs::DirEntry& e) {
    JSContext* ctx = env.context();
    auto obj = env.new_object();
    if (obj.is_exception()) {
      return obj;
    }
    auto set = [&](const char* key, qjs::OwnedValue val) -> bool {
      if (val.is_exception()) {
        return false;
      }
      if (JS_SetPropertyStr(ctx, obj.get(), key, val.release()) < 0) {
        return false;
      }
      return true;
    };
    if (!set("name", Converter<std::string>::to_js(env, e.name))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("isDir", Converter<bool>::to_js(env, e.is_dir))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("isFile", Converter<bool>::to_js(env, e.is_file))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("isSymlink", Converter<bool>::to_js(env, e.is_symlink))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    if (!set("size", Converter<std::uint64_t>::to_js(env, e.size))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return obj;
  }

  static qjs::OwnedValue to_js(Env env, vacps::fs::DirEntry&& e) {
    return to_js(env, static_cast<const vacps::fs::DirEntry&>(e));
  }
};

template <>
struct Converter<std::vector<vacps::fs::DirEntry>> {
  static qjs::OwnedValue to_js(
      Env env,
      const std::vector<vacps::fs::DirEntry>& entries) {
    JSContext* ctx = env.context();
    qjs::OwnedValue arr{ctx, JS_NewArray(ctx)};
    if (arr.is_exception()) {
      return arr;
    }
    std::uint32_t i = 0;
    for (const auto& e : entries) {
      auto item = Converter<vacps::fs::DirEntry>::to_js(env, e);
      if (item.is_exception()) {
        return item;
      }
      if (JS_SetPropertyUint32(ctx, arr.get(), i++, item.release()) < 0) {
        return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
      }
    }
    return arr;
  }

  static qjs::OwnedValue to_js(
      Env env,
      std::vector<vacps::fs::DirEntry>&& entries) {
    return to_js(env, static_cast<const std::vector<vacps::fs::DirEntry>&>(entries));
  }
};

}  // namespace vacps::binding
