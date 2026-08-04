#pragma once

#include "app/error.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::http {

namespace asio = boost::asio;

struct ParsedUrl {
  std::string scheme;  // "http" | "https"
  std::string host;
  std::string port;    // numeric string
  std::string target;  // path + query, at least "/"
};

/**
 * Parse absolute http(s) URL via Ada (WHATWG). No userinfo.
 * Fragment is dropped from target; path+query form the request target.
 */
[[nodiscard]] Result<ParsedUrl> parse_url(std::string_view url);

struct ClientRequest {
  std::string method{"GET"};
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
  /** Must be > 0. Default 30s. One absolute wall budget for the whole request. */
  std::chrono::milliseconds timeout{30'000};
  /**
   * Max response body size enforced during read via Beast body_limit.
   * Default 8 MiB. Oversized bodies fail without buffering the whole payload.
   */
  std::size_t max_response_bytes{8u * 1024u * 1024u};
  /**
   * PEM CA bundle for HTTPS (injected from bootstrap / host config).
   * Empty → platform default paths only (no getenv at request time).
   * Missing CA for https is fail-closed (no verify skip).
   */
  std::string ca_bundle;
};

struct ClientResponse {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
};

/**
 * One-shot HTTP/HTTPS request on the caller's executor (host io_context).
 * HTTPS: verify_peer + SNI + host_name_verification; TLS ≥ 1.2.
 * Does not follow redirects (caller sees 3xx as-is).
 *
 * @param stop External cancellation; stop_callback posts onto the request
 *        executor before emitting Asio cancellation (never emits on a foreign
 *        thread). Runtime/shutdown cancel → ECANCELED; wall timeout → ETIMEDOUT.
 */
[[nodiscard]] asio::awaitable<Result<ClientResponse>> async_request(
    std::stop_token stop,
    ClientRequest req);

/** Resolve CA path: explicit file path → well-known platform defaults. No getenv. */
[[nodiscard]] Result<std::string> resolve_ca_bundle(std::string_view explicit_path);

}  // namespace vacps::http
