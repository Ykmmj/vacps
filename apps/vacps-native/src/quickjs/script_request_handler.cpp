#include "quickjs/script_request_handler.hpp"

#include "http/script_dispatch.hpp"

namespace vacps::js {

ScriptRequestHandler::ScriptRequestHandler(std::weak_ptr<ScriptRuntime> host)
    : host_(std::move(host)) {}

asio::awaitable<vacps::Result<vacps::http::HttpResponse>>
ScriptRequestHandler::handle(vacps::http::HttpRequest req) {
  auto host = host_.lock();
  // Expired (runtime destroyed) or shutting down → 503 via session mapping.
  if (!host || host->closing()) {
    co_return std::unexpected(vacps::Error{"business script not ready"});
  }
  co_return co_await vacps::http::dispatch_to_script(*host, std::move(req));
}

}  // namespace vacps::js
