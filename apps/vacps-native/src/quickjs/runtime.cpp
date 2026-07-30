#include "quickjs/runtime.hpp"

#include "app/log.hpp"
#include "quickjs/value.hpp"

#include <format>

namespace vacps::js {

Result<Runtime> Runtime::create(
    std::size_t heap_limit_bytes,
    std::size_t stack_limit_bytes) {
  JSRuntime* raw = JS_NewRuntime();
  if (raw == nullptr) {
    return std::unexpected(Error{"JS_NewRuntime failed"});
  }

  if (heap_limit_bytes > 0) {
    JS_SetMemoryLimit(raw, heap_limit_bytes);
  }
  if (stack_limit_bytes > 0) {
    JS_SetMaxStackSize(raw, stack_limit_bytes);
  }

  Runtime rt{raw};
  log::info(
      "quickjs runtime ready (heap_limit={} stack_limit={})",
      heap_limit_bytes,
      stack_limit_bytes);
  return rt;
}

VoidResult Runtime::drain_jobs() {
  if (rt_ == nullptr) {
    return std::unexpected(Error{"runtime not open"});
  }

  if (draining_jobs_) {
    drain_requested_ = true;
    return {};
  }

  draining_jobs_ = true;
  drain_requested_ = false;

  do {
    drain_requested_ = false;
    for (;;) {
      JSContext* ctx = nullptr;
      const int err = JS_ExecutePendingJob(rt_, &ctx);
      if (err == 0) {
        break;  // no more jobs
      }
      if (err < 0) {
        // Job threw; clear exception so the runtime stays usable.
        if (ctx != nullptr) {
          Value ex{ctx, JS_GetException(ctx)};
          (void)ex;
        }
        draining_jobs_ = false;
        return std::unexpected(Error{"JS_ExecutePendingJob failed"});
      }
    }
  } while (drain_requested_);

  draining_jobs_ = false;
  return {};
}

}  // namespace vacps::js
