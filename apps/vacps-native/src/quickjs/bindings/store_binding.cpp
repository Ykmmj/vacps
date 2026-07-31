/**
 * vacps:store QuickJS binding (design n1.md §六).
 *
 * JS Store ↔ storage::Store (shared_ptr via StoreHandle opaque).
 * Module exports: class Store with static open only.
 * No free open(); no begin/commit/rollback (use transaction()).
 */

#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/bindings/common.hpp"

#include "fs/async.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/promise_bridge.hpp"
#include "quickjs/raii/value.hpp"
#include "storage/store.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio/post.hpp>
#include <quickjs.h>

namespace vacps::js {
namespace {

using vacps::storage::ExpectedChanges;
using vacps::storage::OpenMode;
using vacps::storage::OpenOptions;
using vacps::storage::RunResult;
using vacps::storage::StepType;
using vacps::storage::Store;
using vacps::storage::TransactionResult;
using vacps::storage::TransactionStep;

/**
 * Opaque for JS Store instances.
 * shared_ptr keeps storage::Store alive across async offload + GC finalizer.
 * host is only used to defer sqlite close onto the offload pool when the JS
 * object is GC'd while the connection is still open.
 */
struct StoreHandle {
  std::shared_ptr<Store> store;
  ScriptRuntime* host{nullptr};
};

JSClassID g_store_class_id = 0;

void store_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<StoreHandle*>(JS_GetOpaque(val, g_store_class_id));
  if (h == nullptr) {
    return;
  }
  if (h->store && !h->store->closed() && h->host != nullptr) {
    auto store = h->store;
    try {
      auto sp = h->host->shared_from_this();
      boost::asio::post(sp->services().pool, [store]() { (void)store->close(); });
    } catch (...) {
      // ScriptRuntime already tearing down — close on this thread.
      (void)store->close();
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
  if (h == nullptr || !h->store) {
    JS_ThrowTypeError(ctx, "Store method requires an open Store instance");
    return nullptr;
  }
  return h;
}

JSValue make_store_object(JSContext* ctx, ScriptRuntime* host, std::shared_ptr<Store> store) {
  ensure_store_class(ctx);
  auto* handle = new StoreHandle{std::move(store), host};
  Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(g_store_class_id))};
  if (obj.is_exception()) {
    delete handle;
    return obj.release();
  }
  JS_SetOpaque(obj.get(), handle);
  return obj.release();
}

Value run_result_to_js(JSContext* ctx, const RunResult& r) {
  auto obj = Value::new_object(ctx);
  obj.set_property_str("changes", converter<std::int64_t>::to_js(ctx, r.changes));
  obj.set_property_str(
      "lastInsertRowid", converter<std::int64_t>::to_js(ctx, r.last_insert_rowid));
  return obj;
}

Value transaction_result_to_js(JSContext* ctx, const TransactionResult& r) {
  return std::visit(
      [&](const auto& v) -> Value {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, RunResult>) {
          return run_result_to_js(ctx, v);
        } else {
          return query_result_to_js(ctx, v);
        }
      },
      r);
}

/** Parse StoreOpenOptions from JS object (mode optional). */
Result<OpenOptions> open_options_from_js(JSContext* ctx, JSValueConst v) {
  OpenOptions opts;
  if (is_nullish(v)) {
    return opts;
  }
  if (!is_object(v) || is_null(v)) {
    return std::unexpected(Error{"Store.open: options must be an object"});
  }
  Value mode_v = Value::get_property_str(ctx, v, "mode");
  if (mode_v.is_nullish()) {
    return opts;
  }
  auto mode_s = converter<std::string>::from_js(ctx, mode_v.get());
  if (!mode_s) {
    return std::unexpected(std::move(mode_s.error()));
  }
  if (*mode_s == "read-only") {
    opts.mode = OpenMode::ReadOnly;
  } else if (*mode_s == "read-write") {
    opts.mode = OpenMode::ReadWrite;
  } else if (*mode_s == "read-write-create") {
    opts.mode = OpenMode::ReadWriteCreate;
  } else {
    return std::unexpected(Error{
        "Store.open: mode must be \"read-only\" | \"read-write\" | \"read-write-create\""});
  }
  return opts;
}

Result<std::optional<ExpectedChanges>> expected_changes_from_js(
    JSContext* ctx,
    JSValueConst v) {
  if (is_nullish(v)) {
    return std::optional<ExpectedChanges>{};
  }
  if (!is_object(v) || is_null(v)) {
    return std::unexpected(Error{"expectedChanges must be an object"});
  }

  auto read_n = [&](const char* key) -> Result<std::optional<std::int64_t>> {
    Value p = Value::get_property_str(ctx, v, key);
    if (p.is_nullish()) {
      return std::optional<std::int64_t>{};
    }
    auto n = converter<std::int64_t>::from_js(ctx, p.get());
    if (!n) {
      return std::unexpected(std::move(n.error()));
    }
    return std::optional<std::int64_t>{*n};
  };

  auto exactly = read_n("exactly");
  if (!exactly) return std::unexpected(std::move(exactly.error()));
  auto at_least = read_n("atLeast");
  if (!at_least) return std::unexpected(std::move(at_least.error()));
  auto at_most = read_n("atMost");
  if (!at_most) return std::unexpected(std::move(at_most.error()));

  const int set =
      (exactly->has_value() ? 1 : 0) + (at_least->has_value() ? 1 : 0) +
      (at_most->has_value() ? 1 : 0);
  if (set != 1) {
    return std::unexpected(Error{
        "expectedChanges must have exactly one of: exactly, atLeast, atMost"});
  }

  ExpectedChanges exp;
  if (exactly->has_value()) {
    exp.kind = ExpectedChanges::Kind::Exactly;
    exp.n = **exactly;
  } else if (at_least->has_value()) {
    exp.kind = ExpectedChanges::Kind::AtLeast;
    exp.n = **at_least;
  } else {
    exp.kind = ExpectedChanges::Kind::AtMost;
    exp.n = **at_most;
  }
  return std::optional<ExpectedChanges>{exp};
}

struct QueryOpts {
  std::size_t max_rows{vacps::storage::Database::kDefaultMaxQueryRows};
  std::optional<std::size_t> max_bytes;
};

Result<QueryOpts> query_options_from_js(JSContext* ctx, JSValueConst v) {
  QueryOpts opts;
  if (is_nullish(v)) {
    return opts;
  }
  if (!is_object(v) || is_null(v)) {
    return std::unexpected(Error{"query options must be an object"});
  }
  Value mr = Value::get_property_str(ctx, v, "maxRows");
  if (!mr.is_nullish()) {
    auto n = converter<std::int64_t>::from_js(ctx, mr.get());
    if (!n) return std::unexpected(std::move(n.error()));
    if (*n < 0) {
      return std::unexpected(Error{"query options.maxRows must be >= 0"});
    }
    opts.max_rows = static_cast<std::size_t>(*n);
  }
  Value mb = Value::get_property_str(ctx, v, "maxBytes");
  if (!mb.is_nullish()) {
    auto n = converter<std::int64_t>::from_js(ctx, mb.get());
    if (!n) return std::unexpected(std::move(n.error()));
    if (*n < 0) {
      return std::unexpected(Error{"query options.maxBytes must be >= 0"});
    }
    opts.max_bytes = static_cast<std::size_t>(*n);
  }
  return opts;
}

/** Store cannot be constructed with `new`; use Store.open. */
JSValue js_store_ctor(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_ThrowTypeError(
      ctx, "Store cannot be constructed with new; use Store.open(path, options?)");
}

/**
 * Store.open(path, options?) → Promise<Store>
 * Factory: creates storage::Store on the host offload pool.
 */
JSValue js_store_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "Store.open: runtime not wired");
  if (argc < 1) return JS_ThrowTypeError(ctx, "Store.open(path, options?)");
  auto path = converter<std::string>::from_js(ctx, argv[0]);
  if (!path) return throw_error(ctx, path.error());

  OpenOptions options;
  if (argc >= 2 && !is_nullish(argv[1])) {
    auto o = open_options_from_js(ctx, argv[1]);
    if (!o) return throw_error(ctx, o.error());
    options = std::move(*o);
  }

  return spawn_js_promise(
      ctx,
      host,
      [path = std::move(*path), options = std::move(options), host](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto opened = co_await vacps::fs::async_offload(
            host->services().pool,
            [path = std::move(path), options = std::move(options)]() mutable
                -> Result<std::shared_ptr<Store>> {
              return Store::open(std::move(path), std::move(options));
            });
        if (!opened) {
          bridge.reject(opened.error());
          co_return;
        }
        Value obj{c, make_store_object(c, host, std::move(*opened))};
        if (obj.is_exception()) {
          bridge.reject_message("Store.open: failed to allocate Store object");
          co_return;
        }
        bridge.resolve(std::move(obj));
        co_return;
      });
}

JSValue js_store_exec(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "Store.exec: runtime not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "store.exec(sql)");
  auto sql = converter<std::string>::from_js(ctx, argv[0]);
  if (!sql) return throw_error(ctx, sql.error());
  auto store = h->store;

  return spawn_js_promise(
      ctx,
      host,
      [store, sql = std::move(*sql), host](JSContext*, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(host->services().pool, [store, sql]() -> VoidResult {
          return store->exec(sql);
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
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "Store.run: runtime not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "store.run(sql[, params])");
  auto sql = converter<std::string>::from_js(ctx, argv[0]);
  if (!sql) return throw_error(ctx, sql.error());
  std::vector<SqlValue> params;
  if (argc >= 2 && !is_nullish(argv[1])) {
    auto p = sql_params_from_js(ctx, argv[1]);
    if (!p) return throw_error(ctx, p.error());
    params = std::move(*p);
  }
  auto store = h->store;

  return spawn_js_promise(
      ctx,
      host,
      [store, sql = std::move(*sql), params = std::move(params), host](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(
            host->services().pool,
            [store, sql, params]() -> Result<RunResult> {
              return store->run(sql, params);
            });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        bridge.resolve(run_result_to_js(c, *r));
        co_return;
      });
}

/**
 * store.query(sql, params?, options?) → Promise<Row[]>
 * options: { maxRows?, maxBytes? }
 * If the second arg is a non-array object, it is treated as options (no params).
 */
JSValue js_store_query(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "Store.query: runtime not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (argc < 1) return JS_ThrowTypeError(ctx, "store.query(sql[, params][, options])");
  auto sql = converter<std::string>::from_js(ctx, argv[0]);
  if (!sql) return throw_error(ctx, sql.error());

  std::vector<SqlValue> params;
  QueryOpts qopts;
  if (argc >= 2 && !is_nullish(argv[1])) {
    if (is_array(ctx, argv[1])) {
      auto p = sql_params_from_js(ctx, argv[1]);
      if (!p) return throw_error(ctx, p.error());
      params = std::move(*p);
      if (argc >= 3 && !is_nullish(argv[2])) {
        auto o = query_options_from_js(ctx, argv[2]);
        if (!o) return throw_error(ctx, o.error());
        qopts = std::move(*o);
      }
    } else if (is_object(argv[1])) {
      auto o = query_options_from_js(ctx, argv[1]);
      if (!o) return throw_error(ctx, o.error());
      qopts = std::move(*o);
    } else {
      return JS_ThrowTypeError(ctx, "store.query: params must be an array or options object");
    }
  }
  auto store = h->store;

  return spawn_js_promise(
      ctx,
      host,
      [store, sql = std::move(*sql), params = std::move(params), qopts, host](
          JSContext* c, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        auto qr = co_await vacps::fs::async_offload(
            host->services().pool,
            [store, sql, params, qopts]() -> Result<QueryResult> {
              return store->query(sql, params, qopts.max_rows, qopts.max_bytes);
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
 * store.transaction(steps) → Promise<(RunResult|Row[])[]>
 * steps: Array<{ sql, params?, type?: "run"|"query", expectedChanges? }>
 * Entire unit: one offload job + Store mutex + BEGIN IMMEDIATE … COMMIT.
 * expectedChanges checked after each step; mismatch → ROLLBACK before later steps.
 */
JSValue js_store_transaction(
    JSContext* ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "Store.transaction: runtime not wired");
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

  std::vector<TransactionStep> steps;
  steps.reserve(static_cast<std::size_t>(*len_r));
  for (std::int32_t i = 0; i < *len_r; ++i) {
    Value el = Value::get_property_uint32(ctx, argv[0], static_cast<std::uint32_t>(i));
    if (!el.is_object() || el.is_null()) {
      return JS_ThrowTypeError(ctx, "store.transaction: each step must be an object");
    }
    Value sql_v = Value::get_property_str(ctx, el.get(), "sql");
    auto sql = converter<std::string>::from_js(ctx, sql_v.get());
    if (!sql) return throw_error(ctx, sql.error());

    TransactionStep step;
    step.sql = std::move(*sql);
    step.type = StepType::Run;

    Value type_v = Value::get_property_str(ctx, el.get(), "type");
    if (!type_v.is_nullish()) {
      auto ts = converter<std::string>::from_js(ctx, type_v.get());
      if (!ts) return throw_error(ctx, ts.error());
      if (*ts == "query") {
        step.type = StepType::Query;
      } else if (*ts == "run") {
        step.type = StepType::Run;
      } else {
        return JS_ThrowTypeError(ctx, "store.transaction: type must be \"run\" or \"query\"");
      }
    }

    Value params_v = Value::get_property_str(ctx, el.get(), "params");
    if (!params_v.is_nullish()) {
      auto p = sql_params_from_js(ctx, params_v.get());
      if (!p) return throw_error(ctx, p.error());
      step.params = std::move(*p);
    }

    Value exp_v = Value::get_property_str(ctx, el.get(), "expectedChanges");
    auto exp = expected_changes_from_js(ctx, exp_v.get());
    if (!exp) return throw_error(ctx, exp.error());
    step.expected_changes = std::move(*exp);

    steps.push_back(std::move(step));
  }

  auto store = h->store;
  return spawn_js_promise(
      ctx,
      host,
      [store, steps = std::move(steps), host](JSContext* c, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        auto r = co_await vacps::fs::async_offload(
            host->services().pool,
            [store, steps = std::move(steps)]() mutable
                -> Result<std::vector<TransactionResult>> {
              return store->transaction(steps);
            });
        if (!r) {
          bridge.reject(r.error());
          co_return;
        }
        auto arr = Value::new_array(c);
        for (std::uint32_t i = 0; i < r->size(); ++i) {
          arr.set_property_uint32(i, transaction_result_to_js(c, (*r)[i]));
        }
        bridge.resolve(std::move(arr));
        co_return;
      });
}

/** Design §六: readonly path. */
JSValue js_store_get_path(JSContext* ctx, JSValueConst this_val) {
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<std::string>::to_js(ctx, h->store->path()).release();
}

/** Design §六: readonly closed. */
JSValue js_store_get_closed(JSContext* ctx, JSValueConst this_val) {
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->store->closed()).release();
}

JSValue js_store_close(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) return throw_msg(ctx, "Store.close: runtime not wired");
  auto* h = store_handle_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto store = h->store;
  return spawn_js_promise(
      ctx,
      host,
      [store, host](JSContext*, PromiseBridge& bridge) mutable -> boost::asio::awaitable<void> {
        co_await vacps::fs::async_offload(host->services().pool, [store]() {
          (void)store->close();
          return true;
        });
        bridge.resolve_undefined();
        co_return;
      });
}

const JSCFunctionListEntry k_store_proto[] = {
    JS_CGETSET_DEF("path", js_store_get_path, nullptr),
    JS_CGETSET_DEF("closed", js_store_get_closed, nullptr),
    JS_CFUNC_DEF("exec", 1, js_store_exec),
    JS_CFUNC_DEF("run", 2, js_store_run),
    JS_CFUNC_DEF("query", 3, js_store_query),
    JS_CFUNC_DEF("transaction", 1, js_store_transaction),
    JS_CFUNC_DEF("close", 0, js_store_close),
};

int js_store_init(JSContext* ctx, JSModuleDef* m) {
  ensure_store_class(ctx);

  Value proto = Value::new_object(ctx);
  JS_SetPropertyFunctionList(ctx, proto.get(), k_store_proto, VACPS_COUNTOF(k_store_proto));
  JS_SetClassProto(ctx, g_store_class_id, proto.duplicate().release());

  // Constructor rejects `new Store()`; static open is the factory.
  Value ctor{
      ctx,
      JS_NewCFunction2(
          ctx, js_store_ctor, "Store", 0, JS_CFUNC_constructor, 0)};
  if (ctor.is_exception()) {
    return -1;
  }
  JS_SetConstructor(ctx, ctor.get(), proto.get());
  JS_SetPropertyStr(
      ctx,
      ctor.get(),
      "open",
      JS_NewCFunction(ctx, js_store_open, "open", 2));

  return JS_SetModuleExport(ctx, m, "Store", ctor.release());
}

}  // namespace

JSModuleDef* init_module_store(JSContext* ctx, const char* name, void* /*binding*/) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_store_init);
  if (!m) return nullptr;
  JS_AddModuleExport(ctx, m, "Store");
  return m;
}

}  // namespace vacps::js
