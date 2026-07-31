#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install globalThis APIs that are not ES modules (URL, TextEncoder, …).
 * Called from install_modules during ScriptRuntime setup.
 */
[[nodiscard]] VoidResult install_global_apis(JSContext* ctx);

}  // namespace vacps::js
