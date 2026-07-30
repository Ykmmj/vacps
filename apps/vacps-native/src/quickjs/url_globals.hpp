#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install WHATWG-compatible globalThis.URL backed by Ada 4.x.
 * Enables Zod z.url() and other code that expects the browser/Node URL API.
 */
[[nodiscard]] VoidResult install_url_global(JSContext* ctx);

}  // namespace vacps::js
