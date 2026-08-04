#include "runtime/runtime_callbacks.hpp"

#include "runtime/detail/runtime_impl.hpp"
#include "runtime/js_engine.hpp"

#include <climits>
#include <cstddef>
#include <utility>
#include <vector>

namespace vacps {

Runtime::Callbacks::Callbacks(Impl& impl) noexcept : impl_(impl) {}

runtime::asio::awaitable<runtime::Result<vacps::qjs::OwnedValue>>
Runtime::Callbacks::call_and_await(
    JSValueConst callable,
    std::vector<vacps::qjs::OwnedValue> args,
    runtime::JsAwaitOptions options) {
  JSContext* ctx = impl_.owner_context();

  std::vector<JSValueConst> argv;
  argv.reserve(args.size());

  for (std::size_t i = 0; i < args.size(); ++i) {
    argv.push_back(args[i].get());
  }

  vacps::qjs::OwnedValue call_result;
  {
    runtime::InterruptBudget budget{
        impl_.engine(), impl_.options().engine.js_time_budget};

    const int argc = static_cast<int>(argv.size());
    JSValue raw = JS_Call(
        ctx,
        callable,
        JS_UNDEFINED,
        argc,
        argc > 0 ? argv.data() : nullptr);

    argv.clear();
    args.clear();

    if (JS_IsException(raw)) {
      co_return std::unexpected(impl_.engine().take_current_exception(
          "Runtime::Callbacks::call_and_await"));
    }
    call_result = vacps::qjs::OwnedValue::take(ctx, raw);
  }

  impl_.schedule_job_pump();

  // args already released on owner before this first suspension.
  co_return co_await runtime::detail::JsPromiseAwaitAccess::await_value(
      impl_.facade(), std::move(call_result), std::move(options));
}

}  // namespace vacps
