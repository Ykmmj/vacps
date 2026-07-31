#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/bindings/common.hpp"

#include "fs/async.hpp"
#include "fs/file.hpp"
#include "fs/fs.hpp"
#include "fs/open_options.hpp"
#include "quickjs/class_binding.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/promise_bridge.hpp"
#include "quickjs/raii/value.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <quickjs.h>

#include <fcntl.h>

namespace vacps::js {
namespace {

/**
 * C++ vacps:fs is pure I/O.
 * Path allowlist policy lives in JS path-guard.ts (no C++ path-policy layer).
 */
Result<std::filesystem::path> resolve_under_data(ScriptRuntime* host, std::string_view user_path) {
  return vacps::fs::resolve_path(host->services().data_dir, user_path);
}

Result<std::string> parse_user_path(JSContext* ctx, JSValueConst path_v) {
  auto* host = script_runtime_from(ctx);
  if (!host) return std::unexpected(Error{"fs: runtime not initialized"});
  auto p = converter<std::string>::from_js(ctx, path_v);
  if (!p) return std::unexpected(std::move(p.error()));
  return *p;
}

JSValue js_fs_mkdir(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.mkdir(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->services().pool, [host, path = std::move(path)]() {
          auto abs = resolve_under_data(host, path);
          if (!abs) return VoidResult{std::unexpected(std::move(abs.error()))};
          return vacps::fs::mkdir_p(*abs);
        });
        if (!r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

JSValue js_fs_exists(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.exists(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        const bool ok = co_await vacps::fs::async_offload(host->services().pool, [host, path = std::move(path)]() {
          auto abs = resolve_under_data(host, path);
          if (!abs) return false;
          return vacps::fs::exists(*abs);
        });
        bridge.resolve(converter<bool>::to_js(c, ok));
        co_return;
      });
}

JSValue js_fs_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.remove(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->services().pool, [host, path = std::move(path)]() {
          auto abs = resolve_under_data(host, path);
          if (!abs) return VoidResult{std::unexpected(std::move(abs.error()))};
          return vacps::fs::remove_path(*abs);
        });
        if (!r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

JSValue js_fs_rename(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  if (argc < 2) return JS_ThrowTypeError(ctx, "fs.rename(from, to)");
  auto from = parse_user_path(ctx, argv[0]);
  if (!from) return throw_error(ctx, from.error());
  auto to = parse_user_path(ctx, argv[1]);
  if (!to) return throw_error(ctx, to.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, from = std::move(*from), to = std::move(*to)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(
            host->services().pool, [host, from = std::move(from), to = std::move(to)]() {
              auto a = resolve_under_data(host, from);
              if (!a) return VoidResult{std::unexpected(std::move(a.error()))};
              auto b = resolve_under_data(host, to);
              if (!b) return VoidResult{std::unexpected(std::move(b.error()))};
              return vacps::fs::rename_path(*a, *b);
            });
        if (!r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

JSValue js_fs_read_directory(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readDirectory(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto entries = co_await vacps::fs::async_offload(host->services().pool, [host, path = std::move(path)]() {
          auto abs = resolve_under_data(host, path);
          if (!abs) {
            return Result<std::vector<vacps::fs::DirEntry>>{
                std::unexpected(std::move(abs.error()))};
          }
          return vacps::fs::list_dir(*abs);
        });
        if (!entries) {
          bridge.reject(entries.error());
          co_return;
        }
        auto arr = Value::new_array(c);
        std::uint32_t i = 0;
        for (const auto& e : *entries) {
          auto obj = Value::new_object(c);
          obj.set_property_str("name", converter<std::string>::to_js(c, e.name));
          obj.set_property_str("isDir", converter<bool>::to_js(c, e.is_dir));
          obj.set_property_str("isFile", converter<bool>::to_js(c, e.is_file));
          obj.set_property_str(
              "size", converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(e.size)));
          arr.set_property_uint32(i++, std::move(obj));
        }
        bridge.resolve(std::move(arr));
        co_return;
      });
}

JSValue js_fs_stat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.stat(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto st = co_await vacps::fs::async_offload(host->services().pool, [host, path = std::move(path)]() {
          auto abs = resolve_under_data(host, path);
          if (!abs) {
            return Result<vacps::fs::FileStat>{std::unexpected(std::move(abs.error()))};
          }
          return vacps::fs::file_stat(*abs);
        });
        if (!st) {
          bridge.reject(st.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str("path", converter<std::string>::to_js(c, st->path));
          obj.set_property_str("type", converter<std::string>::to_js(c, st->type));
          obj.set_property_str(
              "size", converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(st->size_bytes)));
          obj.set_property_str(
              "mtimeMs", converter<std::int64_t>::to_js(c, st->modified_at_ms));
          obj.set_property_str("readable", converter<bool>::to_js(c, st->readable));
          obj.set_property_str("writable", converter<bool>::to_js(c, st->writable));
          obj.set_property_str("isSymlink", converter<bool>::to_js(c, st->is_symlink));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

// ── File class (pure I/O handle; path policy is JS path-guard only) ─
//
// Opaque is shared_ptr<File> only — ClassBinding/ObjectHolder is enough.
// Process/Store hand-roll handles because they also store a host/runtime
// pointer; File always recovers ScriptRuntime via script_runtime_from(ctx).

using FileBinding = ClassBinding<vacps::fs::File>;

std::shared_ptr<vacps::fs::File> file_from_this(JSContext* ctx, JSValueConst this_val) {
  auto f = FileBinding::get_value(ctx, this_val);
  if (!f) {
    JS_ThrowTypeError(ctx, "File method requires a File instance");
  }
  return f;
}

/** Parse Node-like open args: (path, flags, mode?) or (path, { flags, mode? }).
 *  Numeric flags are cast to vacps::fs::Flags (Asio file_base values on Linux). */
Result<vacps::fs::OpenOptions> open_options_from_js(
    JSContext* ctx,
    int argc,
    JSValueConst* argv) {
  vacps::fs::OpenOptions opts;
  if (argc < 2) {
    return std::unexpected(Error{"File.open(path, flags, mode?)"});
  }
  if (is_number(argv[1])) {
    auto flags = converter<std::int32_t>::from_js(ctx, argv[1]);
    if (!flags) return std::unexpected(std::move(flags.error()));
    opts.flags = vacps::fs::flags_from_int(*flags);
    if (argc >= 3 && !is_nullish(argv[2])) {
      auto mode = converter<std::int32_t>::from_js(ctx, argv[2]);
      if (!mode) return std::unexpected(std::move(mode.error()));
      if (*mode < 0) {
        return std::unexpected(Error{"File.open: mode must be >= 0"});
      }
      opts.mode = static_cast<unsigned>(*mode);
    }
    return opts;
  }
  if (!is_object(argv[1]) || is_null(argv[1])) {
    return std::unexpected(Error{"File.open: flags number or options object required"});
  }
  Value flags_v = Value::get_property_str(ctx, argv[1], "flags");
  if (flags_v.is_nullish()) {
    return std::unexpected(Error{"File.open: options.flags required"});
  }
  auto flags = converter<std::int32_t>::from_js(ctx, flags_v.get());
  if (!flags) return std::unexpected(std::move(flags.error()));
  opts.flags = vacps::fs::flags_from_int(*flags);
  Value mode_v = Value::get_property_str(ctx, argv[1], "mode");
  if (!mode_v.is_nullish()) {
    auto mode = converter<std::int32_t>::from_js(ctx, mode_v.get());
    if (!mode) return std::unexpected(std::move(mode.error()));
    if (*mode < 0) {
      return std::unexpected(Error{"File.open: mode must be >= 0"});
    }
    opts.mode = static_cast<unsigned>(*mode);
  }
  return opts;
}

JSValue js_file_ctor(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_ThrowTypeError(ctx, "File is not constructible; use File.open(path, flags, mode?)");
}

JSValue js_file_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  if (argc < 2) return JS_ThrowTypeError(ctx, "File.open(path, flags, mode?)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  auto opts = open_options_from_js(ctx, argc, argv);
  if (!opts) return throw_error(ctx, opts.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path), opts = *opts](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto opened = co_await vacps::fs::File::async_open(
            vacps::fs::AsyncOptions{
                host->services().pool,
                host->ioc().get_executor(),
                host->services().use_asio_file},
            std::move(path),
            opts,
            host->services().data_dir);
        if (!opened) {
          bridge.reject(opened.error());
        } else {
          bridge.resolve(Value{c, FileBinding::wrap(c, "File", std::move(*opened))});
        }
        co_return;
      });
}

JSValue js_file_get_path(JSContext* ctx, JSValueConst this_val) {
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  return converter<std::string>::to_js(ctx, f->display_path()).release();
}

JSValue js_file_get_flags(JSContext* ctx, JSValueConst this_val) {
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  return converter<std::int32_t>::to_js(
             ctx, vacps::fs::flags_to_int(f->flags()))
      .release();
}

JSValue js_file_get_closed(JSContext* ctx, JSValueConst this_val) {
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, f->closed()).release();
}

JSValue js_file_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  std::size_t max_bytes = std::numeric_limits<std::size_t>::max();
  if (argc >= 1 && !is_nullish(argv[0])) {
    auto n = converter<std::int64_t>::from_js(ctx, argv[0]);
    if (!n) return throw_error(ctx, n.error());
    if (*n < 0) return JS_ThrowTypeError(ctx, "File.read: maxBytes must be >= 0");
    max_bytes = static_cast<std::size_t>(*n);
  }
  return spawn_js_promise(
      ctx, host,
      [f, max_bytes](JSContext* c, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        auto data = co_await f->async_read(max_bytes);
        if (!data) bridge.reject(data.error());
        else bridge.resolve(bytes_to_js(c, *data));
        co_return;
      });
}

JSValue js_file_read_at(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  if (argc < 2) return JS_ThrowTypeError(ctx, "File.readAt(offset, maxBytes)");
  auto off = converter<std::int64_t>::from_js(ctx, argv[0]);
  if (!off) return throw_error(ctx, off.error());
  if (*off < 0) return JS_ThrowTypeError(ctx, "File.readAt: offset must be >= 0");
  auto maxb = converter<std::int64_t>::from_js(ctx, argv[1]);
  if (!maxb) return throw_error(ctx, maxb.error());
  if (*maxb < 0) return JS_ThrowTypeError(ctx, "File.readAt: maxBytes must be >= 0");
  return spawn_js_promise(
      ctx, host,
      [f, offset = static_cast<std::uint64_t>(*off),
       max_bytes = static_cast<std::size_t>(*maxb)](
          JSContext* c, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        auto data = co_await f->async_read_at(offset, max_bytes);
        if (!data) bridge.reject(data.error());
        else bridge.resolve(bytes_to_js(c, *data));
        co_return;
      });
}

JSValue js_file_read_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  std::size_t max_bytes = std::numeric_limits<std::size_t>::max();
  if (argc >= 1 && is_object(argv[0]) && !is_null(argv[0])) {
    Value mb = Value::get_property_str(ctx, argv[0], "maxBytes");
    if (!mb.is_nullish()) {
      auto n = converter<std::int64_t>::from_js(ctx, mb.get());
      if (!n) return throw_error(ctx, n.error());
      if (*n < 0) return JS_ThrowTypeError(ctx, "File.readText: maxBytes must be >= 0");
      max_bytes = static_cast<std::size_t>(*n);
    }
  }
  return spawn_js_promise(
      ctx, host,
      [f, max_bytes](JSContext* c, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        auto data = co_await f->async_read_text(max_bytes);
        if (!data) bridge.reject(data.error());
        else bridge.resolve(converter<std::string>::to_js(c, *data));
        co_return;
      });
}

JSValue js_file_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "File.write(data)");
  auto data = bytes_from_js(ctx, argv[0]);
  if (!data) return throw_error(ctx, data.error());
  return spawn_js_promise(
      ctx, host,
      [f, data = std::move(*data)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto n = co_await f->async_write(
            std::span<const std::uint8_t>(data.data(), data.size()));
        if (!n) bridge.reject(n.error());
        else {
          bridge.resolve(
              converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(*n)));
        }
        co_return;
      });
}

JSValue js_file_write_at(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  if (argc < 2) return JS_ThrowTypeError(ctx, "File.writeAt(offset, data)");
  auto off = converter<std::int64_t>::from_js(ctx, argv[0]);
  if (!off) return throw_error(ctx, off.error());
  if (*off < 0) return JS_ThrowTypeError(ctx, "File.writeAt: offset must be >= 0");
  auto data = bytes_from_js(ctx, argv[1]);
  if (!data) return throw_error(ctx, data.error());
  return spawn_js_promise(
      ctx, host,
      [f, offset = static_cast<std::uint64_t>(*off), data = std::move(*data)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto n = co_await f->async_write_at(
            offset,
            std::span<const std::uint8_t>(data.data(), data.size()));
        if (!n) bridge.reject(n.error());
        else {
          bridge.resolve(
              converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(*n)));
        }
        co_return;
      });
}

JSValue js_file_write_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "File.writeText(text)");
  auto text = converter<std::string>::from_js(ctx, argv[0]);
  if (!text) return throw_error(ctx, text.error());
  return spawn_js_promise(
      ctx, host,
      [f, text = std::move(*text)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto n = co_await f->async_write_text(std::move(text));
        if (!n) bridge.reject(n.error());
        else {
          bridge.resolve(
              converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(*n)));
        }
        co_return;
      });
}

JSValue js_file_truncate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "File.truncate(size)");
  auto size = converter<std::int64_t>::from_js(ctx, argv[0]);
  if (!size) return throw_error(ctx, size.error());
  if (*size < 0) return JS_ThrowTypeError(ctx, "File.truncate: size must be >= 0");
  return spawn_js_promise(
      ctx, host,
      [f, size = static_cast<std::uint64_t>(*size)](
          JSContext* c, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        auto r = co_await f->async_truncate(size);
        if (!r) bridge.reject(r.error());
        else bridge.resolve_undefined();
        co_return;
      });
}

JSValue js_file_stat(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  return spawn_js_promise(
      ctx, host,
      [f](JSContext* c, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        auto st = co_await f->async_stat();
        if (!st) {
          bridge.reject(st.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str("path", converter<std::string>::to_js(c, st->path));
          obj.set_property_str("type", converter<std::string>::to_js(c, st->type));
          obj.set_property_str(
              "size", converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(st->size_bytes)));
          obj.set_property_str(
              "mtimeMs", converter<std::int64_t>::to_js(c, st->modified_at_ms));
          obj.set_property_str("readable", converter<bool>::to_js(c, st->readable));
          obj.set_property_str("writable", converter<bool>::to_js(c, st->writable));
          obj.set_property_str("isSymlink", converter<bool>::to_js(c, st->is_symlink));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

JSValue js_file_flush(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  return spawn_js_promise(
      ctx, host,
      [f](JSContext* c, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        auto r = co_await f->async_flush();
        if (!r) bridge.reject(r.error());
        else bridge.resolve_undefined();
        co_return;
      });
}

JSValue js_file_close(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = script_runtime_from(ctx);
  if (!host) return throw_msg(ctx, "fs: runtime not wired");
  auto f = file_from_this(ctx, this_val);
  if (!f) return JS_EXCEPTION;
  return spawn_js_promise(
      ctx, host,
      [f](JSContext* c, PromiseBridge& bridge) -> boost::asio::awaitable<void> {
        auto r = co_await f->async_close();
        if (!r) bridge.reject(r.error());
        else bridge.resolve_undefined();
        co_return;
      });
}

const JSCFunctionListEntry k_file_proto[] = {
    JS_CGETSET_DEF("path", js_file_get_path, nullptr),
    JS_CGETSET_DEF("flags", js_file_get_flags, nullptr),
    JS_CGETSET_DEF("closed", js_file_get_closed, nullptr),
    JS_CFUNC_DEF("read", 1, js_file_read),
    JS_CFUNC_DEF("readAt", 2, js_file_read_at),
    JS_CFUNC_DEF("readText", 1, js_file_read_text),
    JS_CFUNC_DEF("write", 1, js_file_write),
    JS_CFUNC_DEF("writeAt", 2, js_file_write_at),
    JS_CFUNC_DEF("writeText", 1, js_file_write_text),
    JS_CFUNC_DEF("truncate", 1, js_file_truncate),
    JS_CFUNC_DEF("stat", 0, js_file_stat),
    JS_CFUNC_DEF("flush", 0, js_file_flush),
    JS_CFUNC_DEF("close", 0, js_file_close),
};

JSValue make_file_constructor(JSContext* ctx) {
  if (FileBinding::ensure_registered(ctx, "File") < 0) {
    return JS_ThrowInternalError(ctx, "File class registration failed");
  }
  JSValue proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, proto, k_file_proto, VACPS_COUNTOF(k_file_proto));
  JSValue ctor = JS_NewCFunction2(ctx, js_file_ctor, "File", 0, JS_CFUNC_constructor, 0);
  JS_SetConstructor(ctx, ctor, proto);
  JS_SetClassProto(ctx, FileBinding::class_id(), proto);
  // static open
  JS_SetPropertyStr(
      ctx, ctor, "open",
      JS_NewCFunction(ctx, js_file_open, "open", 3));
  return ctor;
}

const JSCFunctionListEntry k_fs_exports[] = {
    JS_CFUNC_DEF("mkdir", 1, js_fs_mkdir),
    JS_CFUNC_DEF("exists", 1, js_fs_exists),
    JS_CFUNC_DEF("stat", 1, js_fs_stat),
    JS_CFUNC_DEF("remove", 1, js_fs_remove),
    JS_CFUNC_DEF("rename", 2, js_fs_rename),
    JS_CFUNC_DEF("readDirectory", 1, js_fs_read_directory),
    // Numeric open flags (Asio file_base values / POSIX-compatible on Linux).
    JS_PROP_INT32_DEF(
        "O_RDONLY", vacps::fs::flags_to_int(vacps::fs::Flags::read_only),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF(
        "O_WRONLY", vacps::fs::flags_to_int(vacps::fs::Flags::write_only),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF(
        "O_RDWR", vacps::fs::flags_to_int(vacps::fs::Flags::read_write),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF(
        "O_CREAT", vacps::fs::flags_to_int(vacps::fs::Flags::create),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF(
        "O_TRUNC", vacps::fs::flags_to_int(vacps::fs::Flags::truncate),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF(
        "O_APPEND", vacps::fs::flags_to_int(vacps::fs::Flags::append),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF(
        "O_EXCL", vacps::fs::flags_to_int(vacps::fs::Flags::exclusive),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
    // Host always sets CLOEXEC on the pool path; export for Node-style OR masks.
    JS_PROP_INT32_DEF("O_CLOEXEC", O_CLOEXEC, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE),
};

int js_fs_init(JSContext* ctx, JSModuleDef* m) {
  if (JS_SetModuleExportList(ctx, m, k_fs_exports, VACPS_COUNTOF(k_fs_exports)) < 0) {
    return -1;
  }
  JSValue file_ctor = make_file_constructor(ctx);
  if (JS_IsException(file_ctor)) return -1;
  if (JS_SetModuleExport(ctx, m, "File", file_ctor) < 0) {
    JS_FreeValue(ctx, file_ctor);
    return -1;
  }
  return 0;
}

}  // namespace

JSModuleDef* init_module_fs(JSContext* ctx, const char* name, void* /*binding*/) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_fs_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_fs_exports, VACPS_COUNTOF(k_fs_exports));
  JS_AddModuleExport(ctx, m, "File");
  return m;
}

}  // namespace vacps::js
