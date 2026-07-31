#include "quickjs/bindings/modules_init.hpp"
#include "quickjs/bindings/common.hpp"

#include "http/client.hpp"
#include "http/server.hpp"
#include "quickjs/script_request_handler.hpp"
#include "quickjs/raii/atom.hpp"
#include "quickjs/script_runtime.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/promise_bridge.hpp"
#include "quickjs/raii/value.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <quickjs.h>

namespace vacps::js {
namespace {

struct HttpServerHandle {
  std::shared_ptr<vacps::http::Server> server;
};

JSClassID g_http_server_class_id = 0;

void http_server_finalizer(JSRuntime* /*rt*/, JSValue val) {
  auto* h = static_cast<HttpServerHandle*>(JS_GetOpaque(val, g_http_server_class_id));
  if (h != nullptr && h->server) {
    // Acceptor only — never process shutdown / io_context.stop from finalizer.
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

/** True for loopback bind targets; non-loopback is rejected (no getenv). */
bool is_loopback_host(std::string_view host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

/**
 * new Server(options) — stores endpoint only; bind happens in listen().
 *
 * options: { host?, port } — host defaults to 127.0.0.1; port required (1–65535).
 * Non-loopback host is rejected with a clear error (no silent rewrite, no getenv).
 */
JSValue js_http_server_constructor(
    JSContext* ctx,
    JSValueConst /*new_target*/,
    int argc,
    JSValueConst* argv) {
  auto* raw = script_runtime_from(ctx);
  if (raw == nullptr) {
    return throw_msg(ctx, "Server: runtime not wired");
  }
  auto host = raw->shared_from_this();

  if (argc < 1 || is_nullish(argv[0]) || !is_object(argv[0])) {
    return JS_ThrowTypeError(ctx, "Server(options): options object with port required");
  }

  vacps::http::ListenEndpoint listen;
  Value h = Value::get_property_str(ctx, argv[0], "host");
  if (!h.is_nullish()) {
    auto s = converter<std::string>::from_js(ctx, h.get());
    if (!s) return throw_error(ctx, s.error());
    listen.host = std::move(*s);
  }
  Value p = Value::get_property_str(ctx, argv[0], "port");
  if (p.is_nullish()) {
    return throw_msg(ctx, "Server: port is required");
  }
  auto port = converter<std::int32_t>::from_js(ctx, p.get());
  if (!port) return throw_error(ctx, port.error());
  if (*port < 1 || *port > 65535) {
    return throw_msg(ctx, "Server: port must be in range 1-65535");
  }
  listen.port = static_cast<std::uint16_t>(*port);

  if (!is_loopback_host(listen.host)) {
    return throw_msg(
        ctx,
        "Server: non-loopback bind host is not allowed "
        "(use 127.0.0.1, localhost, or ::1)");
  }

  ensure_http_server_class(ctx);
  // C++ Server ctor stores endpoint only; acceptor bind is deferred to start()/listen().
  // Wrap ScriptRuntime in ScriptRequestHandler so HTTP transport stays QuickJS-free.
  auto handler = std::make_shared<ScriptRequestHandler>(host);
  auto server =
      std::make_shared<vacps::http::Server>(host->ioc(), std::move(listen), std::move(handler));
  auto* handle = new HttpServerHandle{std::move(server)};
  Value obj{ctx, JS_NewObjectClass(ctx, static_cast<int>(g_http_server_class_id))};
  if (obj.is_exception()) {
    delete handle;
    return obj.release();
  }
  JS_SetOpaque(obj.get(), handle);
  return obj.release();
}

/**
 * server.listen() → Promise<void>
 * start() is sync VoidResult (bind/listen); wrap in spawn_js_promise so failures reject.
 */
JSValue js_http_server_listen(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = http_server_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  if (h->server->is_open()) {
    return throw_msg(ctx, "Server.listen: already listening");
  }
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) {
    return throw_msg(ctx, "Server.listen: runtime not wired");
  }
  auto server = h->server;
  return spawn_js_promise(
      ctx,
      host,
      [server = std::move(server)](JSContext* /*c*/, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        if (server->is_open()) {
          bridge.reject_message("Server.listen: already listening");
          co_return;
        }
        if (auto r = server->start(); !r) {
          bridge.reject(r.error());
        } else {
          bridge.resolve_undefined();
        }
        co_return;
      });
}

/**
 * server.close() → Promise<void>
 * Closes acceptor/signals only — does not stop io_context or run process shutdown.
 */
JSValue js_http_server_close(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
  auto* h = http_server_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) {
    return throw_msg(ctx, "Server.close: runtime not wired");
  }
  auto server = h->server;
  return spawn_js_promise(
      ctx,
      host,
      [server = std::move(server)](JSContext* /*c*/, PromiseBridge& bridge) mutable
          -> boost::asio::awaitable<void> {
        server->close();
        bridge.resolve_undefined();
        co_return;
      });
}

/** readonly server.listening — true while acceptor is open. */
JSValue js_http_server_get_listening(JSContext* ctx, JSValueConst this_val) {
  auto* h = http_server_from_this(ctx, this_val);
  if (!h) return JS_EXCEPTION;
  return converter<bool>::to_js(ctx, h->server->is_open()).release();
}

const JSCFunctionListEntry k_http_server_proto[] = {
    JS_CFUNC_DEF("listen", 0, js_http_server_listen),
    JS_CFUNC_DEF("close", 0, js_http_server_close),
    JS_CGETSET_DEF("listening", js_http_server_get_listening, nullptr),
};

/**
 * http.request(options) → Promise<{ status, headers, body }>
 * options: { method?, url, headers?, body?, timeoutMs?, maxResponseBytes? }
 */
JSValue js_http_request(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  auto* host = script_runtime_from(ctx);
  if (host == nullptr) {
    return throw_msg(ctx, "http.request: runtime not wired");
  }
  if (argc < 1 || !is_object(argv[0])) {
    return JS_ThrowTypeError(ctx, "http.request(options)");
  }

  vacps::http::ClientRequest req;
  req.ca_bundle = host->services().ca_bundle;

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
    JS_CFUNC_DEF("request", 1, js_http_request),
};

int js_http_init(JSContext* ctx, JSModuleDef* m) {
  ensure_http_server_class(ctx);

  Value proto = Value::new_object(ctx);
  JS_SetPropertyFunctionList(
      ctx, proto.get(), k_http_server_proto, VACPS_COUNTOF(k_http_server_proto));
  JS_SetClassProto(ctx, g_http_server_class_id, proto.release());

  // export class Server (constructor)
  JSValue ctor = JS_NewCFunction2(
      ctx, js_http_server_constructor, "Server", 1, JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return -1;
  }
  // Link ctor.prototype (class proto already set via JS_SetClassProto).
  JSValue class_proto = JS_GetClassProto(ctx, g_http_server_class_id);
  JS_DefinePropertyValueStr(
      ctx,
      class_proto,
      "constructor",
      JS_DupValue(ctx, ctor),
      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(
      ctx, ctor, "prototype", class_proto, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  if (JS_SetModuleExport(ctx, m, "Server", ctor) < 0) {
    return -1;
  }

  return JS_SetModuleExportList(ctx, m, k_http_exports, VACPS_COUNTOF(k_http_exports));
}

}  // namespace

JSModuleDef* init_module_http(JSContext* ctx, const char* name, void* /*binding*/) {
  JSModuleDef* m = JS_NewCModule(ctx, name, js_http_init);
  if (!m) return nullptr;
  JS_AddModuleExport(ctx, m, "Server");
  JS_AddModuleExportList(ctx, m, k_http_exports, VACPS_COUNTOF(k_http_exports));
  return m;
}

}  // namespace vacps::js
