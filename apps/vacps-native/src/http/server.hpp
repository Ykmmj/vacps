#pragma once

#include "app/error.hpp"
#include "http/request_handler.hpp"
#include "http/session.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vacps::http {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/** Bind target for Server — always supplied by the caller (JS Server options). */
struct ListenEndpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{8788};
};

/**
 * HTTP transport only — zero product routing. Lifetime: shared_ptr.
 *
 * Construction only stores ListenEndpoint; bind/listen happens in start().
 * close() stops accepting and cancels active sessions; it does NOT stop
 * io_context, cancel process registry, or run JS shutdown.
 *
 * Process-level SIGINT/SIGTERM live in runtime::ShutdownCoordinator.
 * Host does not track Server instances; JS owns close() order (ownership doc).
 * This class must not own signal_set, call ioc.stop(), or invoke ScriptRuntime APIs.
 *
 * Request handling is injected via IRequestHandler (no QuickJS dependency).
 */
class Server : public std::enable_shared_from_this<Server> {
 public:
  Server(
      asio::io_context& ioc,
      ListenEndpoint listen,
      std::shared_ptr<IRequestHandler> handler);

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /**
   * Force-stop accept + cancel sessions (RAII). Same as close(); best-effort
   * when JS omitted close() (docs/NATIVE_RESOURCE_OWNERSHIP.md §五).
   */
  ~Server();

  /** Sync bind + listen + spawn accept loop. Fails on bad host / bind errors. */
  [[nodiscard]] VoidResult start();

  /**
   * Stop accepting and cancel/drain active sessions. Idempotent.
   * Does not stop io_context or tear down ScriptRuntime.
   */
  void close() noexcept;

  void session_started(std::shared_ptr<Session> session) noexcept;
  void session_finished() noexcept;
  void cancel_all_sessions() noexcept;

  [[nodiscard]] std::size_t active_sessions() const noexcept;
  [[nodiscard]] bool is_open() const noexcept;

 private:
  asio::awaitable<void> accept_loop();

  ListenEndpoint listen_;
  std::shared_ptr<IRequestHandler> handler_;
  tcp::acceptor acceptor_;
  std::atomic<std::size_t> active_sessions_{0};
  /** Live sessions for cancel (io_context thread only). */
  std::vector<std::weak_ptr<Session>> sessions_;
};

}  // namespace vacps::http
