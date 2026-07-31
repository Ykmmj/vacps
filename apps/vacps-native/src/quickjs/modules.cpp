#include "quickjs/modules.hpp"

#include "quickjs/encoding_globals.hpp"
#include "quickjs/url_globals.hpp"

#include "crypto/crypto.hpp"
#include "fs/async.hpp"
#include "fs/fs.hpp"
#include "fs/sandbox.hpp"

#include <algorithm>
#include <cstdlib>
#include "http/client.hpp"
#include "http/server.hpp"
#include "quickjs/host.hpp"
#include "quickjs/atom.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/promise_bridge.hpp"
#include "quickjs/value.hpp"
#include "app/log.hpp"
#include "process/process.hpp"
#include "app/version.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

#define VACPS_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

// ── vacps:log ─────────────────────────────────────────────────────

JSValue js_log_write(
    JSContext* ctx,
    JSValueConst /*this_val*/,
    int argc,
    JSValueConst* argv,
    int magic) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "log expects a message string");
  }
  auto msg = converter<std::string>::from_js(ctx, argv[0]);
  if (!msg) {
    return throw_error(ctx, msg.error());
  }
  switch (magic) {
    case 0: vacps::log::trace("{}", *msg); break;
    case 1: vacps::log::debug("{}", *msg); break;
    case 2: vacps::log::info("{}", *msg); break;
    case 3: vacps::log::warn("{}", *msg); break;
    default: vacps::log::error("{}", *msg); break;
  }
  return JS_UNDEFINED;
}

JSValue js_log_flush(JSContext* /*ctx*/, JSValueConst, int, JSValueConst*) {
  vacps::log::flush();
  return JS_UNDEFINED;
}

const JSCFunctionListEntry k_log_exports[] = {
    JS_CFUNC_MAGIC_DEF("trace", 1, js_log_write, 0),
    JS_CFUNC_MAGIC_DEF("debug", 1, js_log_write, 1),
    JS_CFUNC_MAGIC_DEF("info", 1, js_log_write, 2),
    JS_CFUNC_MAGIC_DEF("warn", 1, js_log_write, 3),
    JS_CFUNC_MAGIC_DEF("error", 1, js_log_write, 4),
    JS_CFUNC_DEF("flush", 0, js_log_flush),
};

int js_log_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_log_exports, VACPS_COUNTOF(k_log_exports));
}

JSModuleDef* init_module_log(JSContext* ctx, const char* name) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_log_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_log_exports, VACPS_COUNTOF(k_log_exports));
  return m;
}

// ── vacps:store ───────────────────────────────────────────────────
// Capability/factory only: JS creates instances via store.open(path).
// Connection lives on the JS Store handle (shared_ptr), not on Host.
// SQL runs on Host::pool() so the io_context thread never blocks.
// Multi-step atomicity: store.transaction([...]) = one offloaded job.

struct StoreState {
  std::unique_ptr<vacps::Database> db;
  std::string path;
};

struct StoreHandle {
  std::shared_ptr<StoreState> state;
  /** Only for deferred close on pool; Host does not own the connection. */
  Host* host{nullptr};
};

JSClassID g_store_class_id = 0;

void store_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<StoreHandle*>(JS_GetOpaque(val, g_store_class_id));
  if (h != nullptr && h->state && h->state->db) {
    auto state = h->state;
    if (h->host != nullptr) {
      try {
        auto sp = h->host->shared_from_this();
        boost::asio::post(sp->pool(), [state]() { state->db.reset(); });
      } catch (...) {
        // Host already tearing down — best-effort close here.
        state->db.reset();
      }
    } else {
      state->db.reset();
    }
  }
  delete h;
}

JSClassDef g_store_class = {
    "Store",
    .finalizer = store_finalizer,
};

void ensure_store_class(JSContext* ctx) {
  if (g_store_class_id == 0) {
    JS_NewClassID(&g_store_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_store_class_id)) {
    JS_NewClass(rt, g_store_class_id, &g_store_class);
  }
}

StoreHandle* store_handle_from_this(JSContext* ctx, JSValueConst this_val) {
  auto* h = static_cast<StoreHandle*>(JS_GetOpaque2(ctx, this_val, g_store_class_id));
  if (h == nullptr || !h->state) {
    JS_ThrowTypeError(ctx, "Store method requires an open Store instance");
    return nullptr;
  }
  return h;
}

JSValue make_store_object(JSContext* ctx, Host* host, std::shared_ptr<StoreState> state) {
  ensure_store_class(ctx);
  auto* handle = new StoreHandle{std::move(state), host};
  Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(g_store_class_id))};
  if (obj.is_exception()) {
    delete handle;
    return obj.release();
  }
  JS_SetOpaque(obj.get(), handle);
  return obj.release();
}

/** store.open(path) → Promise<Store> — open + PRAGMA on host pool. */
JSValue js_store_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.open: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "store.open(path)");
  auto path = converter<std::string>::from_js(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());

  return spawn_js_promise(
      ctx,
      host,
      [path = std::move(*path), host](JSContext* c, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        // Database is not default-constructible; async_offload needs a T that is
        // (Asio co_spawn error path), so return unique_ptr.
        auto opened = co_await vacps::fs::async_offload(
            host->pool(),
            [path]() -> Result<std::unique_ptr<vacps::Database>> {
              auto db = vacps::Database::open(path);
              if (!db) {
                return std::unexpected(std::move(db.error()));
              }
              return std::make_unique<vacps::Database>(std::move(*db));
            });
        if (!opened) {
          bridge.reject(opened.error());
          co_return;
        }
        auto state = std::make_shared<StoreState>();
        state->path = path;
        state->db = std::move(*opened);
        Value obj{c, make_store_object(c, host, std::move(state))};
        if (obj.is_exception()) {
          bridge.reject_message("store.open: failed to allocate Store object");
          co_return;
        }
        bridge.resolve(std::move(obj));
        co_return;
      });
}

JSValue js_store_exec(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.exec: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "store.exec(sql)");
  auto sql = converter<std::string>::from_js(ctx, argv[0]);
  if (!sql) return throw_error(ctx, sql.error());
  auto state = h->state;

  return spawn_js_promise(
      ctx,
      host,
      [state, sql = std::move(*sql), host](JSContext*, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->pool(), [state, sql]() -> VoidResult {
          if (!state->db || !state->db->ok()) {
            return std::unexpected(Error{"store closed"});
          }
          return state->db->exec(sql);
        });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        bridge.resolve_undefined();
        co_return;
      });
}

JSValue js_store_run(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.run: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "store.run(sql[, params])");
  auto sql = converter<std::string>::from_js(ctx, argv[0]);
  if (!sql) return throw_error(ctx, sql.error());
  std::vector<SqlValue> params;
  if (argc >= 2) {
    auto p = sql_params_from_js(ctx, argv[1]);
    if (!p) return throw_error(ctx, p.error());
    params = std::move(*p);
  }
  auto state = h->state;

  return spawn_js_promise(
      ctx,
      host,
      [state, sql = std::move(*sql), params = std::move(params), host](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        struct Meta {
          std::int64_t changes{0};
          std::int64_t last_insert_rowid{0};
        };
        auto r = co_await vacps::fs::async_offload(
            host->pool(),
            [state, sql, params]() -> Result<Meta> {
              if (!state->db || !state->db->ok()) {
                return std::unexpected(Error{"store closed"});
              }
              if (auto ex = state->db->execute(sql, params); !ex) {
                return std::unexpected(std::move(ex.error()));
              }
              return Meta{state->db->changes(), state->db->last_insert_rowid()};
            });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        auto obj = Value::new_object(c);
        obj.set_property_str("changes", converter<std::int64_t>::to_js(c, r->changes));
        obj.set_property_str(
            "lastInsertRowid", converter<std::int64_t>::to_js(c, r->last_insert_rowid));
        bridge.resolve(std::move(obj));
        co_return;
      });
}

JSValue js_store_query(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.query: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "store.query(sql[, params])");
  auto sql = converter<std::string>::from_js(ctx, argv[0]);
  if (!sql) return throw_error(ctx, sql.error());
  std::vector<SqlValue> params;
  if (argc >= 2) {
    auto p = sql_params_from_js(ctx, argv[1]);
    if (!p) return throw_error(ctx, p.error());
    params = std::move(*p);
  }
  auto state = h->state;

  return spawn_js_promise(
      ctx,
      host,
      [state, sql = std::move(*sql), params = std::move(params), host](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto qr = co_await vacps::fs::async_offload(
            host->pool(),
            [state, sql, params]() -> Result<QueryResult> {
              if (!state->db || !state->db->ok()) {
                return std::unexpected(Error{"store closed"});
              }
              return state->db->query(sql, params);
            });
        if (!qr) {
          bridge.reject(qr.error());
          co_return;
        }
        bridge.resolve(query_result_to_js(c, *qr));
        co_return;
      });
}

/**
 * store.transaction(steps) → Promise<RunResult[]>
 * steps: Array<{ sql: string, params?: SqlParam[], exec?: boolean }>
 * Entire unit runs as one BEGIN IMMEDIATE … COMMIT job on host pool (no interleaving).
 */
JSValue js_store_transaction(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.transaction: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1 || !is_array(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "store.transaction(steps: Array)");
  }
  Value len_v = Value::get_property_str(ctx, argv[0], "length");
  auto len_r = converter<std::int32_t>::from_js(ctx, len_v.get());
  if (!len_r || *len_r < 0) {
    return JS_ThrowTypeError(ctx, "store.transaction: invalid steps length");
  }
  std::vector<vacps::Database::TxStep> steps;
  steps.reserve(static_cast<std::size_t>(*len_r));
  for (std::int32_t i = 0; i < *len_r; ++i) {
    Value el = Value::get_property_uint32(ctx, argv[0], static_cast<std::uint32_t>(i));
    if (!el.is_object() || el.is_null()) {
      return JS_ThrowTypeError(ctx, "store.transaction: each step must be an object");
    }
    Value sql_v = Value::get_property_str(ctx, el.get(), "sql");
    auto sql = converter<std::string>::from_js(ctx, sql_v.get());
    if (!sql) return throw_error(ctx, sql.error());
    vacps::Database::TxStep step;
    step.sql = std::move(*sql);
    step.is_run = true;
    Value exec_v = Value::get_property_str(ctx, el.get(), "exec");
    if (!exec_v.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, exec_v.get());
      if (b && *b) step.is_run = false;
    }
    Value params_v = Value::get_property_str(ctx, el.get(), "params");
    if (!params_v.is_nullish()) {
      auto p = sql_params_from_js(ctx, params_v.get());
      if (!p) return throw_error(ctx, p.error());
      step.params = std::move(*p);
    }
    steps.push_back(std::move(step));
  }
  auto state = h->state;
  return spawn_js_promise(
      ctx,
      host,
      [state, steps = std::move(steps), host](JSContext* c, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(
            host->pool(),
            [state, steps = std::move(steps)]() mutable
                -> Result<std::vector<vacps::Database::TxStepResult>> {
              if (!state->db || !state->db->ok()) {
                return std::unexpected(Error{"store closed"});
              }
              return state->db->run_transaction(steps);
            });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        auto arr = Value::new_array(c);
        for (std::uint32_t i = 0; i < r->size(); ++i) {
          auto obj = Value::new_object(c);
          obj.set_property_str(
              "changes", converter<std::int64_t>::to_js(c, (*r)[i].changes));
          obj.set_property_str(
              "lastInsertRowid",
              converter<std::int64_t>::to_js(c, (*r)[i].last_insert_rowid));
          arr.set_property_uint32(i, std::move(obj));
        }
        bridge.resolve(std::move(arr));
        co_return;
      });
}

JSValue js_store_begin(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.begin: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto state = h->state;
  return spawn_js_promise(
      ctx,
      host,
      [state, host](JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->pool(), [state]() -> VoidResult {
          if (!state->db || !state->db->ok()) {
            return std::unexpected(Error{"store closed"});
          }
          return state->db->begin();
        });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        bridge.resolve_undefined();
        co_return;
      });
}

JSValue js_store_commit(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.commit: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto state = h->state;
  return spawn_js_promise(
      ctx,
      host,
      [state, host](JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->pool(), [state]() -> VoidResult {
          if (!state->db || !state->db->ok()) {
            return std::unexpected(Error{"store closed"});
          }
          return state->db->commit();
        });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        bridge.resolve_undefined();
        co_return;
      });
}

JSValue js_store_rollback(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.rollback: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto state = h->state;
  return spawn_js_promise(
      ctx,
      host,
      [state, host](JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->pool(), [state]() -> VoidResult {
          if (!state->db || !state->db->ok()) {
            return std::unexpected(Error{"store closed"});
          }
          return state->db->rollback();
        });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        bridge.resolve_undefined();
        co_return;
      });
}

JSValue js_store_path(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<std::string>::to_js(ctx, h->state->path).release();
}

JSValue js_store_close(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "store.close: host not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto state = h->state;
  return spawn_js_promise(
      ctx,
      host,
      [state, host](JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        co_await vacps::fs::async_offload(host->pool(), [state]() {
          state->db.reset();
          return true;
        });
        bridge.resolve_undefined();
        co_return;
      });
}

const JSCFunctionListEntry k_store_proto[] = {
    JS_CFUNC_DEF("exec", 1, js_store_exec),
    JS_CFUNC_DEF("run", 2, js_store_run),
    JS_CFUNC_DEF("query", 2, js_store_query),
    JS_CFUNC_DEF("transaction", 1, js_store_transaction),
    JS_CFUNC_DEF("begin", 0, js_store_begin),
    JS_CFUNC_DEF("commit", 0, js_store_commit),
    JS_CFUNC_DEF("rollback", 0, js_store_rollback),
    JS_CFUNC_DEF("path", 0, js_store_path),
    JS_CFUNC_DEF("close", 0, js_store_close),
};

const JSCFunctionListEntry k_store_exports[] = {
    JS_CFUNC_DEF("open", 1, js_store_open),
};

int js_store_init(JSContext* ctx, JSModuleDef* m) {
  ensure_store_class(ctx);
  Value proto = Value::new_object(ctx);
  JS_SetPropertyFunctionList(ctx, proto.get(), k_store_proto, VACPS_COUNTOF(k_store_proto));
  JS_SetClassProto(ctx, g_store_class_id, proto.release());
  return JS_SetModuleExportList(ctx, m, k_store_exports, VACPS_COUNTOF(k_store_exports));
}

JSModuleDef* init_module_store(JSContext* ctx, const char* name) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_store_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_store_exports, VACPS_COUNTOF(k_store_exports));
  return m;
}

// ── vacps:host ────────────────────────────────────────────────────

// vacps:host — thin process info only (not HTTP/SQL/process/fs).
JSValue js_host_version(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return converter<std::string>::to_js(ctx, std::string{vacps::version()}).release();
}

JSValue js_host_data_dir(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  auto* env = env_from(ctx);
  if (!env) return throw_msg(ctx, "host: not initialized");
  return converter<std::string>::to_js(ctx, env->data_dir()).release();
}

JSValue js_host_now_ms(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  using clock = std::chrono::system_clock;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock::now().time_since_epoch())
                      .count();
  return JS_NewInt64(ctx, static_cast<std::int64_t>(ms));
}

JSValue js_host_platform(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return converter<std::string>::to_js(ctx, "linux-x86_64-musl").release();
}

/** host.getenv(name) → string | null (process environment; empty string if set empty). */
JSValue js_host_getenv(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "host.getenv(name)");
  auto name = converter<std::string>::from_js(ctx, argv[0]);
  if (!name) return throw_error(ctx, name.error());
  if (name->empty() || name->find('\0') != std::string::npos) {
    return JS_ThrowTypeError(ctx, "host.getenv: invalid name");
  }
  const char* v = std::getenv(name->c_str());
  if (v == nullptr) {
    return JS_NULL;
  }
  return converter<std::string>::to_js(ctx, std::string{v}).release();
}

const JSCFunctionListEntry k_host_exports[] = {
    JS_CFUNC_DEF("version", 0, js_host_version),
    JS_CFUNC_DEF("dataDir", 0, js_host_data_dir),
    JS_CFUNC_DEF("nowMs", 0, js_host_now_ms),
    JS_CFUNC_DEF("platform", 0, js_host_platform),
    JS_CFUNC_DEF("getenv", 1, js_host_getenv),
};

int js_host_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_host_exports, VACPS_COUNTOF(k_host_exports));
}

JSModuleDef* init_module_host(JSContext* ctx, const char* name) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_host_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_host_exports, VACPS_COUNTOF(k_host_exports));
  return m;
}

// ── vacps:http (inbound Server class — factory, JS owns instance) ──

struct HttpServerHandle {
  std::shared_ptr<vacps::http::Server> server;
};

JSClassID g_http_server_class_id = 0;

void http_server_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<HttpServerHandle*>(JS_GetOpaque(val, g_http_server_class_id));
  if (h != nullptr && h->server) {
    h->server->close();
  }
  delete h;
}

JSClassDef g_http_server_class = {
    "Server",
    .finalizer = http_server_finalizer,
};

void ensure_http_server_class(JSContext* ctx) {
  if (g_http_server_class_id == 0) {
    JS_NewClassID(&g_http_server_class_id);
  }
  JSRuntime* rt = JS_GetRuntime(ctx);
  if (!JS_IsRegisteredClass(rt, g_http_server_class_id)) {
    JS_NewClass(rt, g_http_server_class_id, &g_http_server_class);
  }
}

HttpServerHandle* http_server_from_this(JSContext* ctx, JSValueConst this_val) {
  auto* h =
      static_cast<HttpServerHandle*>(JS_GetOpaque2(ctx, this_val, g_http_server_class_id));
  if (h == nullptr || !h->server) {
    JS_ThrowTypeError(ctx, "Server method requires a live Server instance");
    return nullptr;
  }
  return h;
}

/**
 * http.createServer([options]) → Server
 * options: { host?, port? } — only values JS passes (loadConfig / explicit args).
 * C++ does not read listen host/port from process Config or env.
 */
JSValue js_http_create_server(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* raw = host_from(ctx);
  if (raw == nullptr) {
    return throw_msg(ctx, "http.createServer: host not wired");
  }
  auto host = raw->shared_from_this();

  vacps::http::ListenEndpoint listen;
  if (argc >= 1 && is_object(argv[0])) {
    Value h = Value::get_property_str(ctx, argv[0], "host");
    if (!h.is_nullish()) {
      auto s = converter<std::string>::from_js(ctx, h.get());
      if (s) listen.host = std::move(*s);
    }
    Value p = Value::get_property_str(ctx, argv[0], "port");
    if (!p.is_nullish()) {
      auto port = converter<std::int32_t>::from_js(ctx, p.get());
      if (port && *port >= 1 && *port <= 65535) {
        listen.port = static_cast<std::uint16_t>(*port);
      }
    }
  }
  // Capability safety on bind: non-loopback requires VACPS_ALLOW_REMOTE_BIND=true.
  // Not product config — refuses accidental public bind from any createServer call.
  {
    const bool loopback = listen.host == "127.0.0.1" || listen.host == "localhost" ||
                          listen.host == "::1";
    const char* allow = std::getenv("VACPS_ALLOW_REMOTE_BIND");
    const bool remote_ok =
        allow != nullptr && (std::string_view{allow} == "true" || std::string_view{allow} == "1");
    if (!loopback && !remote_ok) {
      listen.host = "127.0.0.1";
    }
  }

  ensure_http_server_class(ctx);
  auto server =
      std::make_shared<vacps::http::Server>(host->ioc(), std::move(listen), host);
  auto* handle = new HttpServerHandle{std::move(server)};
  Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(g_http_server_class_id))};
  if (obj.is_exception()) {
    delete handle;
    return obj.release();
  }
  JS_SetOpaque(obj.get(), handle);
  return obj.release();
}

JSValue js_http_server_listen(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = http_server_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (h->server->is_open()) {
    return throw_msg(ctx, "http.Server.listen: already listening");
  }
  if (auto r = h->server->start(); !r) {
    return throw_error(ctx, r.error());
  }
  return JS_UNDEFINED;
}

JSValue js_http_server_close(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = http_server_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  h->server->close();
  return JS_UNDEFINED;
}

JSValue js_http_server_is_listening(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = http_server_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->server->is_open()).release();
}

const JSCFunctionListEntry k_http_server_proto[] = {
    JS_CFUNC_DEF("listen", 0, js_http_server_listen),
    JS_CFUNC_DEF("close", 0, js_http_server_close),
    JS_CFUNC_DEF("isListening", 0, js_http_server_is_listening),
};

/**
 * http.request(options) → Promise<{ status, headers, body }>
 * options: { method?, url, headers?, body?, timeoutMs?, maxResponseBytes? }
 */
JSValue js_http_request(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) {
    return throw_msg(ctx, "http.request: host not wired");
  }
  if (argc < 1 || !is_object(argv[0])) {
    return JS_ThrowTypeError(ctx, "http.request(options)");
  }

  vacps::http::ClientRequest req;
  auto* env = env_from(ctx);
  if (env) req.ca_bundle = env->ca_bundle();

  Value url_v = Value::get_property_str(ctx, argv[0], "url");
  if (url_v.is_nullish()) {
    return JS_ThrowTypeError(ctx, "http.request: url required");
  }
  auto url = converter<std::string>::from_js(ctx, url_v.get());
  if (!url) return throw_error(ctx, url.error());
  req.url = std::move(*url);

  Value method_v = Value::get_property_str(ctx, argv[0], "method");
  if (!method_v.is_nullish()) {
    auto m = converter<std::string>::from_js(ctx, method_v.get());
    if (!m) return throw_error(ctx, m.error());
    req.method = std::move(*m);
  }

  Value body_v = Value::get_property_str(ctx, argv[0], "body");
  if (!body_v.is_nullish()) {
    if (is_string(body_v.get())) {
      auto s = converter<std::string>::from_js(ctx, body_v.get());
      if (!s) return throw_error(ctx, s.error());
      req.body = std::move(*s);
    } else {
      auto b = bytes_from_js(ctx, body_v.get());
      if (!b) return throw_error(ctx, b.error());
      req.body.assign(reinterpret_cast<const char*>(b->data()), b->size());
    }
  }

  Value tmo = Value::get_property_str(ctx, argv[0], "timeoutMs");
  if (!tmo.is_nullish()) {
    auto ms = converter<std::int32_t>::from_js(ctx, tmo.get());
    if (!ms) return throw_error(ctx, ms.error());
    if (*ms < 0) return JS_ThrowTypeError(ctx, "http.request: timeoutMs must be >= 0");
    req.timeout_ms = *ms;
  }

  Value maxb = Value::get_property_str(ctx, argv[0], "maxResponseBytes");
  if (!maxb.is_nullish()) {
    auto n = converter<std::int32_t>::from_js(ctx, maxb.get());
    if (!n) return throw_error(ctx, n.error());
    if (*n <= 0) return JS_ThrowTypeError(ctx, "http.request: maxResponseBytes must be > 0");
    req.max_response_bytes = static_cast<std::size_t>(*n);
  }

  Value hdrs = Value::get_property_str(ctx, argv[0], "headers");
  if (hdrs.is_object() && !hdrs.is_null()) {
    auto names = PropertyEnumList::get_own(ctx, hdrs.get());
    for (std::uint32_t i = 0; i < names.size(); ++i) {
      Value key_v{ctx, JS_AtomToValue(ctx, names.atom_at(i))};
      auto key = converter<std::string>::from_js(ctx, key_v.get());
      if (!key) continue;
      Value val{ctx, JS_GetProperty(ctx, hdrs.get(), names.atom_at(i))};
      auto vs = converter<std::string>::from_js(ctx, val.get());
      if (vs) {
        req.headers.emplace_back(std::move(*key), std::move(*vs));
      }
    }
  }

  return spawn_js_promise(
      ctx,
      host,
      [req = std::move(req)](JSContext* c, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        auto result = co_await vacps::http::async_request(std::move(req));
        if (!result) {
          bridge.reject(result.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str(
              "status", converter<std::int32_t>::to_js(c, result->status));
          auto hdr_obj = Value::new_object(c);
          for (const auto& [k, v] : result->headers) {
            hdr_obj.set_property_str(k.c_str(), converter<std::string>::to_js(c, v));
          }
          obj.set_property_str("headers", std::move(hdr_obj));
          std::vector<std::uint8_t> body_bytes(result->body.begin(), result->body.end());
          obj.set_property_str("body", bytes_to_js(c, body_bytes));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

const JSCFunctionListEntry k_http_exports[] = {
    JS_CFUNC_DEF("createServer", 1, js_http_create_server),
    JS_CFUNC_DEF("request", 1, js_http_request),
};

int js_http_init(JSContext* ctx, JSModuleDef* m) {
  ensure_http_server_class(ctx);
  Value proto = Value::new_object(ctx);
  JS_SetPropertyFunctionList(
      ctx, proto.get(), k_http_server_proto, VACPS_COUNTOF(k_http_server_proto));
  JS_SetClassProto(ctx, g_http_server_class_id, proto.release());
  return JS_SetModuleExportList(ctx, m, k_http_exports, VACPS_COUNTOF(k_http_exports));
}

JSModuleDef* init_module_http(JSContext* ctx, const char* name) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_http_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_http_exports, VACPS_COUNTOF(k_http_exports));
  return m;
}

// ── vacps:fs (async: stream_file and/or thread_pool + PathSandbox) ──
// Tool-layer path-guard.ts remains defense-in-depth; C++ enforces allowlist
// + openat2(RESOLVE_BENEATH) so symlink escape cannot leave configured roots.

Result<std::string> parse_user_path(JSContext* ctx, JSValueConst path_v) {
  auto* host = host_from(ctx);
  if (!host) return std::unexpected(Error{"fs: host not initialized"});
  auto p = converter<std::string>::from_js(ctx, path_v);
  if (!p) return std::unexpected(std::move(p.error()));
  return *p;
}

// All vacps:fs async APIs use spawn_js_promise (promise_bridge.hpp).

JSValue js_fs_read_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readText(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto data = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().read_text(path, host->env().data_dir());
        });
        if (!data) {
          bridge.reject(data.error());
        } else {
          bridge.resolve(converter<std::string>::to_js(c, *data));
        }
        co_return;
      });
}

JSValue js_fs_write_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 2) return JS_ThrowTypeError(ctx, "fs.writeText(path, data)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  auto data = converter<std::string>::from_js(ctx, argv[1]);
  if (!data) return throw_error(ctx, data.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path), data = std::move(*data)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(
            host->pool(), [host, path = std::move(path), data = std::move(data)]() {
              return host->env().path_sandbox().write_text(path, host->env().data_dir(), data);
            });
        if (!r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

JSValue js_fs_append_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 2) return JS_ThrowTypeError(ctx, "fs.appendText(path, data)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  auto data = converter<std::string>::from_js(ctx, argv[1]);
  if (!data) return throw_error(ctx, data.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path), data = std::move(*data)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(
            host->pool(), [host, path = std::move(path), data = std::move(data)]() {
              return host->env().path_sandbox().append_text(path, host->env().data_dir(), data);
            });
        if (!r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

JSValue js_fs_read_bytes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readBytes(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto data = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().read_bytes(path, host->env().data_dir());
        });
        if (!data) {
          bridge.reject(data.error());
        } else {
          bridge.resolve(bytes_to_js(c, *data));
        }
        co_return;
      });
}

/**
 * fs.readRange(path, offset, maxBytes) → Promise<ArrayBuffer>
 * Reads at most maxBytes from offset without loading the whole file.
 */
JSValue js_fs_read_range(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 3) return JS_ThrowTypeError(ctx, "fs.readRange(path, offset, maxBytes)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  auto off = converter<std::int64_t>::from_js(ctx, argv[1]);
  if (!off) return throw_error(ctx, off.error());
  if (*off < 0) return JS_ThrowTypeError(ctx, "fs.readRange: offset must be >= 0");
  auto maxb = converter<std::int64_t>::from_js(ctx, argv[2]);
  if (!maxb) return throw_error(ctx, maxb.error());
  if (*maxb < 0) return JS_ThrowTypeError(ctx, "fs.readRange: maxBytes must be >= 0");
  // Cap single range to 16 MiB to avoid accidental huge allocations.
  const auto max_bytes = static_cast<std::size_t>(
      std::min(*maxb, static_cast<std::int64_t>(16 * 1024 * 1024)));
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path), offset = static_cast<std::uint64_t>(*off), max_bytes](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto data = co_await vacps::fs::async_offload(
            host->pool(),
            [host, path = std::move(path), offset, max_bytes]() {
              return host->env().path_sandbox().read_range(path, host->env().data_dir(), offset, max_bytes);
            });
        if (!data) {
          bridge.reject(data.error());
        } else {
          bridge.resolve(bytes_to_js(c, *data));
        }
        co_return;
      });
}

/** fs.hashFile(path) → Promise<{ sizeBytes, sha256Hex }> (streaming). */
JSValue js_fs_hash_file(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.hashFile(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto dig = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().hash_file(path, host->env().data_dir());
        });
        if (!dig) {
          bridge.reject(dig.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str(
              "sizeBytes",
              converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(dig->size_bytes)));
          obj.set_property_str(
              "sha256Hex", converter<std::string>::to_js(c, dig->sha256_hex));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

JSValue js_fs_write_bytes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 2) return JS_ThrowTypeError(ctx, "fs.writeBytes(path, data)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  auto data = bytes_from_js(ctx, argv[1]);
  if (!data) return throw_error(ctx, data.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path), data = std::move(*data)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(
            host->pool(), [host, path = std::move(path), data = std::move(data)]() {
              return host->env().path_sandbox().write_bytes(path, host->env().data_dir(), data);
            });
        if (!r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

JSValue js_fs_mkdir(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.mkdir(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().mkdir(path, host->env().data_dir());
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
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.exists(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        const bool ok = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().exists(path, host->env().data_dir());
        });
        bridge.resolve(converter<bool>::to_js(c, ok));
        co_return;
      });
}

JSValue js_fs_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.remove(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().remove(path, host->env().data_dir());
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
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
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
            host->pool(), [host, from = std::move(from), to = std::move(to)]() {
              return host->env().path_sandbox().rename(from, to, host->env().data_dir());
            });
        if (!r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

JSValue js_fs_list(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.list(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto entries = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().list(path, host->env().data_dir());
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
  auto* host = host_from(ctx);
  if (!host) return throw_msg(ctx, "fs: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "fs.stat(path)");
  auto path = parse_user_path(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());
  return spawn_js_promise(
      ctx,
      host,
      [host, path = std::move(*path)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto st = co_await vacps::fs::async_offload(host->pool(), [host, path = std::move(path)]() {
          return host->env().path_sandbox().stat(path, host->env().data_dir());
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

const JSCFunctionListEntry k_fs_exports[] = {
    JS_CFUNC_DEF("readText", 1, js_fs_read_text),
    JS_CFUNC_DEF("writeText", 2, js_fs_write_text),
    JS_CFUNC_DEF("appendText", 2, js_fs_append_text),
    JS_CFUNC_DEF("readBytes", 1, js_fs_read_bytes),
    JS_CFUNC_DEF("readRange", 3, js_fs_read_range),
    JS_CFUNC_DEF("hashFile", 1, js_fs_hash_file),
    JS_CFUNC_DEF("writeBytes", 2, js_fs_write_bytes),
    JS_CFUNC_DEF("mkdir", 1, js_fs_mkdir),
    JS_CFUNC_DEF("exists", 1, js_fs_exists),
    JS_CFUNC_DEF("stat", 1, js_fs_stat),
    JS_CFUNC_DEF("remove", 1, js_fs_remove),
    JS_CFUNC_DEF("rename", 2, js_fs_rename),
    JS_CFUNC_DEF("list", 1, js_fs_list),
};

int js_fs_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_fs_exports, VACPS_COUNTOF(k_fs_exports));
}

JSModuleDef* init_module_fs(JSContext* ctx, const char* name) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_fs_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_fs_exports, VACPS_COUNTOF(k_fs_exports));
  return m;
}

// ── vacps:process ─────────────────────────────────────────────────

/**
 * process.run(argv[, options]) → Promise<RunResult>
 * Boost.Process v2 + Asio coroutines on the host io_context.
 */
JSValue js_process_run(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) {
    return throw_msg(ctx, "process.run: host not wired");
  }
  if (argc < 1 || !is_array(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "process.run(argv[, options])");
  }
  Value len_v = Value::get_property_str(ctx, argv[0], "length");
  auto len_r = converter<std::int32_t>::from_js(ctx, len_v.get());
  if (!len_r || *len_r <= 0) {
    return JS_ThrowTypeError(ctx, "process.run: argv empty");
  }
  const auto len = static_cast<std::uint32_t>(*len_r);

  std::vector<std::string> av;
  av.reserve(len);
  for (std::uint32_t i = 0; i < len; ++i) {
    Value el = Value::get_property_uint32(ctx, argv[0], i);
    auto s = converter<std::string>::from_js(ctx, el.get());
    if (!s) return throw_error(ctx, s.error());
    av.push_back(std::move(*s));
  }

  vacps::process::RunOptions opts;
  if (argc >= 2 && !is_nullish(argv[1])) {
    Value cwd = Value::get_property_str(ctx, argv[1], "cwd");
    if (!cwd.is_nullish()) {
      auto s = converter<std::string>::from_js(ctx, cwd.get());
      if (s) opts.cwd = std::move(*s);
    }
    Value tmo = Value::get_property_str(ctx, argv[1], "timeoutMs");
    if (!tmo.is_nullish()) {
      auto ms = converter<std::int32_t>::from_js(ctx, tmo.get());
      if (!ms) return throw_error(ctx, ms.error());
      if (*ms < 0) {
        return JS_ThrowTypeError(ctx, "process.run: timeoutMs must be >= 0");
      }
      opts.timeout_ms = *ms;
    }
    Value max_out = Value::get_property_str(ctx, argv[1], "maxStdoutBytes");
    if (!max_out.is_nullish()) {
      auto n = converter<std::int32_t>::from_js(ctx, max_out.get());
      if (!n) return throw_error(ctx, n.error());
      if (*n < 0) {
        return JS_ThrowTypeError(ctx, "process.run: maxStdoutBytes must be >= 0");
      }
      opts.max_stdout_bytes = static_cast<std::size_t>(*n);
    }
    Value max_err = Value::get_property_str(ctx, argv[1], "maxStderrBytes");
    if (!max_err.is_nullish()) {
      auto n = converter<std::int32_t>::from_js(ctx, max_err.get());
      if (!n) return throw_error(ctx, n.error());
      if (*n < 0) {
        return JS_ThrowTypeError(ctx, "process.run: maxStderrBytes must be >= 0");
      }
      opts.max_stderr_bytes = static_cast<std::size_t>(*n);
    }
  }

  return spawn_js_promise(
      ctx,
      host,
      [av = std::move(av), opts = std::move(opts)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = co_await vacps::process::async_run(std::move(av), std::move(opts));
        if (!result) {
          bridge.reject(result.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str(
              "exitCode", converter<std::int32_t>::to_js(c, result->exit_code));
          obj.set_property_str(
              "timedOut", converter<bool>::to_js(c, result->timed_out));
          obj.set_property_str(
              "stdout", converter<std::string>::to_js(c, result->stdout_str));
          obj.set_property_str(
              "stderr", converter<std::string>::to_js(c, result->stderr_str));
          obj.set_property_str(
              "stdoutProduced",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->stdout_produced)));
          obj.set_property_str(
              "stderrProduced",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->stderr_produced)));
          obj.set_property_str(
              "stdoutTruncated", converter<bool>::to_js(c, result->stdout_truncated));
          obj.set_property_str(
              "stderrTruncated", converter<bool>::to_js(c, result->stderr_truncated));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

/**
 * process.start(argv[, options]) → Promise<{ id, pid }>
 */
JSValue js_process_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "process.start: host not wired");
  if (argc < 1 || !is_array(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "process.start(argv[, options])");
  }
  Value len_v = Value::get_property_str(ctx, argv[0], "length");
  auto len_r = converter<std::int32_t>::from_js(ctx, len_v.get());
  if (!len_r || *len_r <= 0) {
    return JS_ThrowTypeError(ctx, "process.start: argv empty");
  }
  const auto len = static_cast<std::uint32_t>(*len_r);
  std::vector<std::string> av;
  av.reserve(len);
  for (std::uint32_t i = 0; i < len; ++i) {
    Value el = Value::get_property_uint32(ctx, argv[0], i);
    auto s = converter<std::string>::from_js(ctx, el.get());
    if (!s) return throw_error(ctx, s.error());
    av.push_back(std::move(*s));
  }

  vacps::process::StartOptions opts;
  if (argc >= 2 && !is_nullish(argv[1])) {
    Value cwd = Value::get_property_str(ctx, argv[1], "cwd");
    if (!cwd.is_nullish()) {
      auto s = converter<std::string>::from_js(ctx, cwd.get());
      if (s) opts.cwd = std::move(*s);
    }
    Value tmo = Value::get_property_str(ctx, argv[1], "timeoutMs");
    if (!tmo.is_nullish()) {
      auto ms = converter<std::int32_t>::from_js(ctx, tmo.get());
      if (!ms || *ms < 0) return JS_ThrowTypeError(ctx, "process.start: timeoutMs invalid");
      opts.timeout_ms = *ms;
    }
    Value cs = Value::get_property_str(ctx, argv[1], "closeStdin");
    if (!cs.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, cs.get());
      if (b) opts.close_stdin = *b;
    }
    Value hso = Value::get_property_str(ctx, argv[1], "hardMaxStdout");
    if (!hso.is_nullish()) {
      auto n = converter<std::int64_t>::from_js(ctx, hso.get());
    if (n && *n >= 0) opts.hard_max_stdout = static_cast<std::size_t>(*n);
    }
    Value hse = Value::get_property_str(ctx, argv[1], "hardMaxStderr");
    if (!hse.is_nullish()) {
      auto n = converter<std::int64_t>::from_js(ctx, hse.get());
    if (n && *n >= 0) opts.hard_max_stderr = static_cast<std::size_t>(*n);
    }
  }

  return spawn_js_promise(
      ctx,
      host,
      [host_sp = host->shared_from_this(), av = std::move(av), opts = std::move(opts)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = co_await host_sp->env().processes().start(std::move(av), std::move(opts));
        if (!result) {
          bridge.reject(result.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str("id", converter<std::string>::to_js(c, result->id));
          obj.set_property_str("pid", converter<std::int32_t>::to_js(c, result->pid));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

/**
 * process.read(id[, options]) → Promise<ReadInfo>
 * options: waitMs, maxBytes, stdoutOffset, stderrOffset
 */
JSValue js_process_read(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "process.read: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "process.read(id[, options])");
  auto id = converter<std::string>::from_js(ctx, argv[0]);
  if (!id) return throw_error(ctx, id.error());

  vacps::process::ReadOptions opts;
  if (argc >= 2 && !is_nullish(argv[1])) {
    Value w = Value::get_property_str(ctx, argv[1], "waitMs");
    if (!w.is_nullish()) {
      auto ms = converter<std::int32_t>::from_js(ctx, w.get());
      if (ms && *ms >= 0) opts.wait_ms = *ms;
    }
    Value mb = Value::get_property_str(ctx, argv[1], "maxBytes");
    if (!mb.is_nullish()) {
      auto n = converter<std::int64_t>::from_js(ctx, mb.get());
      if (n && *n > 0) opts.max_bytes = static_cast<std::size_t>(*n);
    }
    Value so = Value::get_property_str(ctx, argv[1], "stdoutOffset");
    if (!so.is_nullish()) {
      auto n = converter<std::int64_t>::from_js(ctx, so.get());
      if (n && *n >= 0) opts.stdout_offset = static_cast<std::size_t>(*n);
    }
    Value eo = Value::get_property_str(ctx, argv[1], "stderrOffset");
    if (!eo.is_nullish()) {
      auto n = converter<std::int64_t>::from_js(ctx, eo.get());
      if (n && *n >= 0) opts.stderr_offset = static_cast<std::size_t>(*n);
    }
  }

  return spawn_js_promise(
      ctx,
      host,
      [host_sp = host->shared_from_this(), id = std::move(*id), opts](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = co_await host_sp->env().processes().read(std::move(id), opts);
        if (!result) {
          bridge.reject(result.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str("status", converter<std::string>::to_js(c, result->status));
          obj.set_property_str(
              "exitCode", converter<std::int32_t>::to_js(c, result->exit_code));
          obj.set_property_str("timedOut", converter<bool>::to_js(c, result->timed_out));
          obj.set_property_str("eof", converter<bool>::to_js(c, result->eof));
          obj.set_property_str("stdinOpen", converter<bool>::to_js(c, result->stdin_open));
          obj.set_property_str(
              "stdout", converter<std::string>::to_js(c, result->stdout_slice));
          obj.set_property_str(
              "stderr", converter<std::string>::to_js(c, result->stderr_slice));
          obj.set_property_str(
              "stdoutTotal",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->stdout_total)));
          obj.set_property_str(
              "stderrTotal",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->stderr_total)));
          obj.set_property_str(
              "stdoutProduced",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->stdout_produced)));
          obj.set_property_str(
              "stderrProduced",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->stderr_produced)));
          obj.set_property_str(
              "stdoutTruncated", converter<bool>::to_js(c, result->stdout_truncated));
          obj.set_property_str(
              "stderrTruncated", converter<bool>::to_js(c, result->stderr_truncated));
          obj.set_property_str(
              "nextStdoutOffset",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->next_stdout_offset)));
          obj.set_property_str(
              "nextStderrOffset",
              converter<std::int64_t>::to_js(
                  c, static_cast<std::int64_t>(result->next_stderr_offset)));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

/**
 * process.write(id, data[, { close, timeoutMs, maxBytes }]) → Promise<{ writtenBytes }>
 * Async stdin write (async_write); never blocks the JS io_context on a full pipe.
 */
JSValue js_process_write(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "process.write: host not wired");
  if (argc < 2) return JS_ThrowTypeError(ctx, "process.write(id, data[, options])");
  auto id = converter<std::string>::from_js(ctx, argv[0]);
  if (!id) return throw_error(ctx, id.error());
  auto data = converter<std::string>::from_js(ctx, argv[1]);
  if (!data) return throw_error(ctx, data.error());
  vacps::process::WriteOptions opts;
  if (argc >= 3 && !is_nullish(argv[2])) {
    Value c = Value::get_property_str(ctx, argv[2], "close");
    if (!c.is_nullish()) {
      auto b = converter<bool>::from_js(ctx, c.get());
      if (b) opts.close_stdin = *b;
    }
    Value t = Value::get_property_str(ctx, argv[2], "timeoutMs");
    if (!t.is_nullish()) {
      auto ms = converter<std::int32_t>::from_js(ctx, t.get());
      if (ms && *ms >= 0) opts.timeout_ms = *ms;
    }
    Value m = Value::get_property_str(ctx, argv[2], "maxBytes");
    if (!m.is_nullish()) {
      auto n = converter<std::int32_t>::from_js(ctx, m.get());
      if (n && *n > 0) opts.max_bytes = static_cast<std::size_t>(*n);
    }
  }

  return spawn_js_promise(
      ctx,
      host,
      [host_sp = host->shared_from_this(),
       id = std::move(*id),
       data = std::move(*data),
       opts](JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result =
            co_await host_sp->env().processes().write(std::move(id), std::move(data), opts);
        if (!result) {
          bridge.reject(result.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str(
              "writtenBytes",
              converter<std::int64_t>::to_js(c, static_cast<std::int64_t>(*result)));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

/**
 * process.terminate(id[, { signal, graceMs }]) → Promise<{ requested, status }>
 */
JSValue js_process_terminate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "process.terminate: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "process.terminate(id[, options])");
  auto id = converter<std::string>::from_js(ctx, argv[0]);
  if (!id) return throw_error(ctx, id.error());
  std::string signal = "SIGTERM";
  std::int32_t grace_ms = 3000;
  if (argc >= 2 && !is_nullish(argv[1])) {
    Value s = Value::get_property_str(ctx, argv[1], "signal");
    if (!s.is_nullish()) {
      auto str = converter<std::string>::from_js(ctx, s.get());
      if (str) signal = std::move(*str);
    }
    Value g = Value::get_property_str(ctx, argv[1], "graceMs");
    if (!g.is_nullish()) {
      auto n = converter<std::int32_t>::from_js(ctx, g.get());
      if (n && *n >= 0) grace_ms = *n;
    }
  }

  return spawn_js_promise(
      ctx,
      host,
      [host_sp = host->shared_from_this(),
       id = std::move(*id),
       signal = std::move(signal),
       grace_ms](JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = host_sp->env().processes().terminate(id, signal, grace_ms);
        if (!result) {
          bridge.reject(result.error());
        } else {
          auto snap = host_sp->env().processes().snapshot(id);
          auto obj = Value::new_object(c);
          obj.set_property_str("requested", converter<bool>::to_js(c, *result));
          if (snap) {
            obj.set_property_str("status", converter<std::string>::to_js(c, snap->status));
            obj.set_property_str(
                "exitCode", converter<std::int32_t>::to_js(c, snap->exit_code));
          }
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

/**
 * process.close(id) → Promise<{ closed: boolean }>
 * Drop registry entry and free retained stdout/stderr buffers.
 */
JSValue js_process_close(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = host_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "process.close: host not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "process.close(id)");
  auto id = converter<std::string>::from_js(ctx, argv[0]);
  if (!id) return throw_error(ctx, id.error());

  return spawn_js_promise(
      ctx,
      host,
      [host_sp = host->shared_from_this(), id = std::move(*id)](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto result = host_sp->env().processes().close(id);
        if (!result) {
          bridge.reject(result.error());
        } else {
          auto obj = Value::new_object(c);
          obj.set_property_str("closed", converter<bool>::to_js(c, *result));
          bridge.resolve(std::move(obj));
        }
        co_return;
      });
}

const JSCFunctionListEntry k_process_exports[] = {
    JS_CFUNC_DEF("run", 2, js_process_run),
    JS_CFUNC_DEF("start", 2, js_process_start),
    JS_CFUNC_DEF("read", 2, js_process_read),
    JS_CFUNC_DEF("write", 3, js_process_write),
    JS_CFUNC_DEF("terminate", 2, js_process_terminate),
    JS_CFUNC_DEF("close", 1, js_process_close),
};

int js_process_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_process_exports, VACPS_COUNTOF(k_process_exports));
}

JSModuleDef* init_module_process(JSContext* ctx, const char* name) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_process_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_process_exports, VACPS_COUNTOF(k_process_exports));
  return m;
}

// ── vacps:crypto ──────────────────────────────────────────────────

JSValue js_crypto_random_bytes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.randomBytes(n)");
  auto n = converter<std::int32_t>::from_js(ctx, argv[0]);
  if (!n || *n < 0) {
    return JS_ThrowTypeError(ctx, "crypto.randomBytes: invalid n");
  }
  auto bytes = vacps::crypto::random_bytes(static_cast<std::size_t>(*n));
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_sha256(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha256(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  auto dig = vacps::crypto::sha256(*bytes);
  return bytes_to_js(ctx, dig).release();
}

JSValue js_crypto_sha256_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha256Hex(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  auto dig = vacps::crypto::sha256(*bytes);
  return converter<std::string>::to_js(ctx, vacps::crypto::to_hex(dig)).release();
}

JSValue js_crypto_to_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.toHex(bytes)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  return converter<std::string>::to_js(ctx, vacps::crypto::to_hex(*bytes)).release();
}

JSValue js_crypto_from_hex(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.fromHex(hex)");
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto bytes = vacps::crypto::from_hex(*s);
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_base64_encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64Encode(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  return converter<std::string>::to_js(ctx, vacps::crypto::base64_encode(*bytes)).release();
}

JSValue js_crypto_base64_decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64Decode(s)");
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto bytes = vacps::crypto::base64_decode(*s);
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_base64url_encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64UrlEncode(data)");
  auto bytes = bytes_from_js(ctx, argv[0]);
  if (!bytes) return throw_error(ctx, bytes.error());
  return converter<std::string>::to_js(ctx, vacps::crypto::base64url_encode(*bytes)).release();
}

JSValue js_crypto_base64url_decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.base64UrlDecode(s)");
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto bytes = vacps::crypto::base64url_decode(*s);
  if (!bytes) return throw_error(ctx, bytes.error());
  return bytes_to_js(ctx, *bytes).release();
}

JSValue js_crypto_ed25519_public_from_private(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "crypto.ed25519PublicFromPrivate(privateKey)");
  }
  auto key = bytes_from_js(ctx, argv[0]);
  if (!key) return throw_error(ctx, key.error());
  auto pub = vacps::crypto::ed25519_public_from_private(*key);
  if (!pub) return throw_error(ctx, pub.error());
  return bytes_to_js(ctx, *pub).release();
}

/**
 * crypto.ed25519SeedFromPrivateKey(base64url) → ArrayBuffer(32)
 * Accepts raw seed or PKCS#8 DER (base64url), OpenSSL-parsed.
 */
JSValue js_crypto_ed25519_seed_from_private_key(
    JSContext* ctx,
    JSValueConst,
    int argc,
    JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "crypto.ed25519SeedFromPrivateKey(base64url)");
  }
  auto s = converter<std::string>::from_js(ctx, argv[0]);
  if (!s) return throw_error(ctx, s.error());
  auto seed = vacps::crypto::ed25519_seed_from_private_key_base64url(*s);
  if (!seed) return throw_error(ctx, seed.error());
  return bytes_to_js(ctx, *seed).release();
}

JSValue js_crypto_ed25519_sign(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 2) return JS_ThrowTypeError(ctx, "crypto.ed25519Sign(privateKey, message)");
  auto key = bytes_from_js(ctx, argv[0]);
  if (!key) return throw_error(ctx, key.error());
  auto msg = bytes_from_js(ctx, argv[1]);
  if (!msg) return throw_error(ctx, msg.error());
  auto sig = vacps::crypto::ed25519_sign(
      *key, std::string_view(reinterpret_cast<const char*>(msg->data()), msg->size()));
  if (!sig) return throw_error(ctx, sig.error());
  return bytes_to_js(ctx, *sig).release();
}

JSValue js_crypto_ed25519_verify(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(ctx, "crypto.ed25519Verify(publicKey, message, signature)");
  }
  auto key = bytes_from_js(ctx, argv[0]);
  if (!key) return throw_error(ctx, key.error());
  auto msg = bytes_from_js(ctx, argv[1]);
  if (!msg) return throw_error(ctx, msg.error());
  auto sig = bytes_from_js(ctx, argv[2]);
  if (!sig) return throw_error(ctx, sig.error());
  auto ok = vacps::crypto::ed25519_verify(
      *key,
      std::string_view(reinterpret_cast<const char*>(msg->data()), msg->size()),
      *sig);
  if (!ok) return throw_error(ctx, ok.error());
  return JS_NewBool(ctx, *ok ? 1 : 0);
}

const JSCFunctionListEntry k_crypto_exports[] = {
    JS_CFUNC_DEF("randomBytes", 1, js_crypto_random_bytes),
    JS_CFUNC_DEF("sha256", 1, js_crypto_sha256),
    JS_CFUNC_DEF("sha256Hex", 1, js_crypto_sha256_hex),
    JS_CFUNC_DEF("toHex", 1, js_crypto_to_hex),
    JS_CFUNC_DEF("fromHex", 1, js_crypto_from_hex),
    JS_CFUNC_DEF("base64Encode", 1, js_crypto_base64_encode),
    JS_CFUNC_DEF("base64Decode", 1, js_crypto_base64_decode),
    JS_CFUNC_DEF("base64UrlEncode", 1, js_crypto_base64url_encode),
    JS_CFUNC_DEF("base64UrlDecode", 1, js_crypto_base64url_decode),
    JS_CFUNC_DEF("ed25519PublicFromPrivate", 1, js_crypto_ed25519_public_from_private),
    JS_CFUNC_DEF("ed25519SeedFromPrivateKey", 1, js_crypto_ed25519_seed_from_private_key),
    JS_CFUNC_DEF("ed25519Sign", 2, js_crypto_ed25519_sign),
    JS_CFUNC_DEF("ed25519Verify", 3, js_crypto_ed25519_verify),
};

int js_crypto_init(JSContext* ctx, JSModuleDef* m) {
  return JS_SetModuleExportList(ctx, m, k_crypto_exports, VACPS_COUNTOF(k_crypto_exports));
}

JSModuleDef* init_module_crypto(JSContext* ctx, const char* name) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_crypto_init);
  if (!m) return nullptr;
  JS_AddModuleExportList(ctx, m, k_crypto_exports, VACPS_COUNTOF(k_crypto_exports));
  return m;
}

// ── loader (native capabilities only; business script is Host-evaluated entry) ──

JSModuleDef* module_loader(JSContext* ctx, const char* module_name, void* opaque) {
  (void)opaque;
  if (std::strcmp(module_name, "vacps:log") == 0) return init_module_log(ctx, module_name);
  if (std::strcmp(module_name, "vacps:store") == 0) return init_module_store(ctx, module_name);
  if (std::strcmp(module_name, "vacps:host") == 0) return init_module_host(ctx, module_name);
  if (std::strcmp(module_name, "vacps:http") == 0) return init_module_http(ctx, module_name);
  if (std::strcmp(module_name, "vacps:fs") == 0) return init_module_fs(ctx, module_name);
  if (std::strcmp(module_name, "vacps:process") == 0) return init_module_process(ctx, module_name);
  if (std::strcmp(module_name, "vacps:crypto") == 0) return init_module_crypto(ctx, module_name);
  JS_ThrowReferenceError(ctx, "module not found: %s", module_name);
  return nullptr;
}

}  // namespace

VoidResult install_native_modules(JSRuntime* rt, JSContext* ctx) {
  if (rt == nullptr || ctx == nullptr) {
    return std::unexpected(Error{"install_native_modules: null runtime/context"});
  }
  JS_SetModuleLoaderFunc(rt, nullptr, module_loader, nullptr);
  if (auto url = install_url_global(ctx); !url) {
    return url;
  }
  if (auto enc = install_encoding_globals(ctx); !enc) {
    return enc;
  }
  return {};
}

}  // namespace vacps::js
