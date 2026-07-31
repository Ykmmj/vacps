#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/bindings/common.hpp"

#include "process/process.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/promise_bridge.hpp"
#include "quickjs/raii/value.hpp"

#include <boost/asio/post.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <quickjs.h>

namespace vacps::js {
namespace {

// ── Process class (JS ↔ process::Process; JS never sees registry id) ──

struct ProcessHandle {
  std::shared_ptr<vacps::process::Process> proc;
  std::weak_ptr<ScriptRuntime> runtime;
};

JSClassID g_process_class_id = 0;

void process_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<ProcessHandle*>(JS_GetOpaque(val, g_process_class_id));
  if (h != nullptr && h->proc) {
    auto proc = h->proc;
    if (!proc->closed() && proc->started()) {
      if (auto rt = h->runtime.lock()) {
        try {
          auto sp = rt->shared_from_this();
          boost::asio::post(sp->ioc(), [sp, proc]() {
            (void)sp;  // keep ScriptRuntime (and Registry) alive for close
            (void)proc->close();
          });
        } catch (...) {
          // ScriptRuntime already tearing down — best-effort sync close.
          try {
            (void)proc->close();
          } catch (...) {
          }
        }
      }
    }
  }
  delete h;
}

JSClassDef g_process_class = {
    "Process",
    .finalizer = process_finalizer,
};

void ensure_process_class(JSContext* ctx) {
  if (g_process_class_id == 0) {
    JS_NewClassID(&g_process_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_process_class_id)) {
    JS_NewClass(rt, g_process_class_id, &g_process_class);
  }
}

ProcessHandle* process_handle_from_this(JSContext* ctx, JSValueConst this_val) {
  auto* h =
      static_cast<ProcessHandle*>(JS_GetOpaque2(ctx, this_val, g_process_class_id));
  if (h == nullptr || !h->proc) {
    JS_ThrowTypeError(ctx, "Process method requires a Process instance");
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
      ctx, obj, atom, get, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
  JS_FreeAtom(ctx, atom);
}

/** ArrayBuffer → Uint8Array for Process.read (matches d.ts). */
JSValue bytes_to_uint8array(JSContext* ctx, const char* data, std::size_t len) {
  JSValue ab = JS_NewArrayBufferCopy(
      ctx, reinterpret_cast<const uint8_t*>(data), len);
  if (JS_IsException(ab)) return ab;

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
  JS_FreeValue(ctx, global);
  if (JS_IsException(ctor)) {
    JS_FreeValue(ctx, ab);
    return ctor;
  }
  JSValueConst args[1] = {ab};
  JSValue ua = JS_CallConstructor(ctx, ctor, 1, args);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, ab);
  return ua;
}

Value process_result_to_js(
    JSContext* c,
    std::int32_t exit_code,
    bool timed_out,
    const std::string& stdout_str,
    const std::string& stderr_str,
    std::size_t stdout_produced,
    std::size_t stderr_produced,
    bool stdout_truncated,
    bool stderr_truncated) {
  auto obj = Value::new_object(c);
  obj.set_property_str("exitCode", converter<std::int32_t>::to_js(c, exit_code));
  obj.set_property_str("timedOut", converter<bool>::to_js(c, timed_out));
  obj.set_property_str("stdout", converter<std::string>::to_js(c, stdout_str));
  obj.set_property_str("stderr", converter<std::string>::to_js(c, stderr_str));
  obj.set_property_str(
      "stdoutProduced",
      converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(stdout_produced)));
  obj.set_property_str(
      "stderrProduced",
      converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(stderr_produced)));
  obj.set_property_str(
      "stdoutTruncated", converter<bool>::to_js(c, stdout_truncated));
  obj.set_property_str(
      "stderrTruncated", converter<bool>::to_js(c, stderr_truncated));
  return obj;
}

Value run_result_to_js(JSContext* c, const vacps::process::RunResult& r) {
  return process_result_to_js(
      c,
      r.exit_code,
      r.timed_out,
      r.stdout_str,
      r.stderr_str,
      r.stdout_produced,
      r.stderr_produced,
      r.stdout_truncated,
      r.stderr_truncated);
}

/** Parse ProcessOptions into StartOptions. */
void parse_start_options_fields(
    JSContext* ctx,
    JSValueConst opts_v,
    vacps::process::StartOptions& opts) {
  Value cwd = Value::get_property_str(ctx, opts_v, "cwd");
  if (!cwd.is_nullish()) {
    auto s = converter<std::string>::from_js(ctx, cwd.get());
    if (s) opts.cwd = std::move(*s);
  }
  Value tmo = Value::get_property_str(ctx, opts_v, "timeoutMs");
  if (!tmo.is_nullish()) {
    auto ms = converter<std::int32_t>::from_js(ctx, tmo.get());
    if (ms && *ms >= 0) opts.timeout_ms = *ms;
  }
  Value cs = Value::get_property_str(ctx, opts_v, "closeStdin");
  if (!cs.is_nullish()) {
    auto b = converter<bool>::from_js(ctx, cs.get());
    if (b) opts.close_stdin = *b;
  }
  // StdioMode: only stdin maps today (pipe → keep open; ignore → close).
  Value stdin_m = Value::get_property_str(ctx, opts_v, "stdin");
  if (!stdin_m.is_nullish()) {
    auto s = converter<std::string>::from_js(ctx, stdin_m.get());
    if (s) {
      if (*s == "ignore") opts.close_stdin = true;
      else if (*s == "pipe") opts.close_stdin = false;
      // "inherit" not supported by registry — leave default / closeStdin.
    }
  }
  Value hso = Value::get_property_str(ctx, opts_v, "hardMaxStdout");
  if (hso.is_nullish()) hso = Value::get_property_str(ctx, opts_v, "maxStdoutBytes");
  if (!hso.is_nullish()) {
    auto n = converter<std::int64_t>::from_js(ctx, hso.get());
    if (n && *n >= 0) opts.hard_max_stdout = static_cast<std::size_t>(*n);
  }
  Value hse = Value::get_property_str(ctx, opts_v, "hardMaxStderr");
  if (hse.is_nullish()) hse = Value::get_property_str(ctx, opts_v, "maxStderrBytes");
  if (!hse.is_nullish()) {
    auto n = converter<std::int64_t>::from_js(ctx, hse.get());
    if (n && *n >= 0) opts.hard_max_stderr = static_cast<std::size_t>(*n);
  }
}

Result<std::vector<std::string>> string_array_from_js(
    JSContext* ctx,
    JSValueConst arr) {
  Value len_v = Value::get_property_str(ctx, arr, "length");
  auto len_r = converter<std::int32_t>::from_js(ctx, len_v.get());
  if (!len_r || *len_r < 0) {
    return std::unexpected(Error{"expected string array"});
  }
  const auto len = static_cast<std::uint32_t>(*len_r);
  std::vector<std::string> out;
  out.reserve(len);
  for (std::uint32_t i = 0; i < len; ++i) {
    Value el = Value::get_property_uint32(ctx, arr, i);
    auto s = converter<std::string>::from_js(ctx, el.get());
    if (!s) return std::unexpected(s.error());
    out.push_back(std::move(*s));
  }
  return out;
}

// ── Process constructor / getters / methods ───────────────────────

/**
 * new Process(command, args?, options?)
 * Sync: constructs process::Process; does not spawn.
 */
JSValue js_process_constructor(
    JSContext* ctx,
    JSValueConst /*new_target*/,
    int argc,
    JSValueConst* argv) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) {
    return throw_msg(ctx, "Process: runtime not wired");
  }
  if (argc < 1 || !is_string(argv[0])) {
    return JS_ThrowTypeError(ctx, "new Process(command, args?, options?)");
  }
  auto cmd = converter<std::string>::from_js(ctx, argv[0]);
  if (!cmd) return throw_error(ctx, cmd.error());
  if (cmd->empty()) {
    return JS_ThrowTypeError(ctx, "Process: command must be non-empty");
  }

  std::vector<std::string> av;
  av.push_back(std::move(*cmd));
  vacps::process::StartOptions start_opts{};
  // Keep stdin open by default so Process.write works.
  start_opts.close_stdin = false;

  if (argc >= 2 && !is_nullish(argv[1])) {
    if (is_array(ctx, argv[1])) {
      auto args = string_array_from_js(ctx, argv[1]);
      if (!args) return throw_error(ctx, args.error());
      for (auto& a : *args) av.push_back(std::move(a));
      if (argc >= 3 && !is_nullish(argv[2]) && is_object(argv[2])) {
        parse_start_options_fields(ctx, argv[2], start_opts);
      }
    } else if (is_object(argv[1])) {
      // new Process(command, options) — no args array
      parse_start_options_fields(ctx, argv[1], start_opts);
    } else {
      return JS_ThrowTypeError(ctx, "Process: args must be an array or options object");
    }
  }

  auto proc = std::make_shared<vacps::process::Process>(
      rt->services().processes, std::move(av), std::move(start_opts));

  ensure_process_class(ctx);
  auto* handle = new ProcessHandle{std::move(proc), rt->shared_from_this()};
  Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(g_process_class_id))};
  if (obj.is_exception()) {
    delete handle;
    return obj.release();
  }
  JS_SetOpaque(obj.get(), handle);
  return obj.release();
}

JSValue js_process_get_pid(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto p = h->proc->pid();
  if (!p) {
    return JS_NULL;
  }
  return converter<std::int32_t>::to_js(ctx, *p).release();
}

JSValue js_process_get_running(
    JSContext* ctx,
    JSValueConst this_val,
    int,
    JSValueConst*) {
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->proc->running()).release();
}

/** process.start(): Promise<void> — spawn via process::Process. */
JSValue js_process_inst_start(
    JSContext* ctx,
    JSValueConst this_val,
    int,
    JSValueConst*) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) return throw_msg(ctx, "Process.start: runtime not wired");
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto proc = h->proc;

  return spawn_js_promise(
      ctx,
      rt,
      [proc](JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = co_await proc->start();
        if (!result) {
          bridge.reject(result.error());
          co_return;
        }
        bridge.resolve_undefined();
        co_return;
      });
}

/**
 * process.read(stream?): Promise<Uint8Array>
 * Progressive read of one stream (default "stdout"). Waits for data or finish.
 */
JSValue js_process_inst_read(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) return throw_msg(ctx, "Process.read: runtime not wired");
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto proc = h->proc;
  if (!proc->started()) {
    return JS_ThrowTypeError(ctx, "Process.read: process not started or closed");
  }

  std::string stream = "stdout";
  if (argc >= 1 && !is_nullish(argv[0])) {
    auto s = converter<std::string>::from_js(ctx, argv[0]);
    if (!s) return throw_error(ctx, s.error());
    if (*s != "stdout" && *s != "stderr") {
      return JS_ThrowTypeError(ctx, "Process.read: stream must be \"stdout\" or \"stderr\"");
    }
    stream = std::move(*s);
  }

  return spawn_js_promise(
      ctx,
      rt,
      [proc, stream = std::move(stream)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = co_await proc->read(stream);
        if (!result) {
          bridge.reject(result.error());
          co_return;
        }

        JSValue ua = bytes_to_uint8array(c, result->data(), result->size());
        if (JS_IsException(ua)) {
          Value ex{c, JS_GetException(c)};
          (void)ex;
          bridge.reject_message("Process.read: failed to allocate Uint8Array");
          co_return;
        }
        bridge.resolve(Value{c, ua});
        co_return;
      });
}

/**
 * process.write(data): Promise<number>
 * data: ArrayBufferView | ArrayBuffer | string
 */
JSValue js_process_inst_write(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) return throw_msg(ctx, "Process.write: runtime not wired");
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto proc = h->proc;
  if (!proc->started()) {
    return JS_ThrowTypeError(ctx, "Process.write: process not started or closed");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "Process.write(data)");
  }
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  std::string data(bytes->begin(), bytes->end());

  return spawn_js_promise(
      ctx,
      rt,
      [proc, data = std::move(data)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        vacps::process::WriteOptions wopts;
        wopts.close_stdin = false;
        auto result = co_await proc->write(std::move(data), wopts);
        if (!result) {
          bridge.reject(result.error());
          co_return;
        }
        bridge.resolve(
            converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(*result)));
        co_return;
      });
}

/**
 * process.wait(): Promise<ProcessResult>
 * Blocks until process finishes; returns full captured stdout/stderr.
 */
JSValue js_process_inst_wait(
    JSContext* ctx,
    JSValueConst this_val,
    int,
    JSValueConst*) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) return throw_msg(ctx, "Process.wait: runtime not wired");
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto proc = h->proc;
  if (!proc->started()) {
    return JS_ThrowTypeError(ctx, "Process.wait: process not started or closed");
  }

  return spawn_js_promise(
      ctx,
      rt,
      [proc](JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = co_await proc->wait();
        if (!result) {
          bridge.reject(result.error());
          co_return;
        }
        bridge.resolve(run_result_to_js(c, *result));
        co_return;
      });
}

/**
 * process.terminate(signal?): Promise<void>
 */
JSValue js_process_inst_terminate(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) return throw_msg(ctx, "Process.terminate: runtime not wired");
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto proc = h->proc;
  if (!proc->started()) {
    return JS_ThrowTypeError(ctx, "Process.terminate: process not started or closed");
  }

  std::string signal = "SIGTERM";
  if (argc >= 1 && !is_nullish(argv[0])) {
    if (!is_string(argv[0])) {
      return JS_ThrowTypeError(ctx, "Process.terminate: signal must be a string");
    }
    auto s = converter<std::string>::from_js(ctx, argv[0]);
    if (!s) return throw_error(ctx, s.error());
    signal = std::move(*s);
  }

  return spawn_js_promise(
      ctx,
      rt,
      [proc, signal = std::move(signal)](
          JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = proc->terminate(signal, 3000);
        if (!result) {
          bridge.reject(result.error());
          co_return;
        }
        bridge.resolve_undefined();
        co_return;
      });
}

/**
 * process.close(): Promise<void> — drop registry entry; idempotent.
 */
JSValue js_process_inst_close(
    JSContext* ctx,
    JSValueConst this_val,
    int,
    JSValueConst*) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) return throw_msg(ctx, "Process.close: runtime not wired");
  auto* h = process_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto proc = h->proc;

  return spawn_js_promise(
      ctx,
      rt,
      [proc](JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = proc->close();
        if (!result) {
          bridge.reject(result.error());
          co_return;
        }
        bridge.resolve_undefined();
        co_return;
      });
}

// ── Module free functions ──────────────────────────────────────────

/**
 * process.run(command, args?, options?): Promise<ProcessResult>
 * Convenience: Process → start → wait (captured buffers) → close.
 */
JSValue js_process_run(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* rt = script_runtime_from(ctx);
  if (rt == nullptr) {
    return throw_msg(ctx, "process.run: runtime not wired");
  }
  if (argc < 1 || !is_string(argv[0])) {
    return JS_ThrowTypeError(ctx, "process.run(command, args?, options?)");
  }

  auto cmd = converter<std::string>::from_js(ctx, argv[0]);
  if (!cmd) return throw_error(ctx, cmd.error());
  if (cmd->empty()) {
    return JS_ThrowTypeError(ctx, "process.run: command empty");
  }

  std::vector<std::string> av;
  av.push_back(std::move(*cmd));
  vacps::process::StartOptions start_opts{};
  // One-shot: close stdin; capture pipes via registry defaults.
  start_opts.close_stdin = true;

  if (argc >= 2 && !is_nullish(argv[1])) {
    if (is_array(ctx, argv[1])) {
      auto args = string_array_from_js(ctx, argv[1]);
      if (!args) return throw_error(ctx, args.error());
      for (auto& a : *args) av.push_back(std::move(a));
      if (argc >= 3 && !is_nullish(argv[2]) && is_object(argv[2])) {
        parse_start_options_fields(ctx, argv[2], start_opts);
      }
    } else if (is_object(argv[1])) {
      parse_start_options_fields(ctx, argv[1], start_opts);
    } else {
      return JS_ThrowTypeError(ctx, "process.run: args must be an array or options object");
    }
  }

  auto proc = std::make_shared<vacps::process::Process>(
      rt->services().processes, std::move(av), std::move(start_opts));

  return spawn_js_promise(
      ctx,
      rt,
      [proc](JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto started = co_await proc->start();
        if (!started) {
          bridge.reject(started.error());
          co_return;
        }
        auto result = co_await proc->wait();
        (void)proc->close();
        if (!result) {
          bridge.reject(result.error());
          co_return;
        }
        bridge.resolve(run_result_to_js(c, *result));
        co_return;
      });
}

const JSCFunctionListEntry k_process_proto[] = {
    JS_CFUNC_DEF("start", 0, js_process_inst_start),
    JS_CFUNC_DEF("read", 1, js_process_inst_read),
    JS_CFUNC_DEF("write", 1, js_process_inst_write),
    JS_CFUNC_DEF("wait", 0, js_process_inst_wait),
    JS_CFUNC_DEF("terminate", 1, js_process_inst_terminate),
    JS_CFUNC_DEF("close", 0, js_process_inst_close),
};

const JSCFunctionListEntry k_process_exports[] = {
    JS_CFUNC_DEF("run", 3, js_process_run),
};

int js_process_init(JSContext* ctx, JSModuleDef* m) {
  ensure_process_class(ctx);

  Value proto = Value::new_object(ctx);
  if (proto.is_exception()) return -1;
  JS_SetPropertyFunctionList(
      ctx, proto.get(), k_process_proto, VACPS_COUNTOF(k_process_proto));
  define_getter(ctx, proto.get(), "pid", js_process_get_pid);
  define_getter(ctx, proto.get(), "running", js_process_get_running);
  JS_SetClassProto(ctx, g_process_class_id, proto.release());

  JSValue ctor = JS_NewCFunction2(
      ctx, js_process_constructor, "Process", 3, JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) return -1;
  JSValue class_proto = JS_GetClassProto(ctx, g_process_class_id);
  JS_DefinePropertyValueStr(
      ctx, ctor, "prototype", class_proto, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

  if (JS_SetModuleExport(ctx, m, "Process", ctor) != 0) {
    return -1;
  }
  return JS_SetModuleExportList(
      ctx, m, k_process_exports, VACPS_COUNTOF(k_process_exports));
}

}  // namespace

JSModuleDef* init_module_process(JSContext* ctx, const char* name, void* /*binding*/) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_process_init);
  if (!m) return nullptr;
  JS_AddModuleExport(ctx, m, "Process");
  JS_AddModuleExportList(ctx, m, k_process_exports, VACPS_COUNTOF(k_process_exports));
  return m;
}

}  // namespace vacps::js
