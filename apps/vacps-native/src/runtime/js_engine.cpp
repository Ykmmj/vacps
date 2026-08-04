#include "runtime/js_engine.hpp"

#include "qjs/scoped_cstring.hpp"

#include <string>
#include <utility>

namespace vacps::runtime {

JsEngine::~JsEngine() noexcept {
  if (is_open()) {
    close();
  }
}

VoidResult JsEngine::open(const EngineOptions& options) {
  runtime_ = JS_NewRuntime();
  if (runtime_ == nullptr) {
    return std::unexpected(Error::internal("JS_NewRuntime failed"));
  }

  if (options.heap_limit_bytes > 0) {
    JS_SetMemoryLimit(runtime_, options.heap_limit_bytes);
  }
  if (options.stack_limit_bytes > 0) {
    JS_SetMaxStackSize(runtime_, options.stack_limit_bytes);
  }
  JS_SetCanBlock(runtime_, 0);
  JS_SetInterruptHandler(runtime_, &JsEngine::interrupt_handler, this);

  context_ = JS_NewContext(runtime_);
  if (context_ == nullptr) {
    JS_SetInterruptHandler(runtime_, nullptr, nullptr);
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
    return std::unexpected(Error::internal("JS_NewContext failed"));
  }
  return {};
}

void JsEngine::close() noexcept {
  clear_interrupt();
  JS_SetInterruptHandler(runtime_, nullptr, nullptr);
  JS_SetContextOpaque(context_, nullptr);
  JS_FreeContext(context_);
  context_ = nullptr;
  JS_FreeRuntime(runtime_);
  runtime_ = nullptr;
}

int JsEngine::interrupt_handler(JSRuntime* /*rt*/, void* opaque) noexcept {
  auto* self = static_cast<JsEngine*>(opaque);
  if (!self->interrupt_armed_) {
    return 0;
  }
  if (std::chrono::steady_clock::now() >= self->interrupt_deadline_) {
    return 1;
  }
  return 0;
}

void JsEngine::arm_interrupt(std::chrono::milliseconds budget) noexcept {
  if (budget.count() <= 0) {
    clear_interrupt();
    return;
  }
  interrupt_deadline_ = std::chrono::steady_clock::now() + budget;
  interrupt_armed_ = true;
}

void JsEngine::clear_interrupt() noexcept {
  interrupt_armed_ = false;
}

Result<vacps::qjs::OwnedValue> JsEngine::evaluate(
    std::string_view source,
    std::string_view filename,
    int flags) {
  // QuickJS requires a NUL-terminated filename; string_view may not be.
  const std::string filename_buf{filename};
  const char* filename_c =
      filename_buf.empty() ? "<eval>" : filename_buf.c_str();

  // source is length-based; empty views may have null data().
  const char* source_c = source.empty() ? "" : source.data();
  vacps::qjs::OwnedValue result{
      context_,
      JS_Eval(
          context_,
          source_c,
          source.size(),
          filename_c,
          flags)};
  if (result.is_exception()) {
    return std::unexpected(
        take_current_exception("JavaScript evaluation failed"));
  }
  return result;
}

Result<JobState> JsEngine::execute_one_pending_job() {
  JSContext* job_context = nullptr;
  const int result = JS_ExecutePendingJob(runtime_, &job_context);
  if (result == 0) {
    return JobState::empty;
  }
  if (result < 0) {
    // Prefer the job's own context so exception text matches the failing job.
    JSContext* error_ctx =
        job_context != nullptr ? job_context : context_;
    return std::unexpected(Error::js(
        "QuickJS pending job failed: " +
        take_current_exception({}, error_ctx).message));
  }
  return JobState::executed;
}

bool JsEngine::has_pending_jobs() const noexcept {
  return JS_IsJobPending(runtime_) != 0;
}

Error JsEngine::take_current_exception(
    std::string_view prefix,
    JSContext* ctx) {
  JSContext* use = ctx != nullptr ? ctx : context_;
  vacps::qjs::OwnedValue exception{use, JS_GetException(use)};
  auto text = vacps::qjs::ScopedCString::from_value(use, exception.get());
  std::string message;
  if (!text.empty()) {
    message = text.str();
  } else {
    // JS_ToCString failure raises a secondary exception; clear it so the
    // caller does not observe a pending exception after we return Error.
    JS_FreeValue(use, JS_GetException(use));
    message = "unknown JavaScript exception";
  }
  if (!prefix.empty()) {
    message = std::string{prefix} + ": " + message;
  }
  return Error::js(std::move(message));
}

}  // namespace vacps::runtime
