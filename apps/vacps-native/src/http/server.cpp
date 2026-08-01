#include "http/server.hpp"

#include "app/log.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/error.hpp>

#include <algorithm>
#include <utility>

namespace vacps::http {
namespace beast = boost::beast;

Server::Server(
    asio::io_context& ioc,
    ListenEndpoint listen,
    std::shared_ptr<IRequestHandler> handler)
    : listen_(std::move(listen)),
      handler_(std::move(handler)),
      acceptor_(asio::make_strand(ioc)) {}

Server::~Server() {
  close();
}

VoidResult Server::start() {
  beast::error_code ec;
  tcp::endpoint ep(asio::ip::make_address(listen_.host, ec), listen_.port);
  if (ec) {
    return std::unexpected(Error{"invalid listen host: " + ec.message()});
  }
  acceptor_.open(ep.protocol(), ec);
  if (ec) {
    return std::unexpected(Error{"acceptor open: " + ec.message()});
  }
  acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
  acceptor_.bind(ep, ec);
  if (ec) {
    return std::unexpected(Error{"acceptor bind: " + ec.message()});
  }
  acceptor_.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    return std::unexpected(Error{"acceptor listen: " + ec.message()});
  }

  vacps::log::info("listening on http://{}:{}", listen_.host, listen_.port);

  auto self = shared_from_this();
  asio::co_spawn(
      acceptor_.get_executor(),
      [self]() -> asio::awaitable<void> { co_await self->accept_loop(); },
      [](std::exception_ptr ep) {
        if (!ep) return;
        try {
          std::rethrow_exception(ep);
        } catch (const std::exception& e) {
          vacps::log::error("accept loop: {}", e.what());
        }
      });

  return {};
}

void Server::close() noexcept {
  beast::error_code ignored;
  // 1. Stop accepting new connections (accept_loop co_returns on closed acceptor).
  acceptor_.close(ignored);
  // 2. Cancel/drain in-flight sessions (best-effort socket cancel + clear tracking).
  cancel_all_sessions();
}

void Server::session_started(std::shared_ptr<Session> session) noexcept {
  active_sessions_.fetch_add(1, std::memory_order_relaxed);
  // Prune expired weak_ptrs occasionally so the vector does not grow forever.
  if (sessions_.size() > 64) {
    sessions_.erase(
        std::remove_if(
            sessions_.begin(),
            sessions_.end(),
            [](const std::weak_ptr<Session>& w) { return w.expired(); }),
        sessions_.end());
  }
  sessions_.push_back(std::move(session));
}

void Server::session_finished() noexcept {
  active_sessions_.fetch_sub(1, std::memory_order_relaxed);
}

void Server::cancel_all_sessions() noexcept {
  for (auto& w : sessions_) {
    if (auto s = w.lock()) {
      s->cancel();
    }
  }
  sessions_.clear();
}

std::size_t Server::active_sessions() const noexcept {
  return active_sessions_.load(std::memory_order_relaxed);
}

bool Server::is_open() const noexcept {
  return acceptor_.is_open();
}

asio::awaitable<void> Server::accept_loop() {
  for (;;) {
    auto [ec, socket] =
        co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
    if (ec) {
      // acceptor closed / cancelled — transport stop only, not process shutdown
      co_return;
    }
    auto sess =
        std::make_shared<Session>(std::move(socket), handler_, weak_from_this());
    sess->run();
  }
}

}  // namespace vacps::http
