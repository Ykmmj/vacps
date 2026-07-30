#pragma once

#include "app/config.hpp"
#include "app/error.hpp"
#include "app/log.hpp"
#include "http/session.hpp"
#include "quickjs/host.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/error.hpp>

#include <csignal>
#include <memory>
#include <utility>

namespace vacps::http {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/** HTTP transport only — zero product routing. Lifetime: shared_ptr. */
class Server : public std::enable_shared_from_this<Server> {
 public:
  Server(asio::io_context& ioc, Config cfg, std::shared_ptr<vacps::js::Host> script)
      : ioc_(ioc),
        cfg_(std::move(cfg)),
        script_(std::move(script)),
        acceptor_(asio::make_strand(ioc)),
        signals_(ioc, SIGINT, SIGTERM) {}

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  [[nodiscard]] VoidResult start() {
    beast::error_code ec;
    tcp::endpoint ep(asio::ip::make_address(cfg_.listen_host, ec), cfg_.listen_port);
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

    vacps::log::info("listening on http://{}:{}", cfg_.listen_host, cfg_.listen_port);

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

    signals_.async_wait([self](beast::error_code, int signo) {
      vacps::log::info("signal {}, shutting down", signo);
      self->request_stop();
    });
    return {};
  }

  /** Close acceptor/signals only (does not stop io_context). */
  void close() noexcept {
    beast::error_code ignored;
    acceptor_.close(ignored);
    signals_.cancel(ignored);
  }

  /** Close acceptor and stop io_context (signal / process shutdown). */
  void request_stop() {
    close();
    ioc_.stop();
  }

  [[nodiscard]] bool is_open() const noexcept {
    return acceptor_.is_open();
  }

 private:
  asio::awaitable<void> accept_loop() {
    for (;;) {
      auto [ec, socket] =
          co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
      if (ec) {
        // closed / cancelled during shutdown
        co_return;
      }
      std::make_shared<Session>(std::move(socket), script_)->run();
    }
  }

  asio::io_context& ioc_;
  Config cfg_;
  std::shared_ptr<vacps::js::Host> script_;
  tcp::acceptor acceptor_;
  asio::signal_set signals_;
};

}  // namespace vacps::http
