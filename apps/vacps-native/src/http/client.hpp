#pragma once

#include "app/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace boost::asio::ssl {
class context;
}

namespace vacps::http {

namespace asio = boost::asio;

struct ParsedUrl {
  std::string scheme;  // "http" | "https"
  std::string host;
  std::string port;    // numeric string
  std::string target;  // path + query, at least "/"
  bool host_is_ip_literal{false};
  bool host_is_ipv6{false};
};

/**
 * Parse an absolute HTTP(S) URL via Ada (WHATWG). Userinfo is rejected and
 * fragments are omitted from the request target.
 */
[[nodiscard]] Result<ParsedUrl> parse_url(std::string_view url);

struct ClientRequest {
  std::string method{"GET"};
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
  /** One absolute wall budget for pool wait and every I/O phase. */
  std::chrono::milliseconds timeout{30'000};
  /** Beast enforces this limit while reading, before the full body is buffered. */
  std::size_t max_response_bytes{8u * 1024u * 1024u};
};

struct ClientResponse {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
};

/** Resolve a CA path: injected file path, then platform defaults. No getenv. */
[[nodiscard]] Result<std::string> resolve_ca_bundle(
    std::string_view explicit_path);

/**
 * Runtime-scoped pooled HTTP/1.1 client, confined to its executor.
 *
 * One physical connection serves at most one request at a time. Each origin
 * and the client as a whole have fixed active-connection limits; retained idle
 * descriptors also have a global bound. DNS/TCP/TLS work is performed only
 * for new physical connections.
 *
 * Narrow contract: Options fields are positive and every call/destruction
 * occurs on the supplied executor. The module composition establishes this.
 */
class Client final {
 public:
  struct Options {
    std::size_t max_connections_per_origin{16};
    std::size_t max_active_connections{64};
    std::size_t max_idle_connections{32};
    std::chrono::seconds idle_timeout{30};
  };

  Client(
      asio::any_io_executor executor,
      std::string ca_bundle);
  Client(
      asio::any_io_executor executor,
      std::string ca_bundle,
      Options options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /**
   * Perform one request. Cancellation is marshalled onto the client executor.
   * Stale pooled connections are discarded on failure and are not retried.
   */
  [[nodiscard]] asio::awaitable<Result<ClientResponse>> request(
      std::stop_token stop,
      ClientRequest req);

 private:
  using OriginKey = std::tuple<std::string, std::string, std::string>;
  using Deadline = std::chrono::steady_clock::time_point;

  struct Connection;
  struct OriginPool;
  struct RequestGate;
  using PoolMap = std::map<OriginKey, std::unique_ptr<OriginPool>>;
  using PoolIterator = PoolMap::iterator;

  class OriginLease;
  class GateLease;
  class ConnectionLease;

  [[nodiscard]] PoolIterator acquire_pool(const ParsedUrl& url);
  void release_pool(PoolIterator position) noexcept;
  [[nodiscard]] std::unique_ptr<Connection> take_idle(OriginPool& pool);
  void release(OriginPool& pool, std::unique_ptr<Connection> connection) noexcept;
  void prune_expired_idle(Deadline now) noexcept;
  void evict_oldest_idle() noexcept;

  [[nodiscard]] Result<asio::ssl::context*> ensure_tls_context();
  [[nodiscard]] asio::awaitable<Result<std::unique_ptr<Connection>>>
  connect(
      const ParsedUrl& url,
      asio::cancellation_slot cancel,
      const std::stop_token& stop,
      Deadline deadline);
  [[nodiscard]] asio::awaitable<Result<ClientResponse>> exchange(
      Connection& connection,
      const ParsedUrl& url,
      ClientRequest req,
      asio::cancellation_slot cancel,
      const std::stop_token& stop,
      Deadline deadline);

  asio::any_io_executor executor_;
  std::string ca_bundle_;
  Options options_;
  std::unique_ptr<RequestGate> request_gate_;
  // Pools are destroyed before the TLS context used by their TLS streams.
  std::unique_ptr<asio::ssl::context> tls_context_;
  PoolMap pools_;
  std::size_t idle_connections_{0};
};

}  // namespace vacps::http
