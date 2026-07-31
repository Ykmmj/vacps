#pragma once

#include "app/error.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <string>
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
  std::string body;
  /** 0 = no timeout (not recommended). Default 30s. Applies to resolve+I/O. */
  std::int32_t timeout_ms{30'000};
  /**
   * Max response body size enforced during read via Beast body_limit.
   * Default 8 MiB. Oversized bodies fail without buffering the whole payload.
   */
  std::size_t max_response_bytes{8u * 1024u * 1024u};
  /**
   * PEM CA bundle for HTTPS. Empty → resolve via VACPS_CA_BUNDLE / platform defaults.
   * Missing CA for https is fail-closed (no verify skip).
   */
  std::string ca_bundle;
};

struct ClientResponse {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

/**
 * One-shot HTTP/HTTPS request on the caller's executor (host io_context).
 * HTTPS: verify_peer + SNI + host_name_verification; TLS ≥ 1.2.
 * Does not follow redirects (caller sees 3xx as-is).
 */
[[nodiscard]] asio::awaitable<Result<ClientResponse>> async_request(ClientRequest req);

/** Resolve CA path: explicit → env VACPS_CA_BUNDLE → well-known files that exist. */
[[nodiscard]] Result<std::string> resolve_ca_bundle(std::string_view explicit_path);

}  // namespace vacps::http
