#include "http/server.hpp"

#include "app/log.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/error.hpp>

#include <algorithm>
#include <csignal>
#include <utility>

namespace vacps::http {
namespace beast = boost::beast;

Server::Server(
    asio::io_context& ioc,
    ListenEndpoint listen,
    std::shared_ptr<vacps::js::Host> script)
    : ioc_(ioc),
      listen_(std::move(listen)),
      script_(std::move(script)),
      acceptor_(asio::make_strand(ioc)),
      signals_(ioc, SIGINT, SIGTERM) {}

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

  signals_.async_wait([self](beast::error_code ec, int signo) {
    // cancel() from close() also completes async_wait — must not treat as SIGINT.
    if (ec) {
      return;
    }
    vacps::log::info("signal {}, graceful shutdown", signo);
    self->request_stop();
  });
  return {};
}

void Server::close() noexcept {
  beast::error_code ignored;
  acceptor_.close(ignored);
  signals_.cancel(ignored);
}

void Server::request_stop() {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true)) {
    return;
  }
  close();
  cancel_all_sessions();
  auto self = shared_from_this();
  asio::co_spawn(
      ioc_,
      [self]() -> asio::awaitable<void> { co_await self->graceful_shutdown(); },
      [](std::exception_ptr ep) {
        if (!ep) return;
        try {
          std::rethrow_exception(ep);
        } catch (const std::exception& e) {
          vacps::log::error("graceful shutdown: {}", e.what());
        }
      });
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

bool Server::stopping() const noexcept {
  return stopping_.load(std::memory_order_relaxed);
}

asio::awaitable<void> Server::accept_loop() {
  for (;;) {
    auto [ec, socket] =
        co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
    if (ec) {
      // closed / cancelled during shutdown
      co_return;
    }
    if (stopping_.load(std::memory_order_relaxed)) {
      beast::error_code ignored;
      socket.close(ignored);
      co_return;
    }
    auto sess =
        std::make_shared<Session>(std::move(socket), script_, weak_from_this());
    sess->run();
  }
}

asio::awaitable<void> Server::graceful_shutdown() {
  auto executor = co_await asio::this_coro::executor;
  // Re-cancel in case new sessions slipped in between close and this coroutine.
  cancel_all_sessions();

  const auto deadline = std::chrono::steady_clock::now() + kGracefulSessionDrain;
  while (active_sessions_.load(std::memory_order_relaxed) > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    asio::steady_timer tick{executor};
    tick.expires_after(std::chrono::milliseconds(50));
    auto [ec] = co_await tick.async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
  }
  const auto left = active_sessions_.load(std::memory_order_relaxed);
  if (left > 0) {
    vacps::log::warn(
        "graceful shutdown: {} session(s) still active after {}s; continuing",
        left,
        kGracefulSessionDrain.count());
  }

  if (script_) {
    // Do NOT cancel_host_async before JS shutdown — that makes await_settled
    // busy-spin and blocks db_pool completions needed by await db.close().
    script_->processes().shutdown();
    co_await script_->wait_async_idle(
        std::chrono::duration_cast<std::chrono::milliseconds>(kGracefulAsyncIdle));
    if (auto sh = co_await script_->shutdown_script(); !sh) {
      vacps::log::error("script shutdown: {}", sh.error().message);
    }
    // Drain ops started by JS shutdown (e.g. store.close), then mark done.
    co_await script_->wait_async_idle(
        std::chrono::duration_cast<std::chrono::milliseconds>(kGracefulAsyncIdle));
    script_->cancel_host_async();
    if (auto drain = script_->drain_jobs(); !drain) {
      vacps::log::debug("post-shutdown job drain: {}", drain.error().message);
    }
  }

  vacps::log::info("graceful shutdown complete; stopping io_context");
  ioc_.stop();
  co_return;
}

}  // namespace vacps::http
