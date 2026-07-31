#include "quickjs/script_request_handler.hpp"

#include "http/script_dispatch.hpp"

namespace vacps::js {

ScriptRequestHandler::ScriptRequestHandler(std::shared_ptr<ScriptRuntime> host)
    : host_(std::move(host)) {}

asio::awaitable<vacps::Result<vacps::http::HttpResponse>>
ScriptRequestHandler::handle(vacps::http::HttpRequest req) {
  if (!host_) {
    co_return std::unexpected(vacps::Error{"business script not ready"});
  }
  co_return co_await vacps::http::dispatch_to_script(*host_, std::move(req));
}

}  // namespace vacps::js
