#include "http/session.hpp"

#include "http/server.hpp"

namespace vacps::http {

Session::Session(
    tcp::socket socket,
    std::shared_ptr<vacps::js::Host> script,
    std::weak_ptr<Server> server)
    : stream_(std::move(socket)),
      script_(std::move(script)),
      server_(std::move(server)) {}

void Session::notify_finished() noexcept {
  if (finished_) return;
  finished_ = true;
  if (auto srv = server_.lock()) {
    srv->session_finished();
  }
}

void Session::cancel() noexcept {
  beast::error_code ec;
  stream_.cancel();
  stream_.socket().close(ec);
}

void Session::run() {
  auto self = shared_from_this();
  if (auto srv = server_.lock()) {
    srv->session_started(self);
  }
  asio::co_spawn(
      stream_.get_executor(),
      [self]() -> asio::awaitable<void> {
        try {
          co_await self->do_session();
        } catch (...) {
          self->notify_finished();
          throw;
        }
        self->notify_finished();
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

asio::awaitable<void> Session::do_session() {
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

asio::awaitable<http::response<http::string_body>> Session::dispatch(
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

}  // namespace vacps::http
