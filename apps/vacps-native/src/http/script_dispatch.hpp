#pragma once

/**
 * HTTP transport → script handleRequest mapping.
 * Used by ScriptRequestHandler (and unit tests that invoke script directly).
 * Server/Session no longer depend on this — they use IRequestHandler.
 */

#include "app/error.hpp"
#include "http/types.hpp"
#include "quickjs/script_runtime.hpp"

#include <boost/asio/awaitable.hpp>

namespace vacps::http {

namespace asio = boost::asio;

/** Build JS request, invoke_export("handleRequest"), map response. */
[[nodiscard]] asio::awaitable<Result<HttpResponse>> dispatch_to_script(
    vacps::js::ScriptRuntime& runtime,
    HttpRequest req);

}  // namespace vacps::http
