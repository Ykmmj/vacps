#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install WHATWG-compatible globalThis.URL
 * backed by vacps::url::Url (Ada only inside domain).
 * Requires install_url_search_params_binding first (URL.searchParams).
 * Enables Zod z.url() and other code that expects the browser/Node URL API.
 */
[[nodiscard]] VoidResult install_url_binding(JSContext* ctx);

}  // namespace vacps::js
