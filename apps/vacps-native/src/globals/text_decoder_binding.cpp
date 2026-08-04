#include "globals/text_decoder_binding.hpp"

#include "binding/class.hpp"
#include "binding/coerce.hpp"
#include "qjs/owned_value.hpp"
#include "text/decoder.hpp"

#include <quickjs.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vacps::js {
namespace {

namespace binding = vacps::binding;

using DecoderBuilder = binding::ClassBuilder<vacps::text::Decoder>;

/** Read optional own/inherited boolean property; leave default when absent/non-bool. */
void read_optional_bool_prop(
    JSContext* ctx,
    JSValueConst obj,
    const char* name,
    bool& out) {
  qjs::OwnedValue v{ctx, JS_GetPropertyStr(ctx, obj, name)};
  if (v.is_exception()) {
    binding::clear_exception(ctx);
    (void)v.release();
    return;
  }
  if (v.is_undefined() || v.is_null() || !JS_IsBool(v.get())) {
    return;
  }
  const int b = JS_ToBool(ctx, v.get());
  if (b < 0) {
    binding::clear_exception(ctx);
    return;
  }
  out = b != 0;
}

binding::VoidResult install_text_decoder_class(binding::Env env) {
  DecoderBuilder cls{env, "TextDecoder"};

  cls.constructor(
      [](const binding::CallbackInfo& info)
          -> binding::Result<std::shared_ptr<vacps::text::Decoder>> {
        JSContext* c = info.context();

        std::string label = "utf-8";
        if (info.length() >= 1 && !info[0].is_nullish()) {
          // non-nullish → WebIDL ToString (numbers/objects/etc.).
          auto s = binding::try_coerce_string(c, info[0].get());
          if (!s) {
            return std::unexpected(
                binding::Error::type("TextDecoder: label must be a string"));
          }
          label = std::move(*s);
        }

        vacps::text::DecoderOptions opts{};
        if (info.length() >= 2 && info[1].is_object()) {
          read_optional_bool_prop(c, info[1].get(), "fatal", opts.fatal);
          read_optional_bool_prop(
              c, info[1].get(), "ignoreBOM", opts.ignore_bom);
        }

        auto decoder = vacps::text::Decoder::create(label, opts);
        if (!decoder) {
          // Domain Error would map to InternalError; surface as TypeError.
          return std::unexpected(binding::Error::type(
              "TextDecoder: only utf-8 / utf-16le / utf-16be are supported"));
        }
        try {
          return std::make_shared<vacps::text::Decoder>(std::move(*decoder));
        } catch (const std::bad_alloc&) {
          return std::unexpected(binding::Error::internal("allocation failed"));
        }
      },
      1);

  cls.readonly(
      "encoding",
      [](vacps::text::Decoder& self) {
        return std::string{self.encoding()};
      });
  cls.readonly(
      "fatal", [](vacps::text::Decoder& self) { return self.fatal(); });
  cls.readonly(
      "ignoreBOM",
      [](vacps::text::Decoder& self) { return self.ignore_bom(); });

  cls.method(
      "decode",
      [](const binding::CallbackInfo& info,
         vacps::text::Decoder& self) -> binding::Result<std::string> {
        JSContext* c = info.context();

        std::vector<std::uint8_t> bytes;
        if (info.length() >= 1 && !info[0].is_nullish()) {
          auto b = info[0].as<std::vector<std::uint8_t>>();
          if (!b) {
            return std::unexpected(binding::Error::type(
                "TextDecoder.decode: expected ArrayBuffer, TypedArray, or "
                "string"));
          }
          bytes = std::move(*b);
        }

        bool stream = false;
        if (info.length() >= 2 && info[1].is_object()) {
          read_optional_bool_prop(c, info[1].get(), "stream", stream);
        }

        auto out = self.decode(bytes, stream);
        if (!out) {
          // Fatal decode / domain failures → TypeError (not InternalError).
          return std::unexpected(
              binding::Error::type(std::move(out.error().message)));
        }
        return std::move(*out);
      },
      1);

  auto ctor = cls.commit();
  if (!ctor) {
    return std::unexpected(ctor.error());
  }

  // Escape hatch: install constructor on globalThis (Define takes ownership).
  JSContext* c = env.context();
  JSValue global = JS_GetGlobalObject(c);
  if (JS_IsException(global)) {
    binding::clear_exception(c);
    ctor->reset();
    return std::unexpected(
        binding::Error::internal("TextDecoder GetGlobalObject"));
  }
  JSValue raw_ctor = ctor->release();
  if (JS_DefinePropertyValueStr(
          c,
          global,
          "TextDecoder",
          raw_ctor,
          JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    binding::clear_exception(c);
    JS_FreeValue(c, global);
    return std::unexpected(
        binding::Error::internal("TextDecoder global define"));
  }
  JS_FreeValue(c, global);
  return {};
}

}  // namespace

VoidResult install_text_decoder_binding(JSContext* ctx) {
  try {
    binding::Env env{ctx};
    if (auto r = install_text_decoder_class(env); !r) {
      if (JS_HasException(ctx)) {
        binding::clear_exception(ctx);
      }
      return std::unexpected(Error{std::move(r.error().message)});
    }
    if (JS_HasException(ctx)) {
      binding::clear_exception(ctx);
      return std::unexpected(
          Error{"install_text_decoder_binding: pending exception"});
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
    return std::unexpected(
        Error{"install_text_decoder_binding: unknown failure"});
  }
}

}  // namespace vacps::js
