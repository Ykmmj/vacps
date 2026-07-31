#pragma once

/**
 * HTTP transport DTOs — independent of QuickJS / product routing.
 */

#include <string>
#include <utility>
#include <vector>

namespace vacps::http {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
  std::string request_id;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse {
  int status{500};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

}  // namespace vacps::http
