#include "http/script_dispatch.hpp"

#include "quickjs/raii/atom.hpp"
#include "quickjs/raii/convert.hpp"
#include "quickjs/raii/value.hpp"

#include <quickjs.h>

namespace vacps::http {

asio::awaitable<Result<HttpResponse>> dispatch_to_script(
    vacps::js::ScriptRuntime& runtime,
    HttpRequest req) {
  if (!runtime.script_ready()) {
    co_return std::unexpected(Error{"business script not ready"});
  }
  auto* ctx = runtime.context().get();
  if (ctx == nullptr) {
    co_return std::unexpected(Error{"no js context"});
  }

  using vacps::js::PropertyEnumList;
  using vacps::js::Value;

  auto obj = Value::new_object(ctx);
  if (obj.is_exception()) {
    co_return std::unexpected(runtime.context().take_exception_error());
  }
  obj.set_property_str("method", Value::new_string(ctx, req.method));
  obj.set_property_str("path", Value::new_string(ctx, req.path));
  obj.set_property_str("query", Value::new_string(ctx, req.query));
  obj.set_property_str("body", Value::new_string(ctx, req.body));
  obj.set_property_str("requestId", Value::new_string(ctx, req.request_id));

  auto headers = Value::new_object(ctx);
  for (const auto& [k, v] : req.headers) {
    headers.set_property_str(k.c_str(), Value::new_string(ctx, v));
  }
  obj.set_property_str("headers", std::move(headers));

  JSValueConst argv[1] = {obj.get()};
  auto ret = co_await runtime.invoke_export("handleRequest", 1, argv);
  // obj still owned until end of scope after invoke (argv borrows)
  if (!ret) {
    co_return std::unexpected(std::move(ret.error()));
  }

  if (!ret->is_object()) {
    co_return std::unexpected(Error{"handleRequest must return an object"});
  }

  HttpResponse out;
  {
    Value st = Value::get_property_str(ctx, ret->get(), "status");
    if (st.is_nullish()) {
      co_return std::unexpected(Error{"handleRequest response missing status"});
    }
    auto status_r = vacps::js::converter<std::int32_t>::from_js(ctx, st.get());
    if (!status_r) {
      co_return std::unexpected(Error{"handleRequest status is not an integer"});
    }
    if (*status_r < 100 || *status_r > 599) {
      co_return std::unexpected(Error{"handleRequest status out of range 100..599"});
    }
    out.status = *status_r;
  }

  {
    Value body = Value::get_property_str(ctx, ret->get(), "body");
    auto body_s = vacps::js::converter<std::string>::from_js(ctx, body.get());
    if (body_s) {
      out.body = std::move(*body_s);
    } else {
      out.body.clear();
    }
  }

  {
    Value hdrs = Value::get_property_str(ctx, ret->get(), "headers");
    if (hdrs.is_object()) {
      auto names = PropertyEnumList::get_own(ctx, hdrs.get());
      for (std::uint32_t i = 0; i < names.size(); ++i) {
        Value key{ctx, JS_AtomToString(ctx, names.atom_at(i))};
        Value val{ctx, JS_GetProperty(ctx, hdrs.get(), names.atom_at(i))};
        auto ks = vacps::js::converter<std::string>::from_js(ctx, key.get());
        auto vs = vacps::js::converter<std::string>::from_js(ctx, val.get());
        if (ks && vs) {
          out.headers.emplace_back(std::move(*ks), std::move(*vs));
        }
      }
    }
  }

  if (out.headers.empty()) {
    out.headers.emplace_back("content-type", "application/json; charset=utf-8");
  }
  co_return out;
}

}  // namespace vacps::http
