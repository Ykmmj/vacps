#pragma once

#include "app/error.hpp"

#include <quickjs.h>

namespace vacps::js {

/**
 * Install WHATWG Encoding API globalThis.TextDecoder.
 * Binding uses vacps::text::Decoder only (simdutf inside domain).
 *
 * Pure synchronous global: Env is constructed from the live JSContext alone.
 */
[[nodiscard]] VoidResult install_text_decoder_binding(JSContext* ctx);

}  // namespace vacps::js
