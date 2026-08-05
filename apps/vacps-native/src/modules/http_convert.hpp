#pragma once

/**
 * Module-local JS ↔ C++ convert for vacps:http (request + Server).
 *
 * ClientRequest.from_js decodes the public options bag only. TLS composition
 * belongs to the module-scoped Client and is never request data or a JS field.
 *
 * ClientResponse.to_js → { status: number, headers: object, body: ArrayBuffer }.
 * Duplicate response header names: last value wins (sequential SetProperty).
 *
 * Server options use camelCase → ServerOptions (host, port, maxRequestBytes,
 * maxHeaderBytes, maxResponseBytes, ioTimeoutMs, handlerTimeoutMs, backlog,
 * reuseAddress) with strict bounded integer/type validation. options is a
 * required plain object; port is required (integer 0..65535, 0 = ephemeral).
 * host, when supplied, must be a numeric IPv4/IPv6 bind literal (asio
 * make_address; never DNS) — invalid host fails synchronously at new Server
 * before any Promise is created. backlog is optional; when present it must be
 * a positive integer in 1..65535.
 * ListenAddress.host is the raw numeric bound address (IPv6 unbracketed).
 *
 * Inbound JS request (binary-first):
 *   { method, url, httpVersion, headers, body, remoteAddress }
 * url is the raw HTTP request-target; httpVersion is a conventional string
 * such as "1.1" (not the transport x10 integer); body is ArrayBuffer.
 * Duplicate request headers may collapse last-wins in the JS object; transport
 * DTO retains them.
 *
 * Callback response: { status, headers?, body? }; body accepts
 * string / ArrayBuffer / TypedArray. Domain transport validates transport-owned
 * headers (Connection, Content-Length, Transfer-Encoding, Upgrade, Trailer, TE,
 * Keep-Alive, Proxy-Connection) and limits.
 */

#include "binding/convert.hpp"
#include "http/client.hpp"
#include "http/server.hpp"
#include "qjs/owned_value.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <quickjs.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace vacps::binding {
namespace http_detail {

inline constexpr std::int64_t k_default_timeout_ms = 30'000;
inline constexpr std::int64_t k_min_timeout_ms = 1;
inline constexpr std::int64_t k_max_timeout_ms = 3'600'000;  // 1 hour

inline constexpr std::uint64_t k_default_max_response_bytes =
    8ull * 1024ull * 1024ull;
inline constexpr std::uint64_t k_min_max_response_bytes = 1;
inline constexpr std::uint64_t k_max_max_response_bytes =
    64ull * 1024ull * 1024ull;

// Server option bounds (aligned with client where applicable).
// Unspecified optional fields keep vacps::http::ServerOptions defaults.
inline constexpr std::uint64_t k_min_max_request_bytes = 1;
inline constexpr std::uint64_t k_max_max_request_bytes =
    64ull * 1024ull * 1024ull;

inline constexpr std::uint64_t k_min_max_header_bytes = 1;
inline constexpr std::uint64_t k_max_max_header_bytes = 1ull * 1024ull * 1024ull;

// JS backlog when supplied: positive only. Domain C++ may still use <=0 default.
inline constexpr std::int64_t k_min_backlog = 1;
inline constexpr std::int64_t k_max_backlog = 65535;

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

/**
 * Own string-keyed properties → header pairs. Each value must be a string.
 * Does not read inherited properties. Symbol keys are ignored.
 */
[[nodiscard]] inline Result<std::vector<std::pair<std::string, std::string>>>
headers_from_js(Env env, JSValueConst v) {
  JSContext* ctx = env.context();
  if (auto ok = require_plain_object(env, v, "headers"); !ok) {
    return std::unexpected(std::move(ok.error()));
  }

  JSPropertyEnum* tab = nullptr;
  uint32_t len = 0;
  if (JS_GetOwnPropertyNames(
          ctx, &tab, &len, v, JS_GPN_STRING_MASK) < 0) {
    clear_exception(ctx);
    return std::unexpected(Error::type("failed to enumerate headers"));
  }

  struct PropEnumGuard {
    JSContext* ctx{nullptr};
    JSPropertyEnum* tab{nullptr};
    uint32_t len{0};
    ~PropEnumGuard() {
      if (tab != nullptr) {
        JS_FreePropertyEnum(ctx, tab, len);
      }
    }
  } guard{ctx, tab, len};

  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    qjs::OwnedValue key{ctx, JS_AtomToValue(ctx, tab[i].atom)};
    if (key.is_exception()) {
      clear_exception(ctx);
      (void)key.release();
      return std::unexpected(Error::type("failed to read header name"));
    }
    auto name = Converter<std::string>::from_js(env, key.get());
    if (!name) {
      return std::unexpected(std::move(name.error()));
    }

    qjs::OwnedValue val{
        ctx, JS_GetProperty(ctx, v, tab[i].atom)};
    if (val.is_exception()) {
      clear_exception(ctx);
      (void)val.release();
      return std::unexpected(Error::type(
          std::string{"failed to read header '"} + *name + "'"));
    }
    if (!JS_IsString(val.get())) {
      return std::unexpected(Error::type(
          std::string{"headers['"} + *name + "'] must be a string"));
    }
    auto value = Converter<std::string>::from_js(env, val.get());
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    out.emplace_back(std::move(*name), std::move(*value));
  }
  return out;
}

/** Null-prototype headers object; last value wins for duplicate names. */
[[nodiscard]] inline qjs::OwnedValue headers_to_js(
    Env env,
    const std::vector<std::pair<std::string, std::string>>& headers) {
  JSContext* ctx = env.context();
  qjs::OwnedValue headers_obj{ctx, JS_NewObjectProto(ctx, JS_NULL)};
  if (headers_obj.is_exception()) {
    return headers_obj;
  }
  for (const auto& [name, value] : headers) {
    auto val = Converter<std::string>::to_js(env, value);
    if (val.is_exception()) {
      return val;
    }
    if (JS_SetPropertyStr(
            ctx, headers_obj.get(), name.c_str(), val.release()) < 0) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
  }
  return headers_obj;
}

[[nodiscard]] inline Result<std::uint64_t> bounded_u64(
    Env env,
    JSValueConst v,
    const char* name,
    std::uint64_t min_v,
    std::uint64_t max_v) {
  auto n = Converter<std::uint64_t>::from_js(env, v);
  if (!n) {
    return std::unexpected(std::move(n.error()));
  }
  if (*n < min_v || *n > max_v) {
    return std::unexpected(Error::range(std::format(
        "{} must be an integer in [{}, {}]", name, min_v, max_v)));
  }
  return *n;
}

[[nodiscard]] inline Result<std::int64_t> bounded_i64(
    Env env,
    JSValueConst v,
    const char* name,
    std::int64_t min_v,
    std::int64_t max_v) {
  auto n = Converter<std::int64_t>::from_js(env, v);
  if (!n) {
    return std::unexpected(std::move(n.error()));
  }
  if (*n < min_v || *n > max_v) {
    return std::unexpected(Error::range(std::format(
        "{} must be an integer in [{}, {}]", name, min_v, max_v)));
  }
  return *n;
}

}  // namespace http_detail

template <>
struct Converter<vacps::http::ClientRequest> {
  static Result<vacps::http::ClientRequest> from_js(Env env, JSValueConst v) {
    using vacps::http::ClientRequest;
    using http_detail::get_prop;
    using http_detail::is_nullish;
    using http_detail::require_plain_object;

    if (auto ok = require_plain_object(env, v, "HttpRequest"); !ok) {
      return std::unexpected(std::move(ok.error()));
    }

    ClientRequest req;

    // url: required nonempty string
    auto url_v = get_prop(env, v, "url");
    if (!url_v) {
      return std::unexpected(std::move(url_v.error()));
    }
    if (is_nullish(url_v->get())) {
      return std::unexpected(Error::type("url is required"));
    }
    auto url = Converter<std::string>::from_js(env, url_v->get());
    if (!url) {
      return std::unexpected(std::move(url.error()));
    }
    if (url->empty()) {
      return std::unexpected(Error::type("url must be a nonempty string"));
    }
    req.url = std::move(*url);

    // method?: string (default GET)
    auto method_v = get_prop(env, v, "method");
    if (!method_v) {
      return std::unexpected(std::move(method_v.error()));
    }
    if (!is_nullish(method_v->get())) {
      auto method = Converter<std::string>::from_js(env, method_v->get());
      if (!method) {
        return std::unexpected(std::move(method.error()));
      }
      req.method = std::move(*method);
    }

    // headers?: plain object of string values
    auto headers_v = get_prop(env, v, "headers");
    if (!headers_v) {
      return std::unexpected(std::move(headers_v.error()));
    }
    if (!is_nullish(headers_v->get())) {
      auto headers = http_detail::headers_from_js(env, headers_v->get());
      if (!headers) {
        return std::unexpected(std::move(headers.error()));
      }
      req.headers = std::move(*headers);
    }

    // body?: nullish | string | ArrayBuffer | TypedArray
    auto body_v = get_prop(env, v, "body");
    if (!body_v) {
      return std::unexpected(std::move(body_v.error()));
    }
    if (!is_nullish(body_v->get())) {
      auto body =
          Converter<std::vector<std::uint8_t>>::from_js(env, body_v->get());
      if (!body) {
        return std::unexpected(std::move(body.error()));
      }
      req.body = std::move(*body);
    }

    // timeoutMs?: finite integer in [1, 3600000], default 30000
    auto timeout_v = get_prop(env, v, "timeoutMs");
    if (!timeout_v) {
      return std::unexpected(std::move(timeout_v.error()));
    }
    if (!is_nullish(timeout_v->get())) {
      auto ms = Converter<std::int64_t>::from_js(env, timeout_v->get());
      if (!ms) {
        return std::unexpected(std::move(ms.error()));
      }
      if (*ms < http_detail::k_min_timeout_ms ||
          *ms > http_detail::k_max_timeout_ms) {
        return std::unexpected(Error::range(
            "timeoutMs must be an integer in [1, 3600000]"));
      }
      req.timeout = std::chrono::milliseconds{*ms};
    } else {
      req.timeout =
          std::chrono::milliseconds{http_detail::k_default_timeout_ms};
    }

    // maxResponseBytes?: finite integer in [1, 64 MiB], default 8 MiB
    auto max_v = get_prop(env, v, "maxResponseBytes");
    if (!max_v) {
      return std::unexpected(std::move(max_v.error()));
    }
    if (!is_nullish(max_v->get())) {
      auto n = Converter<std::uint64_t>::from_js(env, max_v->get());
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      if (*n < http_detail::k_min_max_response_bytes ||
          *n > http_detail::k_max_max_response_bytes) {
        return std::unexpected(Error::range(
            "maxResponseBytes must be an integer in [1, 67108864]"));
      }
      req.max_response_bytes = static_cast<std::size_t>(*n);
    } else {
      req.max_response_bytes =
          static_cast<std::size_t>(http_detail::k_default_max_response_bytes);
    }

    return req;
  }
};

template <>
struct Converter<vacps::http::ClientResponse> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::http::ClientResponse& resp) {
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

    if (!set(
            "status",
            Converter<std::int32_t>::to_js(
                env, static_cast<std::int32_t>(resp.status)))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }

    if (!set("headers", http_detail::headers_to_js(env, resp.headers))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }

    if (!set(
            "body",
            Converter<std::vector<std::uint8_t>>::to_js(env, resp.body))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return obj;
  }

  static qjs::OwnedValue to_js(Env env, vacps::http::ClientResponse&& resp) {
    return to_js(env, static_cast<const vacps::http::ClientResponse&>(resp));
  }
};

template <>
struct Converter<vacps::http::ServerOptions> {
  static Result<vacps::http::ServerOptions> from_js(Env env, JSValueConst v) {
    using vacps::http::ServerOptions;
    using http_detail::bounded_i64;
    using http_detail::bounded_u64;
    using http_detail::get_prop;
    using http_detail::is_nullish;
    using http_detail::require_plain_object;

    // Required plain object — no nullish / array / permissive default bag.
    if (auto ok = require_plain_object(env, v, "ServerOptions"); !ok) {
      return std::unexpected(std::move(ok.error()));
    }

    ServerOptions opts;

    auto host_v = get_prop(env, v, "host");
    if (!host_v) {
      return std::unexpected(std::move(host_v.error()));
    }
    if (!is_nullish(host_v->get())) {
      auto host = Converter<std::string>::from_js(env, host_v->get());
      if (!host) {
        return std::unexpected(std::move(host.error()));
      }
      if (host->empty()) {
        return std::unexpected(Error::type(
            "host must be a nonempty numeric IPv4/IPv6 bind literal"));
      }
      // Bind literal only — never DNS. Fail at new Server (before listen
      // Promise) so "localhost" etc. are synchronous TypeErrors.
      boost::system::error_code ec;
      (void)boost::asio::ip::make_address(*host, ec);
      if (ec) {
        return std::unexpected(Error::type(std::format(
            "host must be a numeric IPv4/IPv6 bind literal (got '{}')",
            *host)));
      }
      opts.host = std::move(*host);
    }

    // port is required (0 = ephemeral).
    auto port_v = get_prop(env, v, "port");
    if (!port_v) {
      return std::unexpected(std::move(port_v.error()));
    }
    if (is_nullish(port_v->get())) {
      return std::unexpected(Error::type("port is required"));
    }
    {
      auto port = bounded_i64(env, port_v->get(), "port", 0, 65535);
      if (!port) {
        return std::unexpected(std::move(port.error()));
      }
      opts.port = static_cast<std::uint16_t>(*port);
    }

    auto max_req_v = get_prop(env, v, "maxRequestBytes");
    if (!max_req_v) {
      return std::unexpected(std::move(max_req_v.error()));
    }
    if (!is_nullish(max_req_v->get())) {
      auto n = bounded_u64(
          env,
          max_req_v->get(),
          "maxRequestBytes",
          http_detail::k_min_max_request_bytes,
          http_detail::k_max_max_request_bytes);
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      opts.max_request_bytes = static_cast<std::size_t>(*n);
    }

    auto max_hdr_v = get_prop(env, v, "maxHeaderBytes");
    if (!max_hdr_v) {
      return std::unexpected(std::move(max_hdr_v.error()));
    }
    if (!is_nullish(max_hdr_v->get())) {
      auto n = bounded_u64(
          env,
          max_hdr_v->get(),
          "maxHeaderBytes",
          http_detail::k_min_max_header_bytes,
          http_detail::k_max_max_header_bytes);
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      opts.max_header_bytes = static_cast<std::size_t>(*n);
    }

    auto max_res_v = get_prop(env, v, "maxResponseBytes");
    if (!max_res_v) {
      return std::unexpected(std::move(max_res_v.error()));
    }
    if (!is_nullish(max_res_v->get())) {
      auto n = bounded_u64(
          env,
          max_res_v->get(),
          "maxResponseBytes",
          http_detail::k_min_max_response_bytes,
          http_detail::k_max_max_response_bytes);
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      opts.max_response_bytes = static_cast<std::size_t>(*n);
    }

    auto io_v = get_prop(env, v, "ioTimeoutMs");
    if (!io_v) {
      return std::unexpected(std::move(io_v.error()));
    }
    if (!is_nullish(io_v->get())) {
      auto ms = bounded_i64(
          env,
          io_v->get(),
          "ioTimeoutMs",
          http_detail::k_min_timeout_ms,
          http_detail::k_max_timeout_ms);
      if (!ms) {
        return std::unexpected(std::move(ms.error()));
      }
      opts.io_timeout = std::chrono::milliseconds{*ms};
    }

    auto handler_v = get_prop(env, v, "handlerTimeoutMs");
    if (!handler_v) {
      return std::unexpected(std::move(handler_v.error()));
    }
    if (!is_nullish(handler_v->get())) {
      auto ms = bounded_i64(
          env,
          handler_v->get(),
          "handlerTimeoutMs",
          http_detail::k_min_timeout_ms,
          http_detail::k_max_timeout_ms);
      if (!ms) {
        return std::unexpected(std::move(ms.error()));
      }
      opts.handler_timeout = std::chrono::milliseconds{*ms};
    }

    auto backlog_v = get_prop(env, v, "backlog");
    if (!backlog_v) {
      return std::unexpected(std::move(backlog_v.error()));
    }
    if (!is_nullish(backlog_v->get())) {
      // Optional, but when present must be a positive integer in [1, 65535].
      auto n = bounded_i64(
          env,
          backlog_v->get(),
          "backlog",
          http_detail::k_min_backlog,
          http_detail::k_max_backlog);
      if (!n) {
        return std::unexpected(std::move(n.error()));
      }
      opts.backlog = static_cast<int>(*n);
    }

    auto reuse_v = get_prop(env, v, "reuseAddress");
    if (!reuse_v) {
      return std::unexpected(std::move(reuse_v.error()));
    }
    if (!is_nullish(reuse_v->get())) {
      auto b = Converter<bool>::from_js(env, reuse_v->get());
      if (!b) {
        return std::unexpected(std::move(b.error()));
      }
      opts.reuse_address = *b;
    }

    return opts;
  }
};

template <>
struct Converter<vacps::http::ListenAddress> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::http::ListenAddress& addr) {
    JSContext* ctx = env.context();
    auto obj = env.new_object();
    if (obj.is_exception()) {
      return obj;
    }
    auto host = Converter<std::string>::to_js(env, addr.host);
    if (host.is_exception()) {
      return host;
    }
    if (JS_SetPropertyStr(ctx, obj.get(), "host", host.release()) < 0) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    auto port = Converter<std::int32_t>::to_js(
        env, static_cast<std::int32_t>(addr.port));
    if (port.is_exception()) {
      return port;
    }
    if (JS_SetPropertyStr(ctx, obj.get(), "port", port.release()) < 0) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return obj;
  }

  static qjs::OwnedValue to_js(Env env, vacps::http::ListenAddress&& addr) {
    return to_js(env, static_cast<const vacps::http::ListenAddress&>(addr));
  }
};

template <>
struct Converter<vacps::http::ServerRequest> {
  static qjs::OwnedValue to_js(
      Env env,
      const vacps::http::ServerRequest& req) {
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

    if (!set("method", Converter<std::string>::to_js(env, req.method))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    // Raw HTTP request-target (path + query).
    if (!set("url", Converter<std::string>::to_js(env, req.target))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }

    // Conventional version string ("1.1"), not the transport x10 integer.
    const unsigned major = req.version / 10u;
    const unsigned minor = req.version % 10u;
    const std::string http_version = std::format("{}.{}", major, minor);
    if (!set(
            "httpVersion",
            Converter<std::string>::to_js(env, http_version))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }

    if (!set("headers", http_detail::headers_to_js(env, req.headers))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }

    if (!set(
            "body",
            Converter<std::vector<std::uint8_t>>::to_js(env, req.body))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }

    if (!set(
            "remoteAddress",
            Converter<std::string>::to_js(env, req.remote_address))) {
      return qjs::OwnedValue::take(ctx, JS_EXCEPTION);
    }
    return obj;
  }

  static qjs::OwnedValue to_js(Env env, vacps::http::ServerRequest&& req) {
    return to_js(env, static_cast<const vacps::http::ServerRequest&>(req));
  }
};

template <>
struct Converter<vacps::http::ServerResponse> {
  static Result<vacps::http::ServerResponse> from_js(Env env, JSValueConst v) {
    using vacps::http::ServerResponse;
    using http_detail::get_prop;
    using http_detail::is_nullish;
    using http_detail::require_plain_object;

    if (auto ok = require_plain_object(env, v, "ServerResponse"); !ok) {
      return std::unexpected(std::move(ok.error()));
    }

    ServerResponse res;

    auto status_v = get_prop(env, v, "status");
    if (!status_v) {
      return std::unexpected(std::move(status_v.error()));
    }
    if (is_nullish(status_v->get())) {
      return std::unexpected(Error::type("status is required"));
    }
    auto status = Converter<std::int32_t>::from_js(env, status_v->get());
    if (!status) {
      return std::unexpected(std::move(status.error()));
    }
    res.status = static_cast<int>(*status);

    auto headers_v = get_prop(env, v, "headers");
    if (!headers_v) {
      return std::unexpected(std::move(headers_v.error()));
    }
    if (!is_nullish(headers_v->get())) {
      auto headers = http_detail::headers_from_js(env, headers_v->get());
      if (!headers) {
        return std::unexpected(std::move(headers.error()));
      }
      res.headers = std::move(*headers);
    }

    auto body_v = get_prop(env, v, "body");
    if (!body_v) {
      return std::unexpected(std::move(body_v.error()));
    }
    if (!is_nullish(body_v->get())) {
      auto body =
          Converter<std::vector<std::uint8_t>>::from_js(env, body_v->get());
      if (!body) {
        return std::unexpected(std::move(body.error()));
      }
      res.body = std::move(*body);
    }

    return res;
  }
};

}  // namespace vacps::binding
