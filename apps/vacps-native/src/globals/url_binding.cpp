#include "globals/url_binding.hpp"

#include "binding/class.hpp"
#include "binding/coerce.hpp"
#include "qjs/owned_value.hpp"
#include "globals/url_search_params_binding.hpp"
#include "url/url.hpp"

#include <quickjs.h>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;

/** Hidden own-property: live URLSearchParams identity (GC-managed, not C++ JSValue). */
constexpr const char kSearchParamsSlot[] = "\xff\xffvacps_url_searchParams";

using UrlBuilder = binding::ClassBuilder<vacps::url::Url>;

binding::VoidResult install_url_class(binding::Env env) {
  UrlBuilder cls{env, "URL"};

  cls.constructor(
      [](const binding::CallbackInfo& info)
          -> binding::Result<std::shared_ptr<vacps::url::Url>> {
        if (info.length() < 1) {
          return std::unexpected(binding::Error::type(
              "URL constructor requires at least 1 argument"));
        }
        auto input = binding::try_coerce_string(info.context(), info[0].get());
        if (!input) {
          return std::unexpected(binding::Error::type("Invalid URL"));
        }

        std::optional<std::string> base;
        if (info.length() >= 2 && !info[1].is_nullish()) {
          base = binding::try_coerce_string(info.context(), info[1].get());
          if (!base) {
            return std::unexpected(binding::Error::type("Invalid base URL"));
          }
        }

        auto parsed = base ? vacps::url::Url::parse(*input, *base)
                           : vacps::url::Url::parse(*input);
        if (!parsed) {
          // Domain messages are already "Invalid URL" / "Invalid base URL".
          // Map to TypeError (from_domain would be InternalError).
          const auto& msg = parsed.error().message;
          if (msg.find("base") != std::string::npos) {
            return std::unexpected(binding::Error::type("Invalid base URL"));
          }
          return std::unexpected(binding::Error::type("Invalid URL"));
        }
        try {
          return std::make_shared<vacps::url::Url>(std::move(*parsed));
        } catch (const std::bad_alloc&) {
          return std::unexpected(binding::Error::internal("allocation failed"));
        }
      },
      1);

  auto string_getter = [](auto method) {
    return [method](vacps::url::Url& self) -> std::string {
      return std::string((self.*method)());
    };
  };

  cls.readonly("href", string_getter(&vacps::url::Url::href));
  cls.readonly("protocol", string_getter(&vacps::url::Url::protocol));
  cls.readonly("username", string_getter(&vacps::url::Url::username));
  cls.readonly("password", string_getter(&vacps::url::Url::password));
  cls.readonly("hostname", string_getter(&vacps::url::Url::hostname));
  cls.readonly("host", string_getter(&vacps::url::Url::host));
  cls.readonly("pathname", string_getter(&vacps::url::Url::pathname));
  cls.readonly("hash", string_getter(&vacps::url::Url::hash));
  cls.readonly("port", string_getter(&vacps::url::Url::port));
  cls.readonly("origin", [](vacps::url::Url& self) { return self.origin(); });

  cls.accessor(
      "search",
      [](vacps::url::Url& self) { return std::string(self.search()); },
      [](const binding::CallbackInfo& info,
         vacps::url::Url& self) -> binding::VoidResult {
        std::string value;
        if (info.length() >= 1) {
          auto s = binding::try_coerce_string(info.context(), info[0].get());
          if (!s) {
            return std::unexpected(
                binding::Error::type("URL.search must be a string"));
          }
          value = std::move(*s);
        }
        self.set_search(value);
        return {};
      });

  /**
   * Live searchParams: shared_ptr from Url::search_params(); JS wrapper
   * identity cached in a hidden own property on the JS URL object (never a
   * C++-held JSValue).
   */
  cls.readonly(
      "searchParams",
      [](const binding::CallbackInfo& info,
         vacps::url::Url& self) -> qjs::OwnedValue {
        JSContext* c = info.context();
        JSValueConst this_val = info.this_raw();

        // Ensure the live bag exists on the domain object.
        (void)self.search_params();

        JSValue existing =
            JS_GetPropertyStr(c, this_val, kSearchParamsSlot);
        if (JS_IsException(existing)) {
          return qjs::OwnedValue::take(c, existing);
        }
        if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
          return qjs::OwnedValue::take(c, existing);
        }
        JS_FreeValue(c, existing);

        auto live = self.search_params();
        // Same ClassBuilder storage/class id as new URLSearchParams.
        qjs::OwnedValue sp{c, make_search_params(c, std::move(live))};
        if (sp.is_exception()) {
          return sp;
        }

        // Non-enumerable / non-writable slot; Define takes one ref.
        if (JS_DefinePropertyValueStr(
                c,
                this_val,
                kSearchParamsSlot,
                JS_DupValue(c, sp.get()),
                0) < 0) {
          // Define failed: drop the dup if still owned by the engine path.
          binding::clear_exception(c);
          return qjs::OwnedValue::take(
              c, binding::throw_internal(c, "URL.searchParams cache set"));
        }
        return sp;
      });

  cls.method(
      "toString",
      [](vacps::url::Url& self) { return std::string(self.href()); },
      0);
  cls.method(
      "toJSON",
      [](vacps::url::Url& self) { return std::string(self.href()); },
      0);

  cls.static_function(
      "canParse",
      [](const binding::CallbackInfo& info) -> bool {
        if (info.length() < 1) {
          return false;
        }
        auto input = binding::try_coerce_string(info.context(), info[0].get());
        if (!input) {
          return false;
        }
        if (info.length() >= 2 && !info[1].is_nullish()) {
          auto base = binding::try_coerce_string(info.context(), info[1].get());
          if (!base) {
            return false;
          }
          return vacps::url::Url::can_parse(*input, *base);
        }
        return vacps::url::Url::can_parse(*input);
      },
      1);

  auto ctor = cls.commit();
  if (!ctor) {
    return std::unexpected(ctor.error());
  }

  // Escape hatch: install constructor on globalThis.
  JSContext* c = env.context();
  JSValue global = JS_GetGlobalObject(c);
  if (JS_IsException(global)) {
    binding::clear_exception(c);
    ctor->reset();
    return std::unexpected(binding::Error::internal("URL GetGlobalObject"));
  }
  JSValue raw_ctor = ctor->release();
  if (JS_DefinePropertyValueStr(
          c,
          global,
          "URL",
          raw_ctor,
          JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    binding::clear_exception(c);
    JS_FreeValue(c, global);
    return std::unexpected(binding::Error::internal("URL global define"));
  }
  JS_FreeValue(c, global);
  return {};
}

}  // namespace

VoidResult install_url_binding(JSContext* ctx) {
  try {
    binding::Env env{ctx};
    if (auto r = install_url_class(env); !r) {
      if (JS_HasException(ctx)) {
        binding::clear_exception(ctx);
      }
      return std::unexpected(Error{std::move(r.error().message)});
    }
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
      return std::unexpected(Error{"install_url_binding: pending exception"});
    }
    return {};
  } catch (const std::exception& ex) {
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
    }
    return std::unexpected(Error{ex.what()});
  } catch (...) {
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
    }
    return std::unexpected(Error{"install_url_binding: unknown failure"});
  }
}

}  // namespace vacps::js
