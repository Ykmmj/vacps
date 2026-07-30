#pragma once

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace vacps::http {

namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;

/**
 * C++ HTTP has zero product routes (design: all business in script).
 * Only transport helpers live here.
 */

inline std::string bootstrap_unavailable_json(std::string_view message) {
  return json{
             {"error",
              {
                  {"code", "service_unavailable"},
                  {"message", message},
              }},
         }
      .dump();
}

inline std::string internal_error_json(std::string_view message) {
  return json{
             {"error",
              {
                  {"code", "internal_error"},
                  {"message", message},
              }},
         }
      .dump();
}

template <class Body>
http::response<http::string_body> make_response(
    const http::request<Body>& req,
    http::status status,
    std::string body,
    std::string_view content_type = "application/json; charset=utf-8") {
  http::response<http::string_body> res{status, req.version()};
  res.set(http::field::server, "vacps-agent");
  res.set(http::field::content_type, content_type);
  res.keep_alive(req.keep_alive());
  res.body() = std::move(body);
  res.prepare_payload();
  return res;
}

}  // namespace vacps::http
