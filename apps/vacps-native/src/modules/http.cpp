#include "modules/bindings.hpp"

#include "binding/async_function.hpp"
#include "binding/class.hpp"
#include "binding/module.hpp"
#include "http/client.hpp"
#include "modules/catalog.hpp"
#include "modules/http_convert.hpp"
#include "modules/http_server_native.hpp"
#include "runtime/error.hpp"
#include "runtime/js_promise_await.hpp"
#include "runtime/runtime_async.hpp"
#include "runtime/runtime_callbacks.hpp"

#include <quickjs.h>

#include <chrono>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;
namespace asio = boost::asio;

using http_server::ServerNative;

constexpr const char* k_http_exports[] = {
    "request",
    "Server",
};

/**
 * Map domain Result → runtime::Result at the module edge.
 * Preserves operation / system_code (ETIMEDOUT, ECANCELED, …).
 */
template <class T>
[[nodiscard]] runtime::Result<T> map_http_result(vacps::Result<T> r) {
  if (!r) {
    return std::unexpected(runtime::Error::from_domain(std::move(r.error())));
  }
  return std::move(*r);
}

[[nodiscard]] runtime::VoidResult map_http_void(vacps::VoidResult r) {
  if (!r) {
    return std::unexpected(runtime::Error::from_domain(std::move(r.error())));
  }
  return {};
}

/** Domain Error for handler failures (transport maps to fixed 500 / 504). */
[[nodiscard]] vacps::Error handler_error(
    std::string message,
    int system_code = 0) {
  return vacps::Error{
      std::move(message),
      std::string{"http.server.handler"},
      system_code};
}

/**
 * Transport ServerHandler: weak native only (no Native → Server → handler →
 * Native cycle). Runs on the owner executor; call_and_await timeout is 0
 * (transport owns handler_timeout wall deadline + stop_source; not a hard
 * destroy bound).
 *
 * Operational lifetime only: weak_ptr expiration and a genuinely released
 * onRequest root (explicit close / ClassJsEdges::release). Mandatory
 * composition means Callbacks/Async are always installed at construction.
 */
[[nodiscard]] vacps::http::ServerHandler make_server_handler(
    std::weak_ptr<ServerNative> weak) {
  return [weak = std::move(weak)](
             std::stop_token stop,
             vacps::http::ServerRequest req)
             -> asio::awaitable<vacps::Result<vacps::http::ServerResponse>> {
    auto self = weak.lock();
    if (!self || self->on_request.empty()) {
      co_return std::unexpected(
          handler_error("http.server.handler: callback unavailable"));
    }

    JSContext* ctx = self->on_request.context();
    binding::Env env{ctx};
    qjs::OwnedValue request_value =
        binding::Converter<vacps::http::ServerRequest>::to_js(
            env, std::move(req));
    if (request_value.is_exception()) {
      if (JS_HasException(ctx)) {
        JSValue ex = JS_GetException(ctx);
        JS_FreeValue(ctx, ex);
      }
      (void)request_value.release();
      co_return std::unexpected(
          handler_error("http.server.handler: failed to encode request"));
    }
    std::vector<qjs::OwnedValue> args;
    args.push_back(std::move(request_value));

    // Borrowed callable for the synchronous JS_Call only; root stays in
    // ServerNative. timeout=0: transport owns the wall budget via stop.
    auto settled = co_await self->callbacks.call_and_await(
        self->on_request.get(),
        std::move(args),
        runtime::JsAwaitOptions{
            .timeout = std::chrono::milliseconds{0},
            .stop = std::move(stop),
        });

    if (!settled) {
      co_return std::unexpected(handler_error(
          settled.error().message.empty()
              ? std::string{"http.server.handler: callback failed"}
              : settled.error().message,
          settled.error().system_code));
    }

    auto decoded = binding::Converter<vacps::http::ServerResponse>::from_js(
        env, settled->get());
    // Drop settled value before returning into the transport.
    settled->reset();
    if (!decoded) {
      co_return std::unexpected(handler_error(
          decoded.error().message.empty()
              ? std::string{"http.server.handler: invalid response"}
              : decoded.error().message));
    }
    co_return std::move(*decoded);
  };
}

[[nodiscard]] binding::Result<std::shared_ptr<ServerNative>> construct_server(
    const binding::CallbackInfo& info) {
  JSContext* ctx = info.context();

  if (auto argc = info.check_argc(2, "Server"); !argc) {
    return std::unexpected(std::move(argc.error()));
  }

  auto options =
      binding::Converter<vacps::http::ServerOptions>::from_js(
          info.env(), info[0].get());
  if (!options) {
    return std::unexpected(std::move(options.error()));
  }

  JSValueConst on_req = info[1].get();
  if (!JS_IsFunction(ctx, on_req)) {
    return std::unexpected(
        binding::Error::type("Server onRequest must be a function"));
  }

  // Composition is caller-established at catalog install (Narrow).
  Runtime::Callbacks& callbacks = callbacks_runtime_from_context(ctx);
  Runtime::Async& async = async_runtime_from_context(ctx);
  asio::any_io_executor executor = async.executor();

  auto native = std::make_shared<ServerNative>(
      callbacks, qjs::OwnedValue{ctx, JS_DupValue(ctx, on_req)});

  // No Runtime JS-handle cleanup registration: correct JS shutdown
  // (explicit Server.close) + Asio natural drain is the contract.
  // ClassJsEdges::release remains VM bookkeeping on finalizer only.

  native->server.emplace(
      std::move(executor),
      std::move(*options),
      make_server_handler(std::weak_ptr<ServerNative>{native}));

  return native;
}

/**
 * Module init (phase 2): outbound request + inbound Server class.
 * No C++ exception may escape this C callback.
 *
 * Runtime::Async / Runtime::Callbacks / ca_bundle are recovered from the
 * mandatory JSRuntime composition opaque.
 */
int initialize_http(JSContext* ctx, JSModuleDef* m) noexcept {
  try {
    Runtime::Async* async = &async_runtime_from_context(ctx);
    binding::Env env{ctx, async};
    binding::ModuleBuilder mod{env};

    // One runtime-scoped outbound client. The function slot and every in-flight
    // async call share ownership, so the pool naturally outlives its requests.
    auto client = std::make_shared<vacps::http::Client>(
        async->executor(),
        composition_from_context(ctx).ca_bundle);

    auto export_async = [&](const char* name, auto fn, int length) -> bool {
      qjs::OwnedValue func = binding::create_async_function(
          env, name, std::move(fn), length);
      return mod.set_export(m, name, std::move(func)) == 0;
    };

    // request(options) → Promise<{ status, headers, body: ArrayBuffer }>
    // Options decode (Converter) is synchronous before Promise creation.
    if (!export_async(
            "request",
            [client](
                std::stop_token stop,
                vacps::http::ClientRequest req)
                -> runtime::Task<vacps::http::ClientResponse> {
              co_return map_http_result(
                  co_await client->request(stop, std::move(req)));
            },
            1)) {
      return -1;
    }

    using ServerBuilder = binding::ClassBuilder<ServerNative>;

    auto committed =
        ServerBuilder{env, "Server"}
            .constructor(
                [](const binding::CallbackInfo& info)
                    -> binding::Result<std::shared_ptr<ServerNative>> {
                  return construct_server(info);
                },
                2)
            .readonly(
                "listening",
                [](const ServerNative& self) -> bool {
                  return self.server->listening();
                })
            .readonly(
                "address",
                [](const binding::CallbackInfo& info,
                   const ServerNative& self) -> qjs::OwnedValue {
                  auto addr = self.server->address();
                  if (!addr.has_value()) {
                    return info.env().undefined();
                  }
                  return binding::Converter<vacps::http::ListenAddress>::to_js(
                      info.env(), *addr);
                })
            .async_method(
                "listen",
                [](std::stop_token /*stop*/,
                   std::shared_ptr<ServerNative> self) mutable
                    -> runtime::Task<vacps::http::ListenAddress> {
                  // Owner executor (async method frame); domain requires it.
                  co_return map_http_result(
                      co_await self->server->async_listen());
                },
                0)
            .async_method(
                "close",
                [](std::stop_token /*stop*/,
                   std::shared_ptr<ServerNative> self) mutable
                    -> runtime::Task<void> {
                  // Idempotent domain close; await drain on owner executor.
                  auto closed =
                      map_http_void(co_await self->server->async_close());
                  if (!closed) {
                    co_return std::unexpected(std::move(closed.error()));
                  }
                  // Business shutdown: after drain, drop callback root on the
                  // owner thread. ClassJsEdges::release is separate VM
                  // bookkeeping if the JS object is finalized instead.
                  self->drop_on_request();
                  co_return runtime::success();
                },
                0)
            .commit();

    if (!committed) {
      (void)binding::throw_error(ctx, committed.error());
      return -1;
    }
    if (mod.set_export(m, "Server", std::move(*committed)) != 0) {
      return -1;
    }

    return 0;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return -1;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return -1;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "http module init failed");
    }
    return -1;
  }
}

}  // namespace

JSModuleDef* init_module_http(JSContext* ctx, const char* name) {
  try {
    JSModuleDef* m = JS_NewCModule(ctx, name, initialize_http);
    if (m == nullptr) {
      return nullptr;
    }

    for (const char* export_name : k_http_exports) {
      if (binding::ModuleBuilder::declare_export(ctx, m, export_name) < 0) {
        if (!JS_HasException(ctx)) {
          (void)binding::throw_internal(
              ctx, "http module: declare_export failed");
        }
        return nullptr;
      }
    }
    return m;
  } catch (const std::bad_alloc&) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "allocation failed");
    }
    return nullptr;
  } catch (const std::exception& ex) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, ex.what());
    }
    return nullptr;
  } catch (...) {
    if (!JS_HasException(ctx)) {
      (void)binding::throw_internal(ctx, "http module load failed");
    }
    return nullptr;
  }
}

}  // namespace vacps::js
