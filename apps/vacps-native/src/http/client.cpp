#include "http/client.hpp"

#include <ada.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/channel_error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

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
namespace beast_http = beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using steady_clock = std::chrono::steady_clock;

struct OperationCancel {
  asio::cancellation_signal signal;
};

[[nodiscard]] bool ieq(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(left[index])) !=
        std::tolower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool is_client_owned_header(std::string_view name) {
  return ieq(name, "host") || ieq(name, "content-length") ||
         ieq(name, "transfer-encoding") || ieq(name, "connection") ||
         ieq(name, "proxy-connection") || ieq(name, "keep-alive") ||
         ieq(name, "te") || ieq(name, "trailer") || ieq(name, "upgrade") ||
         ieq(name, "expect");
}

[[nodiscard]] beast_http::verb parse_verb(std::string_view method) {
  if (ieq(method, "GET")) {
    return beast_http::verb::get;
  }
  if (ieq(method, "POST")) {
    return beast_http::verb::post;
  }
  if (ieq(method, "PUT")) {
    return beast_http::verb::put;
  }
  if (ieq(method, "DELETE")) {
    return beast_http::verb::delete_;
  }
  if (ieq(method, "PATCH")) {
    return beast_http::verb::patch;
  }
  if (ieq(method, "HEAD")) {
    return beast_http::verb::head;
  }
  if (ieq(method, "OPTIONS")) {
    return beast_http::verb::options;
  }
  return beast_http::verb::unknown;
}

[[nodiscard]] std::string host_header_value(const ParsedUrl& url) {
  const bool default_port =
      (url.scheme == "https" && url.port == "443") ||
      (url.scheme == "http" && url.port == "80");
  std::string authority =
      url.host_is_ipv6 ? "[" + url.host + "]" : url.host;
  if (default_port) {
    return authority;
  }
  return authority + ":" + url.port;
}

[[nodiscard]] bool deadline_expired(steady_clock::time_point deadline) {
  return steady_clock::now() >= deadline;
}

[[nodiscard]] int system_code(const boost::system::error_code& ec) {
  return ec ? ec.value() : 0;
}

[[nodiscard]] Error phase_error(
    std::string_view operation,
    const boost::system::error_code& ec,
    const std::stop_token& stop,
    steady_clock::time_point deadline) {
  const bool cancelled =
      ec == asio::error::operation_aborted ||
      ec == asio::experimental::error::channel_cancelled;
  const bool timed_out = ec == beast::error::timeout;
  if (cancelled || timed_out) {
    if (stop.stop_requested()) {
      return Error{
          std::format("{}: cancelled", operation),
          std::string{operation},
          ECANCELED};
    }
    if (timed_out || deadline_expired(deadline)) {
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
      system_code(ec)};
}

[[nodiscard]] Error stopped_error(std::string_view operation) {
  return Error{
      std::format("{}: cancelled", operation),
      std::string{operation},
      ECANCELED};
}

[[nodiscard]] Error io_error(
    std::string_view operation,
    const boost::system::error_code& ec) {
  return Error{
      std::format("{}: {}", operation, ec.message()),
      std::string{operation},
      system_code(ec)};
}

/**
 * The stop flag is checked immediately before starting each sequential I/O
 * operation. This is required because cancellation_signal is edge-triggered:
 * a signal emitted between two operations is not replayed into the next slot.
 */
[[nodiscard]] std::optional<Error> stop_before_operation(
    const std::stop_token& stop,
    std::string_view operation) {
  if (stop.stop_requested()) {
    return stopped_error(operation);
  }
  return std::nullopt;
}

auto io_token(
    asio::cancellation_slot slot,
    steady_clock::time_point deadline) {
  return asio::cancel_at(
      deadline,
      asio::cancellation_type::all)(
      asio::bind_cancellation_slot(
          slot,
          asio::as_tuple(asio::use_awaitable)));
}

template <class Stream>
asio::awaitable<Result<ClientResponse>> exchange_stream(
    Stream& stream,
    beast::flat_buffer& buffer,
    bool& reusable,
    const ParsedUrl& url,
    ClientRequest req,
    asio::cancellation_slot cancel,
    const std::stop_token& stop,
    steady_clock::time_point deadline) {
  constexpr std::string_view kWriteOperation = "http.request.write";
  constexpr std::string_view kReadOperation = "http.request.read";

  const beast_http::verb verb = parse_verb(req.method);
  beast_http::request<beast_http::vector_body<std::uint8_t>> message{
      verb,
      url.target,
      11};
  message.set(beast_http::field::host, host_header_value(url));
  message.set(beast_http::field::user_agent, "vacps-native");
  message.keep_alive(true);
  for (const auto& [name, value] : req.headers) {
    message.set(name, value);
  }
  if (!req.body.empty()) {
    message.body() = std::move(req.body);
    message.prepare_payload();
  }

  if (auto stopped = stop_before_operation(stop, kWriteOperation)) {
    co_return std::unexpected(std::move(*stopped));
  }
  auto [write_ec, bytes_written] = co_await beast_http::async_write(
      stream,
      message,
      io_token(cancel, deadline));
  (void)bytes_written;
  if (write_ec) {
    co_return std::unexpected(
        phase_error(kWriteOperation, write_ec, stop, deadline));
  }

  for (;;) {
    if (auto stopped = stop_before_operation(stop, kReadOperation)) {
      co_return std::unexpected(std::move(*stopped));
    }

    beast_http::response_parser<beast_http::vector_body<std::uint8_t>> parser;
    parser.body_limit(req.max_response_bytes);
    if (verb == beast_http::verb::head) {
      parser.skip(true);
    }

    auto [read_ec, bytes_read] = co_await beast_http::async_read(
        stream,
        buffer,
        parser,
        io_token(cancel, deadline));
    (void)bytes_read;
    if (read_ec == beast_http::error::body_limit) {
      co_return std::unexpected(Error{
          std::format(
              "http.request.read: response body exceeds maxResponseBytes {}",
              req.max_response_bytes),
          std::string{kReadOperation},
          0});
    }
    if (read_ec) {
      co_return std::unexpected(
          phase_error(kReadOperation, read_ec, stop, deadline));
    }

    const unsigned status = parser.get().result_int();
    const bool keep_alive =
        parser.keep_alive() && !parser.need_eof() && status != 101;
    auto response = parser.release();

    // Ignore informational responses and continue parsing the final response.
    // 101 switches protocol, so it is returned and the connection is discarded.
    if (status >= 100 && status < 200 && status != 101) {
      continue;
    }

    ClientResponse result;
    result.status = static_cast<int>(status);
    result.body = std::move(response.body());
    for (auto field = response.begin(); field != response.end(); ++field) {
      result.headers.emplace_back(
          std::string{field->name_string()},
          std::string{field->value()});
    }
    reusable = keep_alive;
    co_return result;
  }
}

}  // namespace

struct Client::Connection {
  using PlainStream = beast::tcp_stream;
  using TlsStream = ssl::stream<beast::tcp_stream>;

  explicit Connection(asio::any_io_executor executor)
      : plain(std::make_unique<PlainStream>(std::move(executor))) {}

  Connection(asio::any_io_executor executor, ssl::context& context)
      : tls(std::make_unique<TlsStream>(std::move(executor), context)) {}

  std::unique_ptr<PlainStream> plain;
  std::unique_ptr<TlsStream> tls;
  beast::flat_buffer buffer;
  steady_clock::time_point idle_since{};
  bool reusable{false};
};

struct Client::OriginPool {
  using Gate = asio::experimental::channel<void(boost::system::error_code)>;

  OriginPool(asio::any_io_executor executor, std::size_t capacity)
      : gate(std::move(executor), capacity) {
    idle.reserve(capacity);
  }

  Gate gate;
  std::vector<std::unique_ptr<Connection>> idle;
  std::size_t users{0};
};

struct Client::RequestGate {
  RequestGate(asio::any_io_executor executor, std::size_t capacity)
      : gate(std::move(executor), capacity) {}

  OriginPool::Gate gate;
};

class Client::OriginLease final {
 public:
  OriginLease(Client& client, PoolIterator position) noexcept
      : client_(client), position_(position) {}

  ~OriginLease() noexcept {
    client_.release_pool(position_);
  }

  OriginLease(const OriginLease&) = delete;
  OriginLease& operator=(const OriginLease&) = delete;
  OriginLease(OriginLease&&) = delete;
  OriginLease& operator=(OriginLease&&) = delete;

  [[nodiscard]] OriginPool& pool() noexcept {
    return *position_->second;
  }

 private:
  Client& client_;
  PoolIterator position_;
};

class Client::GateLease final {
 public:
  explicit GateLease(OriginPool::Gate& gate) noexcept : gate_(gate) {}

  ~GateLease() noexcept {
    (void)gate_.try_receive([](boost::system::error_code) noexcept {});
  }

  GateLease(const GateLease&) = delete;
  GateLease& operator=(const GateLease&) = delete;
  GateLease(GateLease&&) = delete;
  GateLease& operator=(GateLease&&) = delete;

 private:
  OriginPool::Gate& gate_;
};

class Client::ConnectionLease final {
 public:
  ConnectionLease(
      Client& client,
      OriginPool& pool,
      std::unique_ptr<Connection> connection) noexcept
      : client_(client), pool_(pool), connection_(std::move(connection)) {}

  ~ConnectionLease() noexcept {
    client_.release(pool_, std::move(connection_));
  }

  ConnectionLease(const ConnectionLease&) = delete;
  ConnectionLease& operator=(const ConnectionLease&) = delete;
  ConnectionLease(ConnectionLease&&) = delete;
  ConnectionLease& operator=(ConnectionLease&&) = delete;

  [[nodiscard]] Connection* get() noexcept {
    return connection_.get();
  }

  void reset(std::unique_ptr<Connection> connection) noexcept {
    connection_ = std::move(connection);
  }

 private:
  Client& client_;
  OriginPool& pool_;
  std::unique_ptr<Connection> connection_;
};

Client::Client(
    asio::any_io_executor executor,
    std::string ca_bundle)
    : Client(std::move(executor), std::move(ca_bundle), Options{}) {}

Client::Client(
    asio::any_io_executor executor,
    std::string ca_bundle,
    Options options)
    : executor_(std::move(executor)),
      ca_bundle_(std::move(ca_bundle)),
      options_(options),
      request_gate_(std::make_unique<RequestGate>(
          executor_, options_.max_active_connections)) {}

Client::~Client() = default;

Client::PoolIterator Client::acquire_pool(const ParsedUrl& url) {
  OriginKey key{url.scheme, url.host, url.port};
  auto position = pools_.find(key);
  if (position == pools_.end()) {
    auto pool = std::make_unique<OriginPool>(
        executor_,
        options_.max_connections_per_origin);
    position = pools_.emplace(std::move(key), std::move(pool)).first;
  }
  ++position->second->users;
  return position;
}

void Client::release_pool(PoolIterator position) noexcept {
  OriginPool& pool = *position->second;
  --pool.users;
  if (pool.users == 0 && pool.idle.empty()) {
    pools_.erase(position);
  }
}

std::unique_ptr<Client::Connection> Client::take_idle(OriginPool& pool) {
  const auto now = steady_clock::now();
  while (!pool.idle.empty()) {
    auto connection = std::move(pool.idle.back());
    pool.idle.pop_back();
    --idle_connections_;
    if (now - connection->idle_since <= options_.idle_timeout) {
      connection->reusable = false;
      return connection;
    }
  }
  return nullptr;
}

void Client::release(
    OriginPool& pool,
    std::unique_ptr<Connection> connection) noexcept {
  if (connection == nullptr || !connection->reusable) {
    return;
  }

  const auto now = steady_clock::now();
  if (idle_connections_ >= options_.max_idle_connections) {
    prune_expired_idle(now);
  }
  if (idle_connections_ >= options_.max_idle_connections) {
    evict_oldest_idle();
  }

  connection->idle_since = now;
  pool.idle.push_back(std::move(connection));
  ++idle_connections_;
}

void Client::prune_expired_idle(Deadline now) noexcept {
  for (auto position = pools_.begin(); position != pools_.end();) {
    OriginPool& pool = *position->second;
    const std::size_t old_size = pool.idle.size();
    std::erase_if(pool.idle, [&](const auto& connection) {
      return now - connection->idle_since > options_.idle_timeout;
    });
    idle_connections_ -= old_size - pool.idle.size();

    if (pool.users == 0 && pool.idle.empty()) {
      position = pools_.erase(position);
    } else {
      ++position;
    }
  }
}

void Client::evict_oldest_idle() noexcept {
  auto oldest_pool = pools_.end();
  std::size_t oldest_index = 0;
  Deadline oldest_time = Deadline::max();

  for (auto position = pools_.begin(); position != pools_.end(); ++position) {
    const auto& idle = position->second->idle;
    for (std::size_t index = 0; index < idle.size(); ++index) {
      if (idle[index]->idle_since < oldest_time) {
        oldest_pool = position;
        oldest_index = index;
        oldest_time = idle[index]->idle_since;
      }
    }
  }

  OriginPool& pool = *oldest_pool->second;
  pool.idle.erase(pool.idle.begin() + static_cast<std::ptrdiff_t>(oldest_index));
  --idle_connections_;
  if (pool.users == 0 && pool.idle.empty()) {
    pools_.erase(oldest_pool);
  }
}

Result<ssl::context*> Client::ensure_tls_context() {
  constexpr std::string_view kTlsOperation = "http.request.tls";
  if (tls_context_ != nullptr) {
    return tls_context_.get();
  }

  auto ca_path = resolve_ca_bundle(ca_bundle_);
  if (!ca_path) {
    return std::unexpected(std::move(ca_path.error()));
  }

  auto context = std::make_unique<ssl::context>(ssl::context::tls_client);
  boost::system::error_code ec;
  context->set_options(
      ssl::context::default_workarounds | ssl::context::no_sslv2 |
          ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
          ssl::context::no_tlsv1_1,
      ec);
  if (ec) {
    return std::unexpected(io_error(kTlsOperation, ec));
  }

#if defined(SSL_CTX_set_min_proto_version)
  if (::SSL_CTX_set_min_proto_version(
          context->native_handle(),
          TLS1_2_VERSION) != 1) {
    return std::unexpected(Error{
        "http.request.tls: failed to require TLS 1.2",
        std::string{kTlsOperation},
        0});
  }
#endif

  context->set_verify_mode(ssl::verify_peer, ec);
  if (ec) {
    return std::unexpected(io_error(kTlsOperation, ec));
  }
  context->load_verify_file(*ca_path, ec);
  if (ec) {
    return std::unexpected(io_error(kTlsOperation, ec));
  }

  tls_context_ = std::move(context);
  return tls_context_.get();
}

asio::awaitable<Result<std::unique_ptr<Client::Connection>>> Client::connect(
    const ParsedUrl& url,
    asio::cancellation_slot cancel,
    const std::stop_token& stop,
    Deadline deadline) {
  constexpr std::string_view kResolveOperation = "http.request.resolve";
  constexpr std::string_view kConnectOperation = "http.request.connect";
  constexpr std::string_view kTlsOperation = "http.request.tls";

  ssl::context* tls_context = nullptr;
  if (url.scheme == "https") {
    auto context = ensure_tls_context();
    if (!context) {
      co_return std::unexpected(std::move(context.error()));
    }
    tls_context = *context;
  }

  if (auto stopped = stop_before_operation(stop, kResolveOperation)) {
    co_return std::unexpected(std::move(*stopped));
  }
  tcp::resolver resolver{executor_};
  auto [resolve_ec, endpoints] = co_await resolver.async_resolve(
      url.host,
      url.port,
      io_token(cancel, deadline));
  if (resolve_ec) {
    co_return std::unexpected(
        phase_error(kResolveOperation, resolve_ec, stop, deadline));
  }

  if (url.scheme == "http") {
    auto connection = std::make_unique<Connection>(executor_);
    if (auto stopped = stop_before_operation(stop, kConnectOperation)) {
      co_return std::unexpected(std::move(*stopped));
    }
    auto [connect_ec, endpoint] = co_await connection->plain->async_connect(
        endpoints,
        io_token(cancel, deadline));
    (void)endpoint;
    if (connect_ec) {
      co_return std::unexpected(
          phase_error(kConnectOperation, connect_ec, stop, deadline));
    }
    co_return connection;
  }

  auto connection = std::make_unique<Connection>(executor_, *tls_context);
  if (!url.host_is_ip_literal &&
      ::SSL_set_tlsext_host_name(
          connection->tls->native_handle(),
          url.host.c_str()) != 1) {
    co_return std::unexpected(Error{
        "http.request.tls: failed to set SNI hostname",
        std::string{kTlsOperation},
        0});
  }
  connection->tls->set_verify_callback(ssl::host_name_verification(url.host));

  if (auto stopped = stop_before_operation(stop, kConnectOperation)) {
    co_return std::unexpected(std::move(*stopped));
  }
  auto [connect_ec, endpoint] =
      co_await beast::get_lowest_layer(*connection->tls).async_connect(
          endpoints,
          io_token(cancel, deadline));
  (void)endpoint;
  if (connect_ec) {
    co_return std::unexpected(
        phase_error(kConnectOperation, connect_ec, stop, deadline));
  }

  if (auto stopped = stop_before_operation(stop, kTlsOperation)) {
    co_return std::unexpected(std::move(*stopped));
  }
  auto [handshake_ec] = co_await connection->tls->async_handshake(
      ssl::stream_base::client,
      io_token(cancel, deadline));
  if (handshake_ec) {
    co_return std::unexpected(
        phase_error(kTlsOperation, handshake_ec, stop, deadline));
  }
  co_return connection;
}

asio::awaitable<Result<ClientResponse>> Client::exchange(
    Connection& connection,
    const ParsedUrl& url,
    ClientRequest req,
    asio::cancellation_slot cancel,
    const std::stop_token& stop,
    Deadline deadline) {
  connection.reusable = false;
  if (connection.plain != nullptr) {
    co_return co_await exchange_stream(
        *connection.plain,
        connection.buffer,
        connection.reusable,
        url,
        std::move(req),
        cancel,
        stop,
        deadline);
  }
  co_return co_await exchange_stream(
      *connection.tls,
      connection.buffer,
      connection.reusable,
      url,
      std::move(req),
      cancel,
      stop,
      deadline);
}

asio::awaitable<Result<ClientResponse>> Client::request(
    std::stop_token stop,
    ClientRequest req) {
  constexpr std::string_view kRequestOperation = "http.request";
  constexpr std::string_view kOriginAcquireOperation =
      "http.request.acquire.origin";
  constexpr std::string_view kGlobalAcquireOperation =
      "http.request.acquire.global";

  if (req.url.empty()) {
    co_return std::unexpected(Error{
        "http.request: url required",
        std::string{kRequestOperation},
        0});
  }
  if (req.timeout <= std::chrono::milliseconds::zero()) {
    co_return std::unexpected(Error{
        "http.request: timeout must be > 0",
        std::string{kRequestOperation},
        0});
  }
  if (req.max_response_bytes == 0) {
    co_return std::unexpected(Error{
        "http.request: maxResponseBytes must be > 0",
        std::string{kRequestOperation},
        0});
  }
  if (parse_verb(req.method) == beast_http::verb::unknown) {
    co_return std::unexpected(Error{
        std::format("http.request: unknown method '{}'", req.method),
        std::string{kRequestOperation},
        0});
  }
  for (const auto& [name, value] : req.headers) {
    (void)value;
    if (is_client_owned_header(name)) {
      co_return std::unexpected(Error{
          std::format(
              "http.request: header '{}' is reserved (owned by the client)",
              name),
          std::string{kRequestOperation},
          0});
    }
  }
  if (stop.stop_requested()) {
    co_return std::unexpected(stopped_error(kRequestOperation));
  }

  auto parsed = parse_url(req.url);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed.error()));
  }

  const Deadline deadline = steady_clock::now() + req.timeout;
  OriginLease origin{*this, acquire_pool(*parsed)};
  OriginPool& pool = origin.pool();

  auto cancellation = std::make_shared<OperationCancel>();
  std::weak_ptr<OperationCancel> weak_cancellation = cancellation;
  std::stop_callback on_stop{
      stop,
      [weak_cancellation, executor = executor_]() noexcept {
        try {
          asio::post(executor, [weak_cancellation]() noexcept {
            if (auto operation = weak_cancellation.lock()) {
              try {
                operation->signal.emit(asio::cancellation_type::all);
              } catch (...) {
                // Cancellation handlers are terminal notification only.
              }
            }
          });
        } catch (...) {
          // Executor teardown is already terminal for this operation.
        }
      }};

  if (auto stopped = stop_before_operation(stop, kOriginAcquireOperation)) {
    co_return std::unexpected(std::move(*stopped));
  }
  auto [acquire_ec] = co_await pool.gate.async_send(
      boost::system::error_code{},
      io_token(cancellation->signal.slot(), deadline));
  if (acquire_ec) {
    co_return std::unexpected(
        phase_error(kOriginAcquireOperation, acquire_ec, stop, deadline));
  }
  GateLease origin_permit{pool.gate};

  if (auto stopped = stop_before_operation(stop, kGlobalAcquireOperation)) {
    co_return std::unexpected(std::move(*stopped));
  }
  auto [global_acquire_ec] = co_await request_gate_->gate.async_send(
      boost::system::error_code{},
      io_token(cancellation->signal.slot(), deadline));
  if (global_acquire_ec) {
    co_return std::unexpected(phase_error(
        kGlobalAcquireOperation,
        global_acquire_ec,
        stop,
        deadline));
  }
  GateLease global_permit{request_gate_->gate};

  ConnectionLease lease{*this, pool, take_idle(pool)};
  if (lease.get() == nullptr) {
    auto connected = co_await connect(
        *parsed,
        cancellation->signal.slot(),
        stop,
        deadline);
    if (!connected) {
      co_return std::unexpected(std::move(connected.error()));
    }
    lease.reset(std::move(*connected));
  }

  co_return co_await exchange(
      *lease.get(),
      *parsed,
      std::move(req),
      cancellation->signal.slot(),
      stop,
      deadline);
}

Result<ParsedUrl> parse_url(std::string_view url) {
  if (url.empty()) {
    return std::unexpected(Error{
        "http.request: empty url",
        "http.request",
        0});
  }

  auto parsed = ada::parse<ada::url_aggregator>(url);
  if (!parsed) {
    return std::unexpected(Error{
        "http.request: invalid url",
        "http.request",
        0});
  }

  const auto protocol = parsed->get_protocol();
  std::string scheme;
  scheme.reserve(protocol.size());
  for (char character : protocol) {
    if (character == ':') {
      break;
    }
    scheme.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(character))));
  }
  if (scheme != "http" && scheme != "https") {
    return std::unexpected(Error{
        "http.request: only http and https schemes are supported",
        "http.request",
        0});
  }
  if (parsed->has_credentials()) {
    return std::unexpected(Error{
        "http.request: userinfo in url is not supported",
        "http.request",
        0});
  }

  ParsedUrl result;
  result.scheme = std::move(scheme);
  result.host = std::string{parsed->get_hostname()};
  result.host_is_ip_literal =
      parsed->host_type != ada::url_host_type::DEFAULT;
  result.host_is_ipv6 = parsed->host_type == ada::url_host_type::IPV6;
  if (result.host_is_ipv6 && result.host.size() >= 2 &&
      result.host.front() == '[' && result.host.back() == ']') {
    result.host.erase(result.host.begin());
    result.host.pop_back();
  }
  if (result.host.empty()) {
    return std::unexpected(Error{
        "http.request: missing host",
        "http.request",
        0});
  }

  const auto port = parsed->get_port();
  if (port.empty()) {
    result.port = result.scheme == "https" ? "443" : "80";
  } else {
    unsigned long number = 0;
    const char* begin = port.data();
    const char* end = port.data() + port.size();
    auto [position, ec] = std::from_chars(begin, end, number);
    if (ec != std::errc{} || position != end || number == 0 ||
        number > 65535) {
      return std::unexpected(Error{
          "http.request: invalid port",
          "http.request",
          0});
    }
    result.port = std::to_string(number);
  }

  const auto path = parsed->get_pathname();
  const auto query = parsed->get_search();
  result.target = path.empty() ? std::string{"/"} : std::string{path};
  result.target.append(query);
  if (result.target.empty()) {
    result.target = "/";
  }
  return result;
}

Result<std::string> resolve_ca_bundle(std::string_view explicit_path) {
  namespace fs = std::filesystem;
  auto regular_file = [](std::string_view path) -> std::optional<std::string> {
    if (path.empty()) {
      return std::nullopt;
    }
    std::error_code ec;
    if (fs::is_regular_file(fs::path{std::string{path}}, ec)) {
      return std::string{path};
    }
    return std::nullopt;
  };

  if (!explicit_path.empty()) {
    if (auto path = regular_file(explicit_path)) {
      return *path;
    }
    return std::unexpected(Error{
        std::format("http.request: CA bundle not a file: {}", explicit_path),
        "http.request.tls",
        0});
  }

  static constexpr const char* kDefaultPaths[] = {
      "/etc/vacps/ca-bundle.pem",
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/ssl/cert.pem",
      "/etc/pki/tls/certs/ca-bundle.crt",
  };
  for (const char* path : kDefaultPaths) {
    if (auto found = regular_file(path)) {
      return *found;
    }
  }
  return std::unexpected(Error{
      "http.request: no CA bundle found "
      "(inject ca_bundle or install ca-certificates)",
      "http.request.tls",
      0});
}

}  // namespace vacps::http
