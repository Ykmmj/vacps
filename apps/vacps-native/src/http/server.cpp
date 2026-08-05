#include "http/server.hpp"

#include "app/log.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::http {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::string_view kOpListen = "http.server.listen";
constexpr std::string_view kOpAccept = "http.server.accept";
constexpr std::string_view kOpRead = "http.server.read";
constexpr std::string_view kOpHandler = "http.server.handler";
constexpr std::string_view kOpWrite = "http.server.write";
constexpr std::string_view kOpClose = "http.server.close";

constexpr int kDefaultBacklog = 128;

enum class Phase : std::uint8_t {
  Created = 0,
  Listening,
  Closing,
  Closed,
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

/** Hop-by-hop / framing headers owned by the transport (case-insensitive). */
bool is_transport_owned_header(std::string_view name) {
  return ieq(name, "connection") || ieq(name, "content-length") ||
         ieq(name, "transfer-encoding") || ieq(name, "upgrade") ||
         ieq(name, "trailer") || ieq(name, "te") || ieq(name, "keep-alive") ||
         ieq(name, "proxy-connection");
}

/** RFC 7230 token (tchar) for header field names. */
bool is_valid_header_name(std::string_view name) {
  if (name.empty()) return false;
  for (unsigned char c : name) {
    const bool ok =
        (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '!' || c == '#' || c == '$' ||
        c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
        c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
        c == '|' || c == '~';
    if (!ok) return false;
  }
  return true;
}

/**
 * Field value: no NUL/CR/LF; allow HTAB and VCHAR / obs-text (0x20-0x7E, 0x80-0xFF).
 */
bool is_valid_header_value(std::string_view value) {
  for (unsigned char c : value) {
    if (c == '\0' || c == '\r' || c == '\n') return false;
    if (c < 0x20 && c != '\t') return false;
  }
  return true;
}

bool is_benign_disconnect(const boost::system::error_code& ec) {
  return ec == http::error::end_of_stream || ec == asio::error::eof ||
         ec == asio::error::connection_reset || ec == asio::error::broken_pipe ||
         ec == asio::error::connection_aborted;
}

bool is_malformed_request(const boost::system::error_code& ec) {
  return ec == http::error::header_limit || ec == http::error::bad_method ||
         ec == http::error::bad_target || ec == http::error::bad_version ||
         ec == http::error::bad_status || ec == http::error::bad_reason ||
         ec == http::error::bad_field || ec == http::error::bad_value ||
         ec == http::error::bad_content_length ||
         ec == http::error::bad_transfer_encoding ||
         ec == http::error::bad_chunk ||
         ec == http::error::bad_chunk_extension ||
         ec == http::error::bad_obs_fold ||
         ec == http::error::bad_line_ending ||
         ec == http::error::partial_message;
}

int system_code_of(const boost::system::error_code& ec) {
  if (!ec) return 0;
  if (ec.category() == boost::system::system_category() ||
      ec.category() == asio::error::get_system_category()) {
    return ec.value();
  }
  return ec.value();
}

Error make_io_error(
    std::string_view operation,
    const boost::system::error_code& ec,
    bool stop_requested,
    bool timed_out_hint) {
  const bool aborted = ec == asio::error::operation_aborted;
  const bool beast_timeout = ec == beast::error::timeout;
  if (aborted || beast_timeout) {
    if (stop_requested && !timed_out_hint) {
      return Error{
          std::format("{}: cancelled", operation),
          std::string{operation},
          ECANCELED};
    }
    if (beast_timeout || timed_out_hint) {
      return Error{
          std::format("{}: timed out", operation),
          std::string{operation},
          ETIMEDOUT};
    }
    return Error{
        std::format("{}: cancelled", operation),
        std::string{operation},
        ECANCELED};
  }
  return Error{
      std::format("{}: {}", operation, ec.message()),
      std::string{operation},
      system_code_of(ec)};
}

std::optional<Error> validate_options(const ServerOptions& opts) {
  if (opts.host.empty()) {
    return Error{"http.server.listen: host required", std::string{kOpListen}, 0};
  }
  if (opts.max_request_bytes == 0) {
    return Error{
        "http.server.listen: max_request_bytes must be > 0",
        std::string{kOpListen},
        0};
  }
  if (opts.max_header_bytes == 0) {
    return Error{
        "http.server.listen: max_header_bytes must be > 0",
        std::string{kOpListen},
        0};
  }
  if (opts.max_response_bytes == 0) {
    return Error{
        "http.server.listen: max_response_bytes must be > 0",
        std::string{kOpListen},
        0};
  }
  if (opts.io_timeout <= std::chrono::milliseconds::zero()) {
    return Error{
        "http.server.listen: io_timeout must be > 0",
        std::string{kOpListen},
        0};
  }
  if (opts.handler_timeout <= std::chrono::milliseconds::zero()) {
    return Error{
        "http.server.listen: handler_timeout must be > 0",
        std::string{kOpListen},
        0};
  }
  return std::nullopt;
}

std::optional<Error> validate_response(
    const ServerResponse& res,
    std::size_t max_body) {
  // Single final response only — no 1xx interim / upgrade model.
  if (res.status < 200 || res.status > 599) {
    return Error{
        std::format("http.server.handler: invalid status {}", res.status),
        std::string{kOpHandler},
        0};
  }
  // RFC 9110: 204/205/304 must not carry a content body.
  if ((res.status == 204 || res.status == 205 || res.status == 304) &&
      !res.body.empty()) {
    return Error{
        std::format(
            "http.server.handler: status {} must not include a body",
            res.status),
        std::string{kOpHandler},
        0};
  }
  if (res.body.size() > max_body) {
    return Error{
        std::format(
            "http.server.handler: response body exceeds max_response_bytes {}",
            max_body),
        std::string{kOpHandler},
        0};
  }
  for (const auto& [name, value] : res.headers) {
    if (is_transport_owned_header(name)) {
      return Error{
          std::format(
              "http.server.handler: transport-owned header '{}' must not be "
              "set by the handler",
              name),
          std::string{kOpHandler},
          0};
    }
    if (!is_valid_header_name(name)) {
      return Error{
          std::format("http.server.handler: invalid header name '{}'", name),
          std::string{kOpHandler},
          0};
    }
    if (!is_valid_header_value(value)) {
      return Error{
          "http.server.handler: invalid header value",
          std::string{kOpHandler},
          0};
    }
  }
  return std::nullopt;
}

std::uint32_t clamp_header_limit(std::size_t n) {
  constexpr std::size_t kMax =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(std::min(n, kMax));
}

auto make_io_token(
    asio::cancellation_slot slot,
    std::chrono::milliseconds timeout) {
  return asio::cancel_after(
      timeout,
      asio::cancellation_type::all)(
      asio::bind_cancellation_slot(
          slot, asio::as_tuple));
}

auto make_cancel_token(asio::cancellation_slot slot) {
  return asio::bind_cancellation_slot(
      slot, asio::as_tuple);
}

std::vector<std::uint8_t> fixed_status_body(int status) {
  std::string_view text;
  switch (status) {
    case 400:
      text = "Bad Request";
      break;
    case 413:
      text = "Payload Too Large";
      break;
    case 500:
      text = "Internal Server Error";
      break;
    case 504:
      text = "Gateway Timeout";
      break;
    default:
      text = "Error";
      break;
  }
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

ServerResponse fixed_error_response(int status) {
  ServerResponse r;
  r.status = status;
  r.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
  r.body = fixed_status_body(status);
  return r;
}

template <class Body>
void apply_response_headers(
    http::response<Body>& res,
    const ServerResponse& src) {
  // insert (not set) preserves duplicate names from the handler DTO.
  for (const auto& [k, v] : src.headers) {
    res.insert(k, v);
  }
}

}  // namespace

struct Server::State : std::enable_shared_from_this<Server::State> {
  asio::any_io_executor ex;
  ServerOptions opts;
  ServerHandler handler;

  Phase phase{Phase::Created};
  std::optional<tcp::acceptor> acceptor;
  ListenAddress bound{};
  asio::cancellation_signal accept_cancel;
  std::stop_source stop_source;

  /**
   * Live children: accept loop (1 while running) + each session.
   * Close finalizes exactly once when phase==Closing && child_count==0.
   */
  std::size_t child_count{0};
  std::vector<std::shared_ptr<asio::steady_timer>> drain_waiters;
  std::vector<std::shared_ptr<asio::steady_timer>> close_waiters;
  std::vector<std::weak_ptr<Session>> sessions;

  bool accept_loop_running{false};

  State(
      asio::any_io_executor executor,
      ServerOptions options,
      ServerHandler h)
      : ex(std::move(executor)),
        opts(std::move(options)),
        handler(std::move(h)) {}

  void child_inc() noexcept { ++child_count; }

  void child_dec() noexcept {
    if (child_count == 0) return;
    --child_count;
    if (child_count == 0) {
      wake_drain_waiters();
      // State invariant: Closing + zero children → finalize Closed exactly once.
      try_finalize_close();
    }
  }

  void wake_drain_waiters() noexcept {
    try {
      auto waiters = std::move(drain_waiters);
      drain_waiters.clear();
      for (auto& t : waiters) {
        if (!t) continue;
        t->cancel();
      }
    } catch (...) {
    }
  }

  void wake_close_waiters() noexcept {
    try {
      auto waiters = std::move(close_waiters);
      close_waiters.clear();
      for (auto& t : waiters) {
        if (!t) continue;
        t->cancel();
      }
    } catch (...) {
    }
  }

  /**
   * Finalize Closing → Closed exactly once when no children remain.
   * Shared by async_close and dispose (via child_dec / begin_close).
   */
  void try_finalize_close() noexcept {
    if (phase != Phase::Closing) return;
    if (child_count != 0) return;

    try {
      if (acceptor) {
        boost::system::error_code ec;
        acceptor->cancel(ec);
        acceptor->close(ec);
        acceptor.reset();
      }
      bound = {};
    } catch (...) {
      // Still mark Closed so waiters cannot hang if socket teardown throws.
      try {
        acceptor.reset();
      } catch (...) {
      }
      bound = {};
    }
    phase = Phase::Closed;
    wake_drain_waiters();
    wake_close_waiters();
  }

  void track_session(const std::shared_ptr<Session>& s) {
    sessions.erase(
        std::remove_if(
            sessions.begin(),
            sessions.end(),
            [](const std::weak_ptr<Session>& w) { return w.expired(); }),
        sessions.end());
    sessions.push_back(s);
  }

  void request_shutdown() noexcept {
    // stop_source APIs are noexcept.
    if (stop_source.stop_possible() && !stop_source.stop_requested()) {
      stop_source.request_stop();
    }
    try {
      accept_cancel.emit(asio::cancellation_type::all);
    } catch (...) {
    }
    try {
      if (acceptor) {
        boost::system::error_code ec;
        acceptor->cancel(ec);
        acceptor->close(ec);
      }
    } catch (...) {
    }
  }

  void cancel_all_sessions() noexcept;

  /**
   * Enter the close path without waiting.
   * Created → Closed immediately.
   * Listening → Closing (+ cancel children); finalize if already drained.
   * Closing → re-assert cancel; finalize if already drained.
   * Closed → no-op.
   */
  void begin_close() noexcept;

  asio::awaitable<void> wait_children_drained();
  asio::awaitable<void> wait_close_complete();
  asio::awaitable<void> accept_loop();
  asio::awaitable<VoidResult> close_impl();
};

struct Server::Session : std::enable_shared_from_this<Session> {
  std::shared_ptr<State> state;
  beast::tcp_stream stream;
  std::string remote_address;
  asio::cancellation_signal io_cancel;
  std::stop_source session_stop;
  bool cancelled{false};

  Session(std::shared_ptr<State> st, tcp::socket sock)
      : state(std::move(st)), stream(std::move(sock)) {
    boost::system::error_code ec;
    auto ep = stream.socket().remote_endpoint(ec);
    if (!ec) {
      // IPv6 as [addr]:port so the address is unambiguous.
      if (ep.address().is_v6()) {
        remote_address = std::format(
            "[{}]:{}", ep.address().to_string(), ep.port());
      } else {
        remote_address =
            std::format("{}:{}", ep.address().to_string(), ep.port());
      }
    }
  }

  void cancel() noexcept {
    if (cancelled) return;
    cancelled = true;
    // stop_token APIs are noexcept; signal emit is not and is owner-executor-oriented.
    if (session_stop.stop_possible() && !session_stop.stop_requested()) {
      session_stop.request_stop();
    }
    try {
      io_cancel.emit(asio::cancellation_type::all);
    } catch (...) {
    }
    // Nonthrowing socket cancel/close (same style as fs/host cancel paths).
    boost::system::error_code ec;
    stream.socket().cancel(ec);
    stream.socket().close(ec);
  }

  [[nodiscard]] bool stop_requested() const noexcept {
    return cancelled || session_stop.stop_requested() ||
           (state && state->stop_source.stop_requested());
  }

  asio::awaitable<bool> write_response(
      unsigned version,
      bool request_keep_alive,
      bool is_head,
      ServerResponse body_src,
      bool force_close);

  asio::awaitable<Result<ServerResponse>> run_handler(ServerRequest req);
  asio::awaitable<void> run();
};

void Server::State::cancel_all_sessions() noexcept {
  for (auto& w : sessions) {
    if (auto s = w.lock()) {
      s->cancel();
    }
  }
}

void Server::State::begin_close() noexcept {
  if (phase == Phase::Closed) return;

  if (phase == Phase::Created) {
    phase = Phase::Closed;
    wake_close_waiters();
    return;
  }

  if (phase == Phase::Listening) {
    phase = Phase::Closing;
    request_shutdown();
    cancel_all_sessions();
    try_finalize_close();  // no-op unless already drained
    return;
  }

  // Already Closing: re-assert cancellation (idempotent) and finalize if drained.
  request_shutdown();
  cancel_all_sessions();
  try_finalize_close();
}

asio::awaitable<void> Server::State::wait_children_drained() {
  // Loop on the real predicate. Parent/slot cancellation must not make close
  // report success while children remain (timers may wake with aborted).
  while (child_count > 0) {
    auto gate = std::make_shared<asio::steady_timer>(ex);
    gate->expires_at(asio::steady_timer::time_point::max());
    drain_waiters.push_back(gate);
    if (child_count == 0) {
      gate->cancel();
    }
    co_await gate->async_wait(asio::as_tuple);
  }
  try_finalize_close();
  co_return;
}

asio::awaitable<void> Server::State::wait_close_complete() {
  while (phase != Phase::Closed) {
    auto gate = std::make_shared<asio::steady_timer>(ex);
    gate->expires_at(asio::steady_timer::time_point::max());
    close_waiters.push_back(gate);
    if (phase == Phase::Closed) {
      gate->cancel();
    }
    co_await gate->async_wait(asio::as_tuple);
  }
  co_return;
}

asio::awaitable<bool> Server::Session::write_response(
    unsigned version,
    bool request_keep_alive,
    bool is_head,
    ServerResponse body_src,
    bool force_close) {
  auto& st = *state;
  const bool keep =
      request_keep_alive && !force_close && !stop_requested();

  try {
    if (is_head) {
      http::response<http::empty_body> res{
          static_cast<http::status>(body_src.status), version};
      apply_response_headers(res, body_src);
      res.content_length(body_src.body.size());
      res.keep_alive(keep);
      auto [ec, n] = co_await http::async_write(
          stream,
          res,
          make_io_token(io_cancel.slot(), st.opts.io_timeout));
      (void)n;
      if (ec) {
        if (!is_benign_disconnect(ec) && !stop_requested()) {
          log::debug(
              "{}",
              make_io_error(kOpWrite, ec, stop_requested(), false).message);
        }
        co_return false;
      }
      co_return keep;
    }

    http::response<http::vector_body<std::uint8_t>> res{
        static_cast<http::status>(body_src.status), version};
    apply_response_headers(res, body_src);
    res.body() = std::move(body_src.body);
    res.prepare_payload();
    res.keep_alive(keep);
    auto [ec, n] = co_await http::async_write(
        stream,
        res,
        make_io_token(io_cancel.slot(), st.opts.io_timeout));
    (void)n;
    if (ec) {
      if (!is_benign_disconnect(ec) && !stop_requested()) {
        log::debug(
            "{}",
            make_io_error(kOpWrite, ec, stop_requested(), false).message);
      }
      co_return false;
    }
    co_return keep;
  } catch (const std::exception& e) {
    log::debug("{}: {}", kOpWrite, e.what());
    co_return false;
  }
}

asio::awaitable<Result<ServerResponse>> Server::Session::run_handler(ServerRequest req) {
  auto st = state;

  struct Box {
    std::optional<Result<ServerResponse>> out;
    std::exception_ptr ep;
    bool done{false};
    bool timed_out{false};
    asio::cancellation_signal cancel;
    asio::steady_timer wake;
    explicit Box(asio::any_io_executor executor)
        : wake(std::move(executor)) {
      wake.expires_at(asio::steady_timer::time_point::max());
    }
    void finish() noexcept {
      done = true;
      try {
        wake.cancel();
      } catch (...) {
      }
    }
  };

  auto box = std::make_shared<Box>(st->ex);
  auto handler_stop = std::stop_source{};

  // std::stop_source is thread-safe. asio::cancellation_signal::emit is not —
  // emit only on the owner executor (timeout handler / posted work).
  auto request_stop_only = [handler_stop]() mutable noexcept {
    if (handler_stop.stop_possible() && !handler_stop.stop_requested()) {
      handler_stop.request_stop();
    }
  };
  auto emit_handler_cancel_on_owner = [box]() noexcept {
    try {
      if (!box->done) {
        box->cancel.emit(asio::cancellation_type::all);
      }
    } catch (...) {
    }
  };
  auto post_handler_cancel = [box, request_stop_only, ex = st->ex]() mutable noexcept {
    request_stop_only();
    try {
      asio::post(ex, [box]() noexcept {
        try {
          if (!box->done) {
            box->cancel.emit(asio::cancellation_type::all);
          }
        } catch (...) {
        }
      });
    } catch (...) {
      // post failed: cooperative stop_token already requested; never emit
      // cancellation_signal from a foreign thread.
    }
  };

  std::stop_callback on_session_stop{
      session_stop.get_token(), post_handler_cancel};

  std::stop_callback on_server_stop{
      st->stop_source.get_token(), post_handler_cancel};

  if (stop_requested()) {
    // Already on owner executor inside the session coroutine.
    request_stop_only();
    emit_handler_cancel_on_owner();
  }

  asio::steady_timer timeout_timer{st->ex};
  timeout_timer.expires_after(st->opts.handler_timeout);
  timeout_timer.async_wait(
      [box, request_stop_only, emit_handler_cancel_on_owner](
          const boost::system::error_code& ec) mutable {
        // Timer completions run on the owner executor.
        try {
          if (ec || box->done) return;
          box->timed_out = true;
          request_stop_only();
          emit_handler_cancel_on_owner();
        } catch (...) {
        }
      });

  try {
    asio::co_spawn(
        st->ex,
        [box, st, tok = handler_stop.get_token(), req = std::move(req)]() mutable
            -> asio::awaitable<void> {
          try {
            box->out = co_await st->handler(tok, std::move(req));
          } catch (...) {
            box->ep = std::current_exception();
          }
          box->finish();
          co_return;
        },
        asio::bind_cancellation_slot(box->cancel.slot(), asio::detached));
  } catch (const std::exception& e) {
    timeout_timer.cancel();
    co_return std::unexpected(Error{
        std::format("http.server.handler: spawn failed: {}", e.what()),
        std::string{kOpHandler},
        0});
  }

  while (!box->done) {
    box->wake.expires_at(asio::steady_timer::time_point::max());
    co_await box->wake.async_wait(asio::as_tuple);
  }
  timeout_timer.cancel();

  if (box->timed_out) {
    co_return std::unexpected(Error{
        "http.server.handler: timed out",
        std::string{kOpHandler},
        ETIMEDOUT});
  }
  if (box->ep) {
    try {
      std::rethrow_exception(box->ep);
    } catch (const std::exception& e) {
      log::debug("http.server.handler: exception: {}", e.what());
    } catch (...) {
      log::debug("http.server.handler: unknown exception");
    }
    co_return std::unexpected(Error{
        "http.server.handler: exception",
        std::string{kOpHandler},
        0});
  }
  if (!box->out) {
    co_return std::unexpected(Error{
        "http.server.handler: no result",
        std::string{kOpHandler},
        0});
  }
  co_return std::move(*box->out);
}

asio::awaitable<void> Server::Session::run() {
  auto st = state;
  beast::flat_buffer buffer;

  std::stop_callback on_server_stop{
      st->stop_source.get_token(),
      [weak = std::weak_ptr<Session>(shared_from_this())]() noexcept {
        if (auto s = weak.lock()) s->cancel();
      }};

  try {
    for (;;) {
      if (stop_requested() || st->phase != Phase::Listening) {
        break;
      }

      http::request_parser<http::vector_body<std::uint8_t>> parser;
      parser.header_limit(clamp_header_limit(st->opts.max_header_bytes));
      parser.body_limit(st->opts.max_request_bytes);

      auto [rec, rn] = co_await http::async_read(
          stream,
          buffer,
          parser,
          make_io_token(io_cancel.slot(), st->opts.io_timeout));
      (void)rn;

      if (rec) {
        if (rec == http::error::end_of_stream || is_benign_disconnect(rec)) {
          break;
        }
        if (rec == http::error::body_limit) {
          co_await write_response(
              11,
              false,
              false,
              fixed_error_response(413),
              /*force_close=*/true);
          break;
        }
        if (is_malformed_request(rec)) {
          if (!stop_requested()) {
            co_await write_response(
                11,
                false,
                false,
                fixed_error_response(400),
                /*force_close=*/true);
          }
          break;
        }
        if (rec == asio::error::operation_aborted || stop_requested()) {
          break;
        }
        if (rec == beast::error::timeout) {
          log::debug(
              "{}",
              make_io_error(kOpRead, rec, stop_requested(), true).message);
          break;
        }
        log::debug(
            "{}",
            make_io_error(kOpRead, rec, stop_requested(), false).message);
        break;
      }

      auto beast_req = parser.release();
      const unsigned version = beast_req.version();
      const bool want_keep_alive = beast_req.keep_alive();
      const bool is_head = beast_req.method() == http::verb::head;

      ServerRequest req;
      req.method = std::string{beast_req.method_string()};
      req.target = std::string{beast_req.target()};
      req.version = version;
      req.remote_address = remote_address;
      req.body = std::move(beast_req.body());
      for (auto it = beast_req.begin(); it != beast_req.end(); ++it) {
        req.headers.emplace_back(
            std::string{it->name_string()}, std::string{it->value()});
      }
      beast_req = {};

      auto handler_result = co_await run_handler(std::move(req));

      if (stop_requested() || st->phase != Phase::Listening) {
        break;
      }

      ServerResponse response;
      bool force_close = false;

      if (!handler_result) {
        const auto& err = handler_result.error();
        if (err.system_code == ETIMEDOUT) {
          response = fixed_error_response(504);
        } else {
          response = fixed_error_response(500);
        }
        force_close = true;
        log::debug("{}: {}", kOpHandler, err.message);
      } else if (auto check =
                     validate_response(*handler_result, st->opts.max_response_bytes)) {
        // Transport-owned headers / invalid names/values / bad status/body
        // → fixed 500.
        log::debug("{}: {}", kOpHandler, check->message);
        response = fixed_error_response(500);
        force_close = true;
      } else {
        response = std::move(*handler_result);
      }

      const bool keep = co_await write_response(
          version,
          want_keep_alive,
          is_head,
          std::move(response),
          force_close);
      if (!keep) break;
    }
  } catch (const std::exception& e) {
    if (!stop_requested()) {
      log::debug("http.server.session: {}", e.what());
    }
  } catch (...) {
    if (!stop_requested()) {
      log::debug("http.server.session: unknown exception");
    }
  }

  boost::system::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  stream.socket().close(ec);
  co_return;
}

asio::awaitable<void> Server::State::accept_loop() {
  // child_count already includes this accept loop (inc'd by async_listen).
  struct Dec {
    Server::State* self;
    ~Dec() {
      if (!self) return;
      self->accept_loop_running = false;
      // Never leave phase Listening without an accept loop.
      if (self->phase == Phase::Listening) {
        self->begin_close();
      }
      self->child_dec();  // may finalize Closing → Closed
    }
  } dec{this};

  auto st = shared_from_this();

  try {
    while (phase == Phase::Listening && acceptor && acceptor->is_open()) {
      auto [ec, sock] = co_await acceptor->async_accept(
          make_cancel_token(accept_cancel.slot()));

      if (ec) {
        if (phase != Phase::Listening ||
            ec == asio::error::operation_aborted ||
            stop_source.stop_requested()) {
          break;
        }
        // Non-cancellation accept failure: enter the same close path.
        log::error(
            "{}",
            make_io_error(kOpAccept, ec, stop_source.stop_requested(), false)
                .message);
        begin_close();
        break;
      }

      if (phase != Phase::Listening) {
        boost::system::error_code ignored;
        sock.close(ignored);
        break;
      }

      auto session = std::make_shared<Session>(st, std::move(sock));
      track_session(session);
      child_inc();
      try {
        asio::co_spawn(
            ex,
            [session]() -> asio::awaitable<void> {
              struct SessionDec {
                std::shared_ptr<Server::State> st;
                ~SessionDec() {
                  if (st) st->child_dec();
                }
              } session_dec{session->state};
              try {
                co_await session->run();
              } catch (...) {
                // SessionDec still runs.
              }
              co_return;
            },
            asio::detached);
      } catch (const std::exception& e) {
        // Spawn failed: reverse the child_inc; session never ran.
        child_dec();
        log::error("http.server.accept: spawn session failed: {}", e.what());
        session->cancel();
      }
    }
  } catch (const std::exception& e) {
    if (phase == Phase::Listening) {
      log::error("http.server.accept: {}", e.what());
      begin_close();
    }
  } catch (...) {
    if (phase == Phase::Listening) {
      log::error("http.server.accept: unknown exception");
      begin_close();
    }
  }
  co_return;
}

asio::awaitable<VoidResult> Server::State::close_impl() {
  try {
    // Drain barrier must not inherit caller cancellation: async_close documents
    // that it waits until Closed even if the surrounding coroutine is cancelled.
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());

    if (phase == Phase::Closed) {
      co_return success();
    }

    // Created / Listening / Closing → begin_close (idempotent).
    begin_close();

    // Join until the State invariant reaches Closed.
    while (phase != Phase::Closed) {
      if (child_count > 0) {
        co_await wait_children_drained();
      } else {
        try_finalize_close();
      }
      if (phase != Phase::Closed) {
        co_await wait_close_complete();
      }
    }
    co_return success();
  } catch (const std::exception& e) {
    co_return std::unexpected(Error{
        std::format("http.server.close: {}", e.what()),
        std::string{kOpClose},
        0});
  } catch (...) {
    co_return std::unexpected(Error{
        "http.server.close: unknown exception",
        std::string{kOpClose},
        0});
  }
}

// ── Server public API ─────────────────────────────────────────────

Server::Server(
    asio::any_io_executor executor,
    ServerOptions options,
    ServerHandler handler)
    : state_(std::make_shared<State>(
          std::move(executor),
          std::move(options),
          std::move(handler))) {}

Server::~Server() { dispose(); }

void Server::dispose() noexcept {
  auto st = state_;
  if (!st) return;
  try {
    // Post keeps a State ref until the handler runs; children retain further.
    asio::post(st->ex, [st]() noexcept {
      try {
        if (st->phase == Phase::Closed) return;
        st->begin_close();
        // If already drained, begin_close → try_finalize_close sets Closed.
        // Otherwise last child_dec finalizes. No leaderless Closing.
      } catch (...) {
      }
    });
  } catch (...) {
  }
}

bool Server::listening() const noexcept {
  return state_ && state_->phase == Phase::Listening;
}

bool Server::closed() const noexcept {
  return !state_ || state_->phase == Phase::Closed;
}

std::optional<ListenAddress> Server::address() const noexcept {
  if (!state_ || state_->phase != Phase::Listening) {
    return std::nullopt;
  }
  return state_->bound;
}

asio::awaitable<Result<ListenAddress>> Server::async_listen() {
  auto st = state_;
  if (!st) {
    co_return std::unexpected(Error{
        "http.server.listen: disposed",
        std::string{kOpListen},
        0});
  }

  try {
    if (st->phase == Phase::Listening) {
      co_return std::unexpected(Error{
          "http.server.listen: already listening",
          std::string{kOpListen},
          0});
    }
    if (st->phase == Phase::Closing) {
      co_return std::unexpected(Error{
          "http.server.listen: closing",
          std::string{kOpListen},
          0});
    }
    if (st->phase == Phase::Closed) {
      co_return std::unexpected(Error{
          "http.server.listen: closed",
          std::string{kOpListen},
          0});
    }

    if (auto verr = validate_options(st->opts)) {
      co_return std::unexpected(std::move(*verr));
    }

    boost::system::error_code ec;
    const auto addr = asio::ip::make_address(st->opts.host, ec);
    if (ec) {
      co_return std::unexpected(Error{
          std::format(
              "http.server.listen: invalid host '{}': {}",
              st->opts.host,
              ec.message()),
          std::string{kOpListen},
          system_code_of(ec)});
    }

    tcp::endpoint ep{addr, st->opts.port};
    tcp::acceptor acc{st->ex};

    acc.open(ep.protocol(), ec);
    if (ec) {
      co_return std::unexpected(Error{
          std::format("http.server.listen: open: {}", ec.message()),
          std::string{kOpListen},
          system_code_of(ec)});
    }

    if (st->opts.reuse_address) {
      acc.set_option(asio::socket_base::reuse_address(true), ec);
      if (ec) {
        boost::system::error_code ignored;
        acc.close(ignored);
        co_return std::unexpected(Error{
            std::format(
                "http.server.listen: set reuse_address: {}", ec.message()),
            std::string{kOpListen},
            system_code_of(ec)});
      }
    }

    acc.bind(ep, ec);
    if (ec) {
      boost::system::error_code ignored;
      acc.close(ignored);
      // Remain Created — retry allowed.
      co_return std::unexpected(Error{
          std::format("http.server.listen: bind: {}", ec.message()),
          std::string{kOpListen},
          system_code_of(ec)});
    }

    const int backlog =
        st->opts.backlog > 0 ? st->opts.backlog : kDefaultBacklog;
    acc.listen(backlog, ec);
    if (ec) {
      boost::system::error_code ignored;
      acc.close(ignored);
      co_return std::unexpected(Error{
          std::format("http.server.listen: listen: {}", ec.message()),
          std::string{kOpListen},
          system_code_of(ec)});
    }

    auto local = acc.local_endpoint(ec);
    if (ec) {
      boost::system::error_code ignored;
      acc.close(ignored);
      co_return std::unexpected(Error{
          std::format("http.server.listen: local_endpoint: {}", ec.message()),
          std::string{kOpListen},
          system_code_of(ec)});
    }

    // Preconstruct TWO ListenAddress values before any commit so post-commit
    // work is only moves / best-effort log (no throwing string copies).
    const auto host_str = local.address().to_string();
    const auto port_num = local.port();
    ListenAddress bound_for_state;
    bound_for_state.host = host_str;
    bound_for_state.port = port_num;
    ListenAddress bound_for_return;
    bound_for_return.host = host_str;
    bound_for_return.port = port_num;

    // Commit: move bound first while acc is still local (if a move somehow
    // failed, outer catch still owns/closes acc with phase Created).
    // One-shot: no reassignment of accept_cancel / stop_source.
    st->bound = std::move(bound_for_state);
    st->acceptor = std::move(acc);
    st->phase = Phase::Listening;
    st->accept_loop_running = true;
    st->child_inc();  // accept loop

    try {
      asio::co_spawn(
          st->ex,
          [st]() -> asio::awaitable<void> {
            co_await st->accept_loop();
            co_return;
          },
          asio::detached);
    } catch (const std::exception& e) {
      // Spawn failed: roll back to Created before balancing the accept-loop
      // child so try_finalize_close (Closing-only) cannot observe this path.
      st->accept_loop_running = false;
      boost::system::error_code ignored;
      if (st->acceptor) {
        st->acceptor->close(ignored);
        st->acceptor.reset();
      }
      st->bound = {};
      st->phase = Phase::Created;
      st->child_dec();  // pairs the accept-loop child_inc; phase is Created
      co_return std::unexpected(Error{
          std::format("http.server.listen: spawn accept loop: {}", e.what()),
          std::string{kOpListen},
          0});
    }

    // Committed + accept loop running: never return error from here.
    try {
      log::info(
          "http.server listening on {}:{}",
          bound_for_return.host,
          bound_for_return.port);
    } catch (...) {
    }
    co_return std::move(bound_for_return);
  } catch (const std::exception& e) {
    // Commit tail is move/log only; this path should not see a live listener.
    // Do not co_return a copy of st->bound (allocation may be why we are here).
    if (st->phase == Phase::Listening && st->accept_loop_running) {
      st->begin_close();
      co_return std::unexpected(Error{
          std::format(
              "http.server.listen: post-commit failure while listening: {}",
              e.what()),
          std::string{kOpListen},
          0});
    }
    if (st->phase == Phase::Listening && !st->accept_loop_running) {
      boost::system::error_code ignored;
      if (st->acceptor) {
        st->acceptor->close(ignored);
        st->acceptor.reset();
      }
      st->bound = {};
      st->phase = Phase::Created;
    }
    // phase still Created: local acceptor (if any) was not moved; close it.
    // (acc is out of scope here — open failures already closed their sockets.)
    co_return std::unexpected(Error{
        std::format("http.server.listen: {}", e.what()),
        std::string{kOpListen},
        0});
  } catch (...) {
    if (st->phase == Phase::Listening && st->accept_loop_running) {
      st->begin_close();
      co_return std::unexpected(Error{
          "http.server.listen: post-commit failure while listening",
          std::string{kOpListen},
          0});
    }
    if (st->phase == Phase::Listening && !st->accept_loop_running) {
      boost::system::error_code ignored;
      if (st->acceptor) {
        st->acceptor->close(ignored);
        st->acceptor.reset();
      }
      st->bound = {};
      st->phase = Phase::Created;
    }
    co_return std::unexpected(Error{
        "http.server.listen: unknown exception",
        std::string{kOpListen},
        0});
  }
}

asio::awaitable<VoidResult> Server::async_close() {
  auto st = state_;
  if (!st) {
    co_return success();
  }
  co_return co_await st->close_impl();
}

}  // namespace vacps::http
