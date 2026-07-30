#pragma once

/**
 * HTTP transport → script handleRequest mapping.
 * Owned by the http layer (Session uses this); not part of Host.
 */

#include "app/error.hpp"
#include "quickjs/host.hpp"

#include <boost/asio/awaitable.hpp>

#include <string>
#include <utility>
#include <vector>

namespace vacps::http {

namespace asio = boost::asio;

struct ScriptHttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
  std::string request_id;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct ScriptHttpResponse {
  int status{500};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

/** Build JS request, invoke_export("handleRequest"), map response. */
[[nodiscard]] asio::awaitable<Result<ScriptHttpResponse>> dispatch_to_script(
    vacps::js::Host& host,
    ScriptHttpRequest req);

}  // namespace vacps::http
