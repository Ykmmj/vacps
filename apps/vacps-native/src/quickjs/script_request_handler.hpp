#pragma once

/**
 * IRequestHandler adapter that dispatches HTTP requests into the business
 * script via ScriptRuntime::invoke_export("handleRequest").
 */

#include "http/request_handler.hpp"
#include "quickjs/script_runtime.hpp"

#include <memory>

namespace vacps::js {

class ScriptRequestHandler final : public vacps::http::IRequestHandler {
 public:
  explicit ScriptRequestHandler(std::shared_ptr<ScriptRuntime> host);

  [[nodiscard]] asio::awaitable<vacps::Result<vacps::http::HttpResponse>> handle(
      vacps::http::HttpRequest req) override;

 private:
  std::shared_ptr<ScriptRuntime> host_;
};

}  // namespace vacps::js
