#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install WHATWG Encoding API globals: TextEncoder / TextDecoder.
 * Transcoding uses simdutf 9.x (not a hand-rolled UTF-8 codec).
 */
[[nodiscard]] VoidResult install_encoding_globals(JSContext* ctx);

}  // namespace vacps::js
