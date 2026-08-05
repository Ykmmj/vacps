#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install globalThis APIs that are not ES modules (URL, TextEncoder, …).
 * Called from Application::initialize (composition root).
 * Pure synchronous globals construct Env from the live JSContext alone.
 */
[[nodiscard]] VoidResult install_global_apis(JSContext* ctx);

}  // namespace vacps::js
