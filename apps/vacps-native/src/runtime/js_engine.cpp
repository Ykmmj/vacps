#include "runtime/js_engine.hpp"

#include "qjs/scoped_cstring.hpp"

#include <mimalloc.h>

#include <cstddef>
#include <string>
#include <utility>

namespace vacps::runtime {
namespace {

// QuickJS 2026-06-04 uses an eight-byte allocator overhead on Linux when
// enforcing JS_SetMemoryLimit. Custom allocators must preserve that accounting.
constexpr std::size_t kQuickJsMallocOverhead{8};

[[nodiscard]] mi_heap_t* allocator_heap(JSMallocState* state) noexcept {
  return static_cast<mi_heap_t*>(state->opaque);
}

void* quickjs_malloc(JSMallocState* state, std::size_t size) noexcept {
  if (state->malloc_size + size > state->malloc_limit) {
    return nullptr;
  }

  void* ptr = mi_heap_malloc(allocator_heap(state), size);
  if (ptr == nullptr) {
    return nullptr;
  }

  ++state->malloc_count;
  state->malloc_size += mi_usable_size(ptr) + kQuickJsMallocOverhead;
  return ptr;
}

void quickjs_free(JSMallocState* state, void* ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }

  --state->malloc_count;
  state->malloc_size -= mi_usable_size(ptr) + kQuickJsMallocOverhead;
  mi_free(ptr);
}

void* quickjs_realloc(
    JSMallocState* state,
    void* ptr,
    std::size_t size) noexcept {
  if (ptr == nullptr) {
    if (size == 0) {
      return nullptr;
    }
    return quickjs_malloc(state, size);
  }

  const std::size_t old_size = mi_usable_size(ptr);
  if (size == 0) {
    --state->malloc_count;
    state->malloc_size -= old_size + kQuickJsMallocOverhead;
    mi_free(ptr);
    return nullptr;
  }
  if (state->malloc_size + size - old_size > state->malloc_limit) {
    return nullptr;
  }

  void* resized = mi_heap_realloc(allocator_heap(state), ptr, size);
  if (resized == nullptr) {
    return nullptr;
  }

  state->malloc_size += mi_usable_size(resized) - old_size;
  return resized;
}

std::size_t quickjs_malloc_usable_size(const void* ptr) noexcept {
  return mi_usable_size(ptr);
}

const JSMallocFunctions kQuickJsMimallocFunctions{
    .js_malloc = &quickjs_malloc,
    .js_free = &quickjs_free,
    .js_realloc = &quickjs_realloc,
    .js_malloc_usable_size = &quickjs_malloc_usable_size,
};

}  // namespace

JsEngine::~JsEngine() noexcept {
  if (is_open()) {
    close();
  }
}

VoidResult JsEngine::open(const EngineOptions& options) {
  allocator_heap_ = mi_heap_new();
  if (allocator_heap_ == nullptr) {
    return std::unexpected(Error::internal("mi_heap_new failed"));
  }

  runtime_ = JS_NewRuntime2(&kQuickJsMimallocFunctions, allocator_heap_);
  if (runtime_ == nullptr) {
    mi_heap_delete(allocator_heap_);
    allocator_heap_ = nullptr;
    return std::unexpected(Error::internal("JS_NewRuntime2 failed"));
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
    mi_heap_delete(allocator_heap_);
    allocator_heap_ = nullptr;
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
  mi_heap_delete(allocator_heap_);
  allocator_heap_ = nullptr;
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
