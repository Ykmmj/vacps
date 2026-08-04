#include "runtime/runtime_script.hpp"

#include "runtime/detail/runtime_impl.hpp"
#include "runtime/js_engine.hpp"

#include <string>
#include <utility>

namespace vacps {

JSContext* Runtime::Script::prepare_entry() const noexcept {
  return impl_.owner_context();
}

Runtime::Script::Script(Impl& impl) noexcept : impl_(impl) {}

runtime::Result<runtime::EvaluatedModule> Runtime::Script::evaluate_module(
    std::string_view source,
    std::string_view filename) {
  JSContext* ctx = prepare_entry();

  // Empty source is valid JavaScript (not rejected merely for being empty).
  const std::string filename_buf{filename};
  const char* filename_c =
      filename_buf.empty() ? "<module>" : filename_buf.c_str();
  const char* source_c = source.data() != nullptr ? source.data() : "";

  runtime::EvaluatedModule out{};
  {
    runtime::InterruptBudget budget{
        impl_.engine(), impl_.options().engine.js_time_budget};

    vacps::qjs::OwnedValue compiled{
        ctx,
        JS_Eval(
            ctx,
            source_c,
            source.size(),
            filename_c,
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY)};
    if (compiled.is_exception()) {
      return std::unexpected(impl_.engine().take_current_exception(
          "module compilation failed"));
    }
    if (JS_VALUE_GET_TAG(compiled.get()) != JS_TAG_MODULE) {
      return std::unexpected(
          runtime::Error::js("compiled value is not a module"));
    }

    out.definition =
        static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.get()));
    vacps::qjs::OwnedValue completion{
        ctx, JS_EvalFunction(ctx, compiled.release())};
    if (completion.is_exception()) {
      return std::unexpected(impl_.engine().take_current_exception(
          "module evaluation failed"));
    }
    out.completion = std::move(completion);
  }

  impl_.schedule_job_pump();
  return out;
}

runtime::Result<vacps::qjs::OwnedValue> Runtime::Script::invoke_export(
    JSModuleDef* definition,
    const char* name) {
  JSContext* ctx = prepare_entry();

  vacps::qjs::OwnedValue result{};
  {
    runtime::InterruptBudget budget{
        impl_.engine(), impl_.options().engine.js_time_budget};

    vacps::qjs::OwnedValue ns{ctx, JS_GetModuleNamespace(ctx, definition)};
    if (ns.is_exception()) {
      return std::unexpected(impl_.engine().take_current_exception(
          "JS_GetModuleNamespace failed"));
    }

    vacps::qjs::OwnedValue fn =
        vacps::qjs::OwnedValue::get_property_str(ctx, ns.get(), name);
    if (fn.is_exception()) {
      return std::unexpected(impl_.engine().take_current_exception(
          "module export lookup failed"));
    }
    if (!fn.is_function()) {
      // Missing / non-callable export in the user module — operational.
      return std::unexpected(runtime::Error::invalid_state(
          "module export is missing or not a function"));
    }

    vacps::qjs::OwnedValue call_result{
        ctx, JS_Call(ctx, fn.get(), JS_UNDEFINED, 0, nullptr)};
    if (call_result.is_exception()) {
      return std::unexpected(
          impl_.engine().take_current_exception("module export call failed"));
    }
    result = std::move(call_result);
  }

  impl_.schedule_job_pump();
  return result;
}

}  // namespace vacps
