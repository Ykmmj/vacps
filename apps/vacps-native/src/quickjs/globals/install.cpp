#include "quickjs/globals/install.hpp"

#include "quickjs/globals/text_decoder_binding.hpp"
#include "quickjs/globals/text_encoder_binding.hpp"
#include "quickjs/globals/url_binding.hpp"
#include "quickjs/globals/url_search_params_binding.hpp"

namespace vacps::js {

VoidResult install_global_apis(JSContext* ctx) {
  if (ctx == nullptr) {
    return std::unexpected(Error{"install_global_apis: null context"});
  }
  // URLSearchParams first so URL.searchParams can construct instances.
  if (auto sp = install_url_search_params_binding(ctx); !sp) {
    return sp;
  }
  if (auto url = install_url_binding(ctx); !url) {
    return url;
  }
  if (auto enc = install_text_encoder_binding(ctx); !enc) {
    return enc;
  }
  if (auto dec = install_text_decoder_binding(ctx); !dec) {
    return dec;
  }
  return {};
}

}  // namespace vacps::js
