#pragma once

#include "http/routes.hpp"
#include "http/script_dispatch.hpp"
#include "app/log.hpp"
#include "quickjs/host.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace vacps::http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

/** Per-operation idle timeout for read/write. */
inline constexpr std::chrono::seconds kSessionIoTimeout{30};
/** Max HTTP body size (bytes). */
inline constexpr std::uint64_t kMaxBodyBytes = 1 * 1024 * 1024;

inline bool is_benign_disconnect(const beast::error_code& ec) {
  return ec == http::error::end_of_stream || ec == asio::error::eof ||
         ec == asio::error::connection_reset || ec == asio::error::broken_pipe ||
         ec == beast::error::timeout || ec == http::error::body_limit;
}

/** TCP session: pure transport; dispatches to script via http::dispatch_to_script. */
class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(tcp::socket socket, std::shared_ptr<vacps::js::Host> script)
      : stream_(std::move(socket)), script_(std::move(script)) {}

  void run() {
    asio::co_spawn(
        stream_.get_executor(),
        [self = shared_from_this()]() -> asio::awaitable<void> {
          co_await self->do_session();
        },
        [](std::exception_ptr ep) {
          if (!ep) return;
          try {
            std::rethrow_exception(ep);
          } catch (const boost::system::system_error& e) {
            if (!is_benign_disconnect(e.code())) {
              vacps::log::error("session error: {}", e.what());
            }
          } catch (const std::exception& e) {
            vacps::log::error("session error: {}", e.what());
          } catch (...) {
            vacps::log::error("session error: unknown");
          }
        });
  }

 private:
  asio::awaitable<void> do_session() {
    beast::flat_buffer buffer;
    for (;;) {
      http::request_parser<http::string_body> parser;
      parser.body_limit(kMaxBodyBytes);
      stream_.expires_after(kSessionIoTimeout);
      auto [rec, nbytes] = co_await http::async_read(
          stream_, buffer, parser, asio::as_tuple(asio::use_awaitable));
      (void)nbytes;
      if (rec == http::error::body_limit) {
        http::response<http::string_body> res{http::status::payload_too_large, 11};
        res.set(http::field::server, "vacps-agent");
        res.set(http::field::content_type, "application/json; charset=utf-8");
        res.keep_alive(false);
        res.body() = R"({"error":{"code":"payload_too_large","message":"body exceeds 1MiB"}})";
        res.prepare_payload();
        stream_.expires_after(kSessionIoTimeout);
        co_await http::async_write(stream_, res, asio::as_tuple(asio::use_awaitable));
        break;
      }
      if (is_benign_disconnect(rec)) break;
      if (rec) throw boost::system::system_error(rec);

      auto req = parser.release();
      auto res = co_await dispatch(std::move(req));
      const bool close = res.need_eof();
      stream_.expires_after(kSessionIoTimeout);
      auto [wec, wn] =
          co_await http::async_write(stream_, res, asio::as_tuple(asio::use_awaitable));
      (void)wn;
      if (is_benign_disconnect(wec)) break;
      if (wec) throw boost::system::system_error(wec);
      if (close) break;
    }
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
  }

  asio::awaitable<http::response<http::string_body>> dispatch(
      http::request<http::string_body> req) {
    if (!script_ || !script_->script_ready()) {
      co_return make_response(
          req,
          http::status::service_unavailable,
          bootstrap_unavailable_json("business script not ready"));
    }

    const std::string_view target = req.target();
    const auto qpos = target.find('?');
    const auto path = target.substr(0, qpos);
    const auto query =
        (qpos == std::string_view::npos) ? std::string_view{} : target.substr(qpos + 1);

    ScriptHttpRequest dto;
    dto.method = std::string(req.method_string());
    dto.path = std::string(path);
    dto.query = std::string(query);
    dto.body = req.body();
    dto.request_id = std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    for (const auto& h : req) {
      dto.headers.emplace_back(std::string(h.name_string()), std::string(h.value()));
    }

    auto jr = co_await dispatch_to_script(*script_, std::move(dto));
    if (!jr) {
      vacps::log::error("handleRequest failed: {}", jr.error().message);
      co_return make_response(
          req,
          http::status::internal_server_error,
          internal_error_json("script handleRequest failed"));
    }

    if (jr->status < 100 || jr->status > 599) {
      co_return make_response(
          req,
          http::status::internal_server_error,
          internal_error_json("script returned invalid HTTP status"));
    }

    http::response<http::string_body> res{
        static_cast<http::status>(jr->status), req.version()};
    res.set(http::field::server, "vacps-agent");
    res.keep_alive(req.keep_alive());
    for (const auto& [k, v] : jr->headers) {
      res.set(k, v);
    }
    res.body() = std::move(jr->body);
    res.prepare_payload();
    co_return res;
  }

  beast::tcp_stream stream_;
  std::shared_ptr<vacps::js::Host> script_;
};

}  // namespace vacps::http
