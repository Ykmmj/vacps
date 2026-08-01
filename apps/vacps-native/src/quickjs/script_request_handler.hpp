#pragma once

/**
 * IRequestHandler adapter that dispatches HTTP requests into the business
 * script via ScriptRuntime::invoke_export("handleRequest").
 */

#include "http/request_handler.hpp"
#include "quickjs/script_runtime.hpp"

#include <memory>

namespace vacps::js {

/**
 * Adapts IRequestHandler → ScriptRuntime without owning the runtime.
 *
 * Holds weak_ptr to break the reference cycle:
 *   JS Server → shared_ptr<http::Server> → shared_ptr<IRequestHandler>
 *   → ScriptRequestHandler → (was shared_ptr) ScriptRuntime → JSContext → JS Server
 */
class ScriptRequestHandler final : public vacps::http::IRequestHandler {
 public:
  /** Stores a non-owning weak reference; construction from shared_ptr is fine. */
  explicit ScriptRequestHandler(std::weak_ptr<ScriptRuntime> host);

  [[nodiscard]] asio::awaitable<vacps::Result<vacps::http::HttpResponse>> handle(
      vacps::http::HttpRequest req) override;

 private:
  std::weak_ptr<ScriptRuntime> host_;
};

}  // namespace vacps::js
