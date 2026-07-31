#include "http/client.hpp"

#include "app/log.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <gtest/gtest.h>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

vacps::Result<vacps::http::ClientResponse> sync_request(vacps::http::ClientRequest req) {
  std::optional<vacps::Result<vacps::http::ClientResponse>> out;
  asio::io_context ioc{1};
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        out = co_await vacps::http::async_request(std::move(req));
        co_return;
      },
      asio::detached);
  ioc.run();
  return std::move(*out);
}

}  // namespace

TEST(HttpClientUrlTest, ParsesHttpsDefaultPort) {
  auto u = vacps::http::parse_url("https://example.com/path?q=1");
  ASSERT_TRUE(u) << u.error().message;
  EXPECT_EQ(u->scheme, "https");
  EXPECT_EQ(u->host, "example.com");
  EXPECT_EQ(u->port, "443");
  EXPECT_EQ(u->target, "/path?q=1");
}

TEST(HttpClientUrlTest, ParsesHttpExplicitPort) {
  auto u = vacps::http::parse_url("http://127.0.0.1:9876/");
  ASSERT_TRUE(u) << u.error().message;
  EXPECT_EQ(u->scheme, "http");
  EXPECT_EQ(u->host, "127.0.0.1");
  EXPECT_EQ(u->port, "9876");
  EXPECT_EQ(u->target, "/");
}

TEST(HttpClientUrlTest, QueryWithSlashDoesNotCorruptHost) {
  // Handwritten parser treated '?'... as path start wrongly when '/' appears in query.
  auto u = vacps::http::parse_url("https://example.com?next=/admin");
  ASSERT_TRUE(u) << u.error().message;
  EXPECT_EQ(u->host, "example.com");
  EXPECT_EQ(u->port, "443");
  EXPECT_EQ(u->target, "/?next=/admin");
}

TEST(HttpClientUrlTest, DropsFragment) {
  auto u = vacps::http::parse_url("https://example.com/a#section");
  ASSERT_TRUE(u) << u.error().message;
  EXPECT_EQ(u->target, "/a");
}

TEST(HttpClientUrlTest, RejectsUserinfo) {
  auto u = vacps::http::parse_url("https://user:pass@example.com/");
  ASSERT_FALSE(u);
  EXPECT_NE(u.error().message.find("userinfo"), std::string::npos) << u.error().message;
}

TEST(HttpClientUrlTest, RejectsNoScheme) {
  EXPECT_FALSE(vacps::http::parse_url("example.com/x"));
}

TEST(HttpClientUrlTest, RejectsFtp) {
  EXPECT_FALSE(vacps::http::parse_url("ftp://example.com/"));
}

/**
 * Local plain HTTP server (no TLS) + client round-trip on same ioc.
 */
TEST(HttpClientTest, LocalHttpGet) {
  vacps::log::init("off");
  asio::io_context ioc{1};

  // Bind ephemeral port
  tcp::acceptor acceptor{ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0}};
  const auto port = acceptor.local_endpoint().port();

  bool server_done = false;
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        auto [ec, sock] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) co_return;
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> req;
        auto [rec, rn] = co_await boost::beast::http::async_read(
            sock, buffer, req, asio::as_tuple(asio::use_awaitable));
        (void)rn;
        if (rec) co_return;
        boost::beast::http::response<boost::beast::http::string_body> res{
            boost::beast::http::status::ok, req.version()};
        res.set(boost::beast::http::field::content_type, "text/plain");
        res.body() = "hello-client";
        res.prepare_payload();
        co_await boost::beast::http::async_write(sock, res, asio::as_tuple(asio::use_awaitable));
        boost::system::error_code sec;
        sock.shutdown(tcp::socket::shutdown_both, sec);
        server_done = true;
        co_return;
      },
      asio::detached);

  vacps::Result<vacps::http::ClientResponse> result =
      std::unexpected(vacps::Error{"not run"});
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::http::ClientRequest req;
        req.method = "GET";
        req.url = std::format("http://127.0.0.1:{}/ping", port);
        req.timeout_ms = 5000;
        result = co_await vacps::http::async_request(std::move(req));
        co_return;
      },
      asio::detached);

  ioc.run();

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result->status, 200);
  EXPECT_EQ(result->body, "hello-client");
  EXPECT_TRUE(server_done);
}

TEST(HttpClientTest, LocalHttpPostBody) {
  vacps::log::init("off");
  asio::io_context ioc{1};
  tcp::acceptor acceptor{ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0}};
  const auto port = acceptor.local_endpoint().port();

  std::string seen_body;
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        auto [ec, sock] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) co_return;
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> req;
        co_await boost::beast::http::async_read(
            sock, buffer, req, asio::as_tuple(asio::use_awaitable));
        seen_body = req.body();
        boost::beast::http::response<boost::beast::http::string_body> res{
            boost::beast::http::status::created, 11};
        res.body() = "ok";
        res.prepare_payload();
        co_await boost::beast::http::async_write(sock, res, asio::as_tuple(asio::use_awaitable));
        co_return;
      },
      asio::detached);

  vacps::Result<vacps::http::ClientResponse> result =
      std::unexpected(vacps::Error{"not run"});
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::http::ClientRequest req;
        req.method = "POST";
        req.url = std::format("http://127.0.0.1:{}/", port);
        req.body = "payload-xyz";
        req.headers.emplace_back("content-type", "text/plain");
        req.timeout_ms = 5000;
        result = co_await vacps::http::async_request(std::move(req));
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result->status, 201);
  EXPECT_EQ(seen_body, "payload-xyz");
  EXPECT_EQ(result->body, "ok");
}

TEST(HttpClientTest, BodyLimitEnforcedDuringRead) {
  vacps::log::init("off");
  asio::io_context ioc{1};
  tcp::acceptor acceptor{ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0}};
  const auto port = acceptor.local_endpoint().port();

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        auto [ec, sock] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) co_return;
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> req;
        co_await boost::beast::http::async_read(
            sock, buffer, req, asio::as_tuple(asio::use_awaitable));
        boost::beast::http::response<boost::beast::http::string_body> res{
            boost::beast::http::status::ok, 11};
        // 64-byte body; client caps at 16.
        res.body() = std::string(64, 'Z');
        res.prepare_payload();
        co_await boost::beast::http::async_write(sock, res, asio::as_tuple(asio::use_awaitable));
        co_return;
      },
      asio::detached);

  vacps::Result<vacps::http::ClientResponse> result =
      std::unexpected(vacps::Error{"not run"});
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::http::ClientRequest req;
        req.method = "GET";
        req.url = std::format("http://127.0.0.1:{}/big", port);
        req.timeout_ms = 5000;
        req.max_response_bytes = 16;
        result = co_await vacps::http::async_request(std::move(req));
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_FALSE(result) << "expected body_limit failure";
  EXPECT_NE(result.error().message.find("maxResponseBytes"), std::string::npos)
      << result.error().message;
}
