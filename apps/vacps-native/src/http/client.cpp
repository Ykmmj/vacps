#include "http/client.hpp"

#include "app/config.hpp"
#include "app/log.hpp"

#include <ada.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace vacps::http {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

http::verb parse_verb(std::string_view method) {
  if (ieq(method, "GET")) return http::verb::get;
  if (ieq(method, "POST")) return http::verb::post;
  if (ieq(method, "PUT")) return http::verb::put;
  if (ieq(method, "DELETE")) return http::verb::delete_;
  if (ieq(method, "PATCH")) return http::verb::patch;
  if (ieq(method, "HEAD")) return http::verb::head;
  if (ieq(method, "OPTIONS")) return http::verb::options;
  return http::verb::unknown;
}

/** Host header: hostname, or hostname:port when non-default. */
std::string host_header_value(const ParsedUrl& url) {
  const bool default_port =
      (url.scheme == "https" && url.port == "443") ||
      (url.scheme == "http" && url.port == "80");
  if (default_port) return url.host;
  return url.host + ":" + url.port;
}

ssl::context make_tls_client_context(const std::string& ca_path) {
  ssl::context ctx{ssl::context::tls_client};
  ctx.set_options(
      ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3);
  ctx.set_verify_mode(ssl::verify_peer);
  // TLS 1.2 minimum (design §8.4).
#if defined(SSL_CTX_set_min_proto_version)
  ::SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION);
#endif
  boost::system::error_code ec;
  ctx.load_verify_file(ca_path, ec);
  if (ec) {
    throw boost::system::system_error(ec, "load_verify_file");
  }
  return ctx;
}

template <class Stream>
asio::awaitable<Result<ClientResponse>> do_http_exchange(
    Stream& stream,
    const ParsedUrl& url,
    ClientRequest req) {
  const auto verb = parse_verb(req.method);
  if (verb == http::verb::unknown) {
    co_return std::unexpected(Error{std::format("http.request: unknown method '{}'", req.method)});
  }

  http::request<http::string_body> http_req{verb, url.target, 11};
  http_req.set(http::field::host, host_header_value(url));
  http_req.set(http::field::user_agent, "vacps-native");
  http_req.set(http::field::connection, "close");
  for (const auto& [k, v] : req.headers) {
    http_req.set(k, v);
  }
  if (!req.body.empty()) {
    http_req.body() = req.body;
    http_req.prepare_payload();
  }

  auto [wec, wn] =
      co_await http::async_write(stream, http_req, asio::as_tuple(asio::use_awaitable));
  (void)wn;
  if (wec) {
    co_return std::unexpected(Error{std::format("http.request write: {}", wec.message())});
  }

  // body_limit applies during async_read — not after a full unbounded read.
  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  parser.body_limit(req.max_response_bytes);
  auto [rec, rn] =
      co_await http::async_read(stream, buffer, parser, asio::as_tuple(asio::use_awaitable));
  (void)rn;
  if (rec == http::error::body_limit) {
    co_return std::unexpected(Error{std::format(
        "http.request: response body exceeds maxResponseBytes {}",
        req.max_response_bytes)});
  }
  if (rec) {
    co_return std::unexpected(Error{std::format("http.request read: {}", rec.message())});
  }

  http::response<http::string_body> res = parser.release();
  ClientResponse out;
  out.status = static_cast<int>(res.result_int());
  out.body = std::move(res.body());
  for (auto it = res.begin(); it != res.end(); ++it) {
    out.headers.emplace_back(std::string{it->name_string()}, std::string{it->value()});
  }
  co_return out;
}

}  // namespace

Result<ParsedUrl> parse_url(std::string_view url) {
  if (url.empty()) {
    return std::unexpected(Error{"http.request: empty url"});
  }

  // WHATWG parse via Ada (same engine as JS global URL).
  auto parsed = ada::parse<ada::url_aggregator>(url);
  if (!parsed) {
    return std::unexpected(Error{"http.request: invalid url"});
  }

  // protocol is "https:" / "http:" (with trailing colon).
  auto protocol = parsed->get_protocol();
  std::string scheme;
  scheme.reserve(protocol.size());
  for (char c : protocol) {
    if (c == ':') break;
    scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (scheme != "http" && scheme != "https") {
    return std::unexpected(Error{"http.request: only http and https schemes are supported"});
  }

  // Reject credentials (agent outbound requests must not embed userinfo).
  if (parsed->has_credentials()) {
    return std::unexpected(Error{"http.request: userinfo in url is not supported"});
  }

  ParsedUrl out;
  out.scheme = std::move(scheme);
  out.host = std::string{parsed->get_hostname()};
  if (out.host.empty()) {
    return std::unexpected(Error{"http.request: missing host"});
  }

  auto port_sv = parsed->get_port();
  if (port_sv.empty()) {
    out.port = (out.scheme == "https") ? "443" : "80";
  } else {
    auto pr = vacps::parse_port(port_sv);
    if (!pr) {
      return std::unexpected(Error{"http.request: invalid port"});
    }
    out.port = std::to_string(*pr);
  }

  // path + query only (fragment must not be sent).
  auto path = parsed->get_pathname();
  auto search = parsed->get_search();  // includes leading '?' when present
  out.target = path.empty() ? std::string{"/"} : std::string{path};
  out.target.append(search);
  if (out.target.empty()) {
    out.target = "/";
  }
  return out;
}

Result<std::string> resolve_ca_bundle(std::string_view explicit_path) {
  namespace fs = std::filesystem;
  auto try_path = [](std::string_view p) -> std::optional<std::string> {
    if (p.empty()) return std::nullopt;
    std::error_code ec;
    if (fs::is_regular_file(fs::path{std::string{p}}, ec)) {
      return std::string{p};
    }
    return std::nullopt;
  };

  if (auto p = try_path(explicit_path)) {
    return *p;
  }
  if (const char* env = std::getenv("VACPS_CA_BUNDLE"); env != nullptr && env[0] != '\0') {
    if (auto p = try_path(env)) {
      return *p;
    }
    return std::unexpected(
        Error{std::format("http.request: VACPS_CA_BUNDLE not a file: {}", env)});
  }
  static constexpr const char* kDefaults[] = {
      "/etc/vacps/ca-bundle.pem",
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/ssl/cert.pem",
      "/etc/pki/tls/certs/ca-bundle.crt",
  };
  for (const char* d : kDefaults) {
    if (auto p = try_path(d)) {
      return *p;
    }
  }
  return std::unexpected(Error{
      "http.request: no CA bundle found (set VACPS_CA_BUNDLE or install ca-certificates)"});
}

asio::awaitable<Result<ClientResponse>> async_request(ClientRequest req) {
  if (req.url.empty()) {
    co_return std::unexpected(Error{"http.request: url required"});
  }
  if (req.timeout_ms < 0) {
    co_return std::unexpected(Error{"http.request: timeoutMs must be >= 0"});
  }
  if (req.max_response_bytes == 0) {
    co_return std::unexpected(Error{"http.request: maxResponseBytes must be > 0"});
  }

  auto parsed = parse_url(req.url);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed.error()));
  }
  const ParsedUrl url = std::move(*parsed);

  try {
    auto executor = co_await asio::this_coro::executor;
    const auto timeout = std::chrono::milliseconds(
        req.timeout_ms > 0 ? req.timeout_ms : 30'000);

    // DNS resolve is covered by the same wall budget (previously uncancellable).
    tcp::resolver resolver{executor};
    auto [rec, results] = co_await resolver.async_resolve(
        url.host,
        url.port,
        asio::as_tuple(asio::cancel_after(timeout, asio::use_awaitable)));
    if (rec == asio::error::operation_aborted) {
      co_return std::unexpected(Error{"http.request resolve: timed out"});
    }
    if (rec) {
      co_return std::unexpected(Error{std::format("http.request resolve: {}", rec.message())});
    }

    if (url.scheme == "http") {
      beast::tcp_stream stream{executor};
      stream.expires_after(timeout);
      auto [cec, ep] =
          co_await stream.async_connect(results, asio::as_tuple(asio::use_awaitable));
      (void)ep;
      if (cec) {
        co_return std::unexpected(Error{std::format("http.request connect: {}", cec.message())});
      }
      stream.expires_after(timeout);
      auto out = co_await do_http_exchange(stream, url, std::move(req));
      beast::error_code sec;
      stream.socket().shutdown(tcp::socket::shutdown_both, sec);
      co_return out;
    }

    // HTTPS
    auto ca = resolve_ca_bundle(req.ca_bundle);
    if (!ca) {
      co_return std::unexpected(std::move(ca.error()));
    }
    ssl::context ctx = make_tls_client_context(*ca);

    ssl::stream<beast::tcp_stream> stream{executor, ctx};
    // SNI
    if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
      co_return std::unexpected(Error{"http.request: SSL_set_tlsext_host_name failed"});
    }
    stream.set_verify_callback(ssl::host_name_verification(url.host));

    beast::get_lowest_layer(stream).expires_after(timeout);
    auto [cec, ep] = co_await beast::get_lowest_layer(stream).async_connect(
        results, asio::as_tuple(asio::use_awaitable));
    (void)ep;
    if (cec) {
      co_return std::unexpected(Error{std::format("http.request connect: {}", cec.message())});
    }

    beast::get_lowest_layer(stream).expires_after(timeout);
    auto [hec] =
        co_await stream.async_handshake(ssl::stream_base::client, asio::as_tuple(asio::use_awaitable));
    if (hec) {
      co_return std::unexpected(Error{std::format("http.request tls: {}", hec.message())});
    }

    beast::get_lowest_layer(stream).expires_after(timeout);
    auto out = co_await do_http_exchange(stream, url, std::move(req));

    beast::get_lowest_layer(stream).expires_after(timeout);
    auto [sec] = co_await stream.async_shutdown(asio::as_tuple(asio::use_awaitable));
    if (sec && sec != ssl::error::stream_truncated) {
      log::debug("http.request shutdown: {}", sec.message());
    }
    co_return out;
  } catch (const boost::system::system_error& e) {
    co_return std::unexpected(Error{std::format("http.request: {}", e.what())});
  } catch (const std::exception& e) {
    co_return std::unexpected(Error{std::format("http.request: {}", e.what())});
  }
}

}  // namespace vacps::http
