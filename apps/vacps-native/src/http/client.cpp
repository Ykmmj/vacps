#include "http/client.hpp"

#include "app/log.hpp"

#include <ada.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace vacps::http {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using steady_clock = std::chrono::steady_clock;

/** Operation-local cancel bridge: stop_token → post(executor) → signal. */
struct OpCancelState {
  asio::cancellation_signal signal;
  bool active{true};
};

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

bool is_forbidden_header(std::string_view name) {
  return ieq(name, "host") || ieq(name, "content-length") ||
         ieq(name, "transfer-encoding");
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
  // Fail closed on ancient protocols; min proto 1.2 is also set below.
  ctx.set_options(
      ssl::context::default_workarounds | ssl::context::no_sslv2 |
      ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
  ctx.set_verify_mode(ssl::verify_peer);
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

[[nodiscard]] bool deadline_expired(steady_clock::time_point deadline) {
  return steady_clock::now() >= deadline;
}

/**
 * Classify aborted/timeout errors; otherwise preserve a useful system code.
 * operation_aborted + stop → ECANCELED; else deadline/beast timeout → ETIMEDOUT.
 */
Error make_phase_error(
    std::string_view operation,
    const boost::system::error_code& ec,
    const std::stop_token& stop,
    steady_clock::time_point deadline) {
  const bool aborted = ec == asio::error::operation_aborted;
  const bool beast_timeout = ec == beast::error::timeout;
  if (aborted || beast_timeout) {
    if (stop.stop_requested()) {
      return Error{
          std::format("{}: cancelled", operation),
          std::string{operation},
          ECANCELED};
    }
    if (beast_timeout || deadline_expired(deadline)) {
      return Error{
          std::format("{}: timed out", operation),
          std::string{operation},
          ETIMEDOUT};
    }
    // Aborted without stop or deadline (e.g. runtime/shutdown cancel path).
    return Error{
        std::format("{}: cancelled", operation),
        std::string{operation},
        ECANCELED};
  }

  int code = 0;
  if (ec) {
    // Prefer errno-compatible values when the category is the system category.
    if (ec.category() == boost::system::system_category() ||
        ec.category() == asio::error::get_system_category()) {
      code = ec.value();
    } else if (ec.value() != 0) {
      code = ec.value();
    }
  }
  return Error{
      std::format("{}: {}", operation, ec.message()),
      std::string{operation},
      code};
}

Error make_precheck_error(
    std::string_view operation,
    std::string message,
    int system_code,
    const std::stop_token& stop,
    steady_clock::time_point deadline) {
  if (stop.stop_requested()) {
    return Error{
        std::format("{}: cancelled", operation),
        std::string{operation},
        ECANCELED};
  }
  if (deadline_expired(deadline)) {
    return Error{
        std::format("{}: timed out", operation),
        std::string{operation},
        ETIMEDOUT};
  }
  return Error{std::move(message), std::string{operation}, system_code};
}

/**
 * Absolute cancel_at(deadline) composed with an external cancellation slot.
 * timed_cancel_op installs a proxy on `slot` so stop-token emits and the
 * deadline timer share one child signal for the underlying op.
 */
auto make_io_token(
    asio::cancellation_slot slot,
    steady_clock::time_point deadline) {
  return asio::cancel_at(
      deadline,
      asio::cancellation_type::all)(
      asio::bind_cancellation_slot(
          slot, asio::as_tuple(asio::use_awaitable)));
}

template <class Stream>
asio::awaitable<Result<ClientResponse>> do_http_exchange(
    Stream& stream,
    const ParsedUrl& url,
    ClientRequest req,
    std::shared_ptr<OpCancelState> op,
    const std::stop_token& stop,
    steady_clock::time_point deadline) {
  constexpr std::string_view kWriteOp = "http.request.write";
  constexpr std::string_view kReadOp = "http.request.read";

  if (stop.stop_requested() || deadline_expired(deadline)) {
    co_return std::unexpected(
        make_precheck_error(kWriteOp, "", 0, stop, deadline));
  }

  const auto verb = parse_verb(req.method);
  // Method/header policy is validated in async_request_impl before I/O.

  http::request<http::vector_body<std::uint8_t>> http_req{verb, url.target, 11};
  http_req.set(http::field::host, host_header_value(url));
  http_req.set(http::field::user_agent, "vacps-native");
  http_req.set(http::field::connection, "close");
  for (const auto& [k, v] : req.headers) {
    http_req.set(k, v);
  }
  if (!req.body.empty()) {
    http_req.body() = std::move(req.body);
    http_req.prepare_payload();
  }

  {
    auto [wec, wn] = co_await http::async_write(
        stream,
        http_req,
        make_io_token(op->signal.slot(), deadline));
    (void)wn;
    if (wec) {
      co_return std::unexpected(make_phase_error(kWriteOp, wec, stop, deadline));
    }
  }

  if (stop.stop_requested() || deadline_expired(deadline)) {
    co_return std::unexpected(
        make_precheck_error(kReadOp, "", 0, stop, deadline));
  }

  // body_limit applies during async_read — not after a full unbounded read.
  beast::flat_buffer buffer;
  http::response_parser<http::vector_body<std::uint8_t>> parser;
  parser.body_limit(req.max_response_bytes);
  auto [rec, rn] = co_await http::async_read(
      stream,
      buffer,
      parser,
      make_io_token(op->signal.slot(), deadline));
  (void)rn;
  if (rec == http::error::body_limit) {
    co_return std::unexpected(Error{
        std::format(
            "http.request.read: response body exceeds maxResponseBytes {}",
            req.max_response_bytes),
        std::string{kReadOp},
        0});
  }
  if (rec) {
    co_return std::unexpected(make_phase_error(kReadOp, rec, stop, deadline));
  }

  http::response<http::vector_body<std::uint8_t>> res = parser.release();
  ClientResponse out;
  out.status = static_cast<int>(res.result_int());
  out.body = std::move(res.body());
  for (auto it = res.begin(); it != res.end(); ++it) {
    out.headers.emplace_back(std::string{it->name_string()}, std::string{it->value()});
  }
  co_return out;
}

/**
 * Named coroutine body: values are owned across initial suspend.
 * stop_callback must post onto the request executor before emit.
 */
asio::awaitable<Result<ClientResponse>> async_request_impl(
    std::stop_token stop,
    ClientRequest req) {
  constexpr std::string_view kResolveOp = "http.request.resolve";
  constexpr std::string_view kConnectOp = "http.request.connect";
  constexpr std::string_view kTlsOp = "http.request.tls";

  if (req.url.empty()) {
    co_return std::unexpected(Error{"http.request: url required", "http.request", 0});
  }
  if (req.timeout <= std::chrono::milliseconds::zero()) {
    co_return std::unexpected(Error{
        "http.request: timeout must be > 0",
        "http.request",
        0});
  }
  if (req.max_response_bytes == 0) {
    co_return std::unexpected(Error{
        "http.request: maxResponseBytes must be > 0",
        "http.request",
        0});
  }
  if (parse_verb(req.method) == http::verb::unknown) {
    co_return std::unexpected(Error{
        std::format("http.request: unknown method '{}'", req.method),
        "http.request",
        0});
  }
  for (const auto& [name, value] : req.headers) {
    (void)value;
    if (is_forbidden_header(name)) {
      co_return std::unexpected(Error{
          std::format(
              "http.request: header '{}' is reserved (owned by the client)",
              name),
          "http.request",
          0});
    }
  }

  // One absolute wall deadline for resolve / connect / tls / write / read / shutdown.
  const auto deadline = steady_clock::now() + req.timeout;

  if (stop.stop_requested()) {
    co_return std::unexpected(Error{
        "http.request: cancelled",
        "http.request",
        ECANCELED});
  }
  if (deadline_expired(deadline)) {
    co_return std::unexpected(Error{
        "http.request: timed out",
        "http.request",
        ETIMEDOUT});
  }

  auto parsed = parse_url(req.url);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed.error()));
  }
  const ParsedUrl url = std::move(*parsed);

  // HTTPS CA is resolved before any network I/O (fail-closed, no getenv).
  std::optional<std::string> ca_path;
  if (url.scheme == "https") {
    auto ca = resolve_ca_bundle(req.ca_bundle);
    if (!ca) {
      co_return std::unexpected(std::move(ca.error()));
    }
    ca_path = std::move(*ca);
  }

  try {
    auto executor = co_await asio::this_coro::executor;

    // Shared so a stop_callback post that outlives this frame is harmless.
    auto op = std::make_shared<OpCancelState>();
    auto weak_op = std::weak_ptr<OpCancelState>(op);
    // stop_callback may run on a foreign thread: only post to this executor.
    // If stop is already requested, the ctor invokes the callback synchronously
    // (still only posts; emit runs on the executor once a slot is bound).
    std::stop_callback on_stop{
        stop, [weak_op, executor]() noexcept {
          try {
            asio::post(executor, [weak_op]() {
              if (auto s = weak_op.lock(); s && s->active) {
                s->signal.emit(asio::cancellation_type::all);
              }
            });
          } catch (...) {
          }
        }};

    tcp::resolver resolver{executor};
    auto [rec, results] = co_await resolver.async_resolve(
        url.host,
        url.port,
        make_io_token(op->signal.slot(), deadline));
    if (rec) {
      op->active = false;
      co_return std::unexpected(make_phase_error(kResolveOp, rec, stop, deadline));
    }

    if (stop.stop_requested() || deadline_expired(deadline)) {
      op->active = false;
      co_return std::unexpected(
          make_precheck_error(kConnectOp, "", 0, stop, deadline));
    }

    if (url.scheme == "http") {
      beast::tcp_stream stream{executor};
      auto [cec, ep] = co_await stream.async_connect(
          results, make_io_token(op->signal.slot(), deadline));
      (void)ep;
      if (cec) {
        op->active = false;
        co_return std::unexpected(make_phase_error(kConnectOp, cec, stop, deadline));
      }

      auto out = co_await do_http_exchange(
          stream, url, std::move(req), op, stop, deadline);

      // Best-effort shutdown under the same wall deadline.
      if (op->active && !deadline_expired(deadline) && !stop.stop_requested()) {
        beast::error_code sec;
        stream.socket().shutdown(tcp::socket::shutdown_both, sec);
      }
      op->active = false;
      co_return out;
    }

    ssl::context ctx = make_tls_client_context(*ca_path);

    ssl::stream<beast::tcp_stream> stream{executor, ctx};
    if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
      op->active = false;
      co_return std::unexpected(Error{
          "http.request.tls: SSL_set_tlsext_host_name failed",
          std::string{kTlsOp},
          0});
    }
    stream.set_verify_callback(ssl::host_name_verification(url.host));

    auto [cec, ep] = co_await beast::get_lowest_layer(stream).async_connect(
        results, make_io_token(op->signal.slot(), deadline));
    (void)ep;
    if (cec) {
      op->active = false;
      co_return std::unexpected(make_phase_error(kConnectOp, cec, stop, deadline));
    }

    if (stop.stop_requested() || deadline_expired(deadline)) {
      op->active = false;
      co_return std::unexpected(
          make_precheck_error(kTlsOp, "", 0, stop, deadline));
    }

    auto [hec] = co_await stream.async_handshake(
        ssl::stream_base::client,
        make_io_token(op->signal.slot(), deadline));
    if (hec) {
      op->active = false;
      co_return std::unexpected(make_phase_error(kTlsOp, hec, stop, deadline));
    }

    auto out = co_await do_http_exchange(
        stream, url, std::move(req), op, stop, deadline);

    if (op->active && !deadline_expired(deadline) && !stop.stop_requested()) {
      auto [sec] = co_await stream.async_shutdown(
          make_io_token(op->signal.slot(), deadline));
      if (sec && sec != ssl::error::stream_truncated) {
        log::debug("http.request shutdown: {}", sec.message());
      }
    }
    op->active = false;
    co_return out;
  } catch (const boost::system::system_error& e) {
    const auto& ec = e.code();
    int code = 0;
    if (ec) {
      if (ec.category() == boost::system::system_category() ||
          ec.category() == asio::error::get_system_category()) {
        code = ec.value();
      } else if (ec.value() != 0) {
        code = ec.value();
      }
    }
    co_return std::unexpected(Error{
        std::format("http.request: {}", e.what()),
        "http.request",
        code});
  } catch (const std::exception& e) {
    co_return std::unexpected(Error{
        std::format("http.request: {}", e.what()),
        "http.request",
        0});
  }
}

}  // namespace

Result<ParsedUrl> parse_url(std::string_view url) {
  if (url.empty()) {
    return std::unexpected(Error{"http.request: empty url", "http.request", 0});
  }

  // WHATWG parse via Ada (same engine as JS global URL).
  auto parsed = ada::parse<ada::url_aggregator>(url);
  if (!parsed) {
    return std::unexpected(Error{"http.request: invalid url", "http.request", 0});
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
    return std::unexpected(Error{
        "http.request: only http and https schemes are supported",
        "http.request",
        0});
  }

  // Reject credentials (agent outbound requests must not embed userinfo).
  if (parsed->has_credentials()) {
    return std::unexpected(Error{
        "http.request: userinfo in url is not supported",
        "http.request",
        0});
  }

  ParsedUrl out;
  out.scheme = std::move(scheme);
  out.host = std::string{parsed->get_hostname()};
  if (out.host.empty()) {
    return std::unexpected(Error{"http.request: missing host", "http.request", 0});
  }

  auto port_sv = parsed->get_port();
  if (port_sv.empty()) {
    out.port = (out.scheme == "https") ? "443" : "80";
  } else {
    unsigned long n = 0;
    const char* begin = port_sv.data();
    const char* end = port_sv.data() + port_sv.size();
    auto [ptr, ec] = std::from_chars(begin, end, n);
    if (ec != std::errc{} || ptr != end || n == 0 || n > 65535) {
      return std::unexpected(Error{"http.request: invalid port", "http.request", 0});
    }
    out.port = std::to_string(n);
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

  // explicit_path is injected from bootstrap/host ca_bundle config only.
  // Do not call getenv here — post-bootstrap product code uses injected config.
  if (!explicit_path.empty()) {
    if (auto p = try_path(explicit_path)) {
      return *p;
    }
    return std::unexpected(Error{
        std::format("http.request: CA bundle not a file: {}", explicit_path),
        "http.request.tls",
        0});
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
      "http.request: no CA bundle found (inject ca_bundle or install ca-certificates)",
      "http.request.tls",
      0});
}

asio::awaitable<Result<ClientResponse>> async_request(
    std::stop_token stop,
    ClientRequest req) {
  return async_request_impl(std::move(stop), std::move(req));
}

}  // namespace vacps::http
