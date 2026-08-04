#pragma once

/**
 * Pure Asio/Beast inbound HTTP/1 transport (domain layer).
 *
 * - No io_context ownership / stop(); caller supplies any_io_executor.
 * - No QuickJS, product routes, or JSON error policy.
 * - Binary-first DTOs; transport-owned hop-by-hop / framing headers
 *   (Connection, Content-Length, Transfer-Encoding, Upgrade, Trailer, TE,
 *   Keep-Alive, Proxy-Connection) must not be set by the handler.
 * - One-shot lifecycle: created → listening → closing → closed.
 */

#include "app/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace vacps::http {

namespace asio = boost::asio;

struct ServerRequest {
  std::string method;
  /** Request-target (path + query). */
  std::string target;
  /** HTTP version × 10 (10 = 1.0, 11 = 1.1). */
  unsigned version{11};
  /** Wire headers; duplicates preserved in arrival order. */
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
  std::string remote_address;
};

struct ServerResponse {
  int status{200};
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
};

struct ServerOptions {
  /**
   * Numeric IPv4/IPv6 bind literal only (never DNS / hostnames).
   * Validated with asio::ip::make_address at listen (and at the JS binding
   * edge before Promise creation). Default "127.0.0.1".
   */
  std::string host{"127.0.0.1"};
  /** 0 = ephemeral port. */
  std::uint16_t port{0};
  /** Max request body bytes (Beast body_limit). Must be > 0. Default 1 MiB. */
  std::size_t max_request_bytes{1u * 1024u * 1024u};
  /** Max header block bytes (Beast header_limit). Must be > 0. Default 64 KiB. */
  std::size_t max_header_bytes{64u * 1024u};
  /** Max response body bytes accepted from the handler. Must be > 0. Default 8 MiB. */
  std::size_t max_response_bytes{8u * 1024u * 1024u};
  /** Per read/write idle budget. Must be > 0. Default 30s. */
  std::chrono::milliseconds io_timeout{30'000};
  /**
   * Wall-clock deadline for one handler invocation. Starts a timer that
   * requests cooperative cancellation via the handler stop_token (and may
   * map to fixed 504). This is not a hard completion bound: async_close
   * still waits for handler drain, so arbitrary non-cooperative C++ handlers
   * cannot be forcibly destroyed. Must be > 0. Default 30s.
   */
  std::chrono::milliseconds handler_timeout{30'000};
  /** listen(2) backlog; <= 0 → implementation default. */
  int backlog{128};
  bool reuse_address{true};
};

struct ListenAddress {
  /**
   * Raw numeric address from the bound endpoint (asio address::to_string).
   * IPv6 is unbracketed; callers must format when building a URL
   * (e.g. "[" + host + "]:" + port).
   */
  std::string host;
  std::uint16_t port{0};
};

/**
 * Per-request handler. Move-only.
 * Cooperative cancellation via stop_token (server close / session cancel /
 * handler_timeout wall deadline). May return domain Error → transport writes
 * fixed 500. handler_timeout does not forcibly destroy a non-cooperative
 * handler; close still drains it.
 */
using ServerHandler = std::move_only_function<
    asio::awaitable<Result<ServerResponse>>(std::stop_token, ServerRequest)>;

/**
 * Inbound HTTP/1 server handle. Owns a shared private State used by the
 * accept loop and sessions across every suspension point.
 *
 * Thread / executor model:
 * - async_listen / async_close / the accept loop / sessions / handler all run
 *   on the caller-provided owner executor (typically a single-threaded
 *   io_context).
 * - listening() / closed() / address() are owner-executor-only snapshots.
 *   Do not call them from arbitrary threads.
 * - dispose() / destructor may be invoked from any context; they only post
 *   cancellation onto the owner executor and never block. State self-retains
 *   until the accept loop and sessions drain, then finalizes to Closed.
 */
class Server {
 public:
  /**
   * Store executor/options/handler only — no bind.
   * @param executor Host io_context executor (not owned).
   * @param options  Bind and limit configuration (validated on listen).
   * @param handler  Request handler.
   *
   * Contract: Narrow
   * Preconditions: handler contains a callable target.
   */
  Server(
      asio::any_io_executor executor,
      ServerOptions options,
      ServerHandler handler);

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&&) noexcept = default;
  Server& operator=(Server&&) noexcept = default;

  /**
   * Non-blocking: request the same cancellation as close on the owner
   * executor. State self-retains until accept loop and sessions drain and
   * phase reaches Closed.
   */
  ~Server();

  /**
   * Bind + listen + spawn accept loop. Returns the effective address.
   * Bind/listen failure rolls phase back to created (retry allowed).
   * Rejects a second listen while listening/closing/closed.
   * Must be co_awaited on the owner executor.
   */
  [[nodiscard]] asio::awaitable<Result<ListenAddress>> async_listen();

  /**
   * Idempotent close. Concurrent callers join the same drain barrier.
   * Does not return until the accept loop has exited and all sessions
   * (including pending handlers) have drained and phase is Closed.
   * Never stops the executor. close-before-listen → Closed.
   * Must be co_awaited on the owner executor.
   */
  [[nodiscard]] asio::awaitable<VoidResult> async_close();

  /** Finalizer/dtor path: request cancel; never throws; never blocks. */
  void dispose() noexcept;

  /**
   * Owner-executor-only. True while phase == Listening.
   * Not safe to call from arbitrary threads.
   */
  [[nodiscard]] bool listening() const noexcept;

  /**
   * Owner-executor-only. True while phase == Closed (or handle is empty).
   * Not safe to call from arbitrary threads.
   */
  [[nodiscard]] bool closed() const noexcept;

  /**
   * Owner-executor-only. Effective bind address while Listening; nullopt
   * otherwise. Not safe to call from arbitrary threads.
   */
  [[nodiscard]] std::optional<ListenAddress> address() const noexcept;

 private:
  struct State;
  struct Session;
  std::shared_ptr<State> state_;
};

}  // namespace vacps::http
