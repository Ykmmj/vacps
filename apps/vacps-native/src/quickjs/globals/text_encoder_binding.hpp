#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install WHATWG Encoding API globalThis.TextEncoder.
 * Binding uses vacps::text::Encoder only (simdutf inside domain).
 */
[[nodiscard]] VoidResult install_text_encoder_binding(JSContext* ctx);

}  // namespace vacps::js
