#pragma once

#include "app/error.hpp"
#include "url/search_params.hpp"

#include <quickjs.h>

#include <memory>

namespace vacps::js {

/**
 * Install WHATWG-compatible globalThis.URLSearchParams
 * backed by vacps::url::SearchParams (Ada only inside domain).
 *
 * Pure synchronous global: Env is constructed from the live JSContext alone.
 */
[[nodiscard]] VoidResult install_url_search_params_binding(JSContext* ctx);

/**
 * Wrap a SearchParams (standalone or Url-attached live bag) as JS URLSearchParams.
 * Prefer sharing the same std::shared_ptr so Url.searchParams stays one object.
 */
JSValue make_search_params(
    JSContext* ctx,
    std::shared_ptr<vacps::url::SearchParams> params);

/** Convenience: wrap an owned snapshot / standalone instance. */
inline JSValue make_search_params(
    JSContext* ctx,
    vacps::url::SearchParams params) {
  return make_search_params(
      ctx, std::make_shared<vacps::url::SearchParams>(std::move(params)));
}

}  // namespace vacps::js
