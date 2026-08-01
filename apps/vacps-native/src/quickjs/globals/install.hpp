#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install globalThis APIs that are not ES modules (URL, TextEncoder, …).
 * Called from install_modules / install_default_modules (composition root).
 */
[[nodiscard]] VoidResult install_global_apis(JSContext* ctx);

}  // namespace vacps::js
