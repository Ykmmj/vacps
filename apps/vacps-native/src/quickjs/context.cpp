#include "quickjs/context.hpp"

#include "quickjs/cstring.hpp"
#include "quickjs/value.hpp"

#include <string>

namespace vacps::js {

Result<Context> Context::create(Runtime& runtime) {
  if (!runtime.ok()) {
    return std::unexpected(Error{"runtime not open"});
  }
  JSContext* raw = JS_NewContext(runtime.get());
  if (raw == nullptr) {
    return std::unexpected(Error{"JS_NewContext failed"});
  }
  return Context{raw};
}

Error Context::take_exception_error() {
  if (ctx_ == nullptr) {
    return Error{"no js context"};
  }
  Value ex{ctx_, JS_GetException(ctx_)};
  auto cs = CString::from_value(ctx_, ex.get());
  if (cs.empty()) {
    return Error{"js exception (unprintable)"};
  }
  return Error{cs.str()};
}

Result<Value> Context::eval(
    std::string_view source,
    std::string_view filename,
    int flags) {
  if (ctx_ == nullptr) {
    return std::unexpected(Error{"context not open"});
  }

  const std::string filename_owned{filename};
  Value raw{
      ctx_,
      JS_Eval(ctx_, source.data(), source.size(), filename_owned.c_str(), flags)};

  if (raw.is_exception()) {
    raw.reset();
    return std::unexpected(take_exception_error());
  }

  return raw;
}

}  // namespace vacps::js
