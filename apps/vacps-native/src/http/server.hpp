#pragma once

#include "app/error.hpp"
#include "http/session.hpp"
#include "quickjs/host.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vacps::http {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/** Bind target for Server — always supplied by the caller (JS createServer options). */
struct ListenEndpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{8788};
};

/** Max time to wait for in-flight sessions after signal before forcing cancel. */
inline constexpr std::chrono::seconds kGracefulSessionDrain{10};
/** Max time to wait for native Promise async scope after process kill. */
inline constexpr std::chrono::seconds kGracefulAsyncIdle{5};

/**
 * HTTP transport only — zero product routing. Lifetime: shared_ptr.
 *
 * Graceful stop (SIGINT/SIGTERM) — process-level async scope:
 *  1. close acceptor (no new accepts)
 *  2. cancel active sessions (close sockets)
 *  3. wait for sessions to finish (up to kGracefulSessionDrain)
 *  4. cancel Host progress waiters / process registry
 *  5. wait for spawn_js_promise outstanding ops (up to kGracefulAsyncIdle)
 *  6. JS shutdown export
 *  7. stop io_context
 */
class Server : public std::enable_shared_from_this<Server> {
 public:
  Server(
      asio::io_context& ioc,
      ListenEndpoint listen,
      std::shared_ptr<vacps::js::Host> script);

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  [[nodiscard]] VoidResult start();

  /** Close acceptor/signals only (does not stop io_context). */
  void close() noexcept;

  /**
   * Begin graceful process shutdown (signal / process stop).
   * Safe to call multiple times; only the first run performs the sequence.
   */
  void request_stop();

  void session_started(std::shared_ptr<Session> session) noexcept;
  void session_finished() noexcept;
  void cancel_all_sessions() noexcept;

  [[nodiscard]] std::size_t active_sessions() const noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] bool stopping() const noexcept;

 private:
  asio::awaitable<void> accept_loop();
  asio::awaitable<void> graceful_shutdown();

  asio::io_context& ioc_;
  ListenEndpoint listen_;
  std::shared_ptr<vacps::js::Host> script_;
  tcp::acceptor acceptor_;
  asio::signal_set signals_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> active_sessions_{0};
  /** Live sessions for cancel-on-stop (io_context thread only). */
  std::vector<std::weak_ptr<Session>> sessions_;
};

}  // namespace vacps::http
