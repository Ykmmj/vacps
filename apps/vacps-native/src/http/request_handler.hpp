#pragma once

/**
 * Application-facing request handler for the HTTP transport.
 * Server/Session depend only on this interface — not on QuickJS/ScriptRuntime.
 */

#include "app/error.hpp"
#include "http/types.hpp"

#include <boost/asio/awaitable.hpp>

namespace vacps::http {

namespace asio = boost::asio;

class IRequestHandler {
 public:
  virtual ~IRequestHandler() = default;

  [[nodiscard]] virtual asio::awaitable<Result<HttpResponse>> handle(
      HttpRequest req) = 0;
};

}  // namespace vacps::http
