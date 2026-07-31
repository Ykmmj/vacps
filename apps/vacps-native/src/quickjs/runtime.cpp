#include "quickjs/runtime.hpp"

#include "app/log.hpp"
#include "quickjs/js_bridge.hpp"
#include "quickjs/value.hpp"

#include <format>

namespace vacps::js {

int Runtime::interrupt_handler(JSRuntime* /*rt*/, void* opaque) noexcept {
  auto* st = static_cast<InterruptState*>(opaque);
  if (st == nullptr || !st->armed) {
    return 0;
  }
  if (std::chrono::steady_clock::now() >= st->deadline) {
    return 1;  // non-zero → QuickJS throws uncatchable "interrupted"
  }
  return 0;
}

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

  // Never allow Atomics.wait / blocking atomics to park the Asio thread.
  JS_SetCanBlock(raw, 0);

  auto interrupt = std::make_unique<InterruptState>();
  JS_SetInterruptHandler(raw, interrupt_handler, interrupt.get());

  Runtime rt{raw, std::move(interrupt)};
  log::info(
      "quickjs runtime ready (heap_limit={} stack_limit={} can_block=false interrupt=on)",
      heap_limit_bytes,
      stack_limit_bytes);
  return rt;
}

void Runtime::arm_interrupt(std::chrono::milliseconds budget) noexcept {
  if (interrupt_ == nullptr) {
    return;
  }
  if (budget.count() <= 0) {
    interrupt_->armed = false;
    return;
  }
  interrupt_->deadline = std::chrono::steady_clock::now() + budget;
  interrupt_->armed = true;
}

void Runtime::clear_interrupt() noexcept {
  if (interrupt_ != nullptr) {
    interrupt_->armed = false;
  }
}

bool Runtime::interrupt_expired() const noexcept {
  if (interrupt_ == nullptr || !interrupt_->armed) {
    return false;
  }
  return std::chrono::steady_clock::now() >= interrupt_->deadline;
}

std::optional<std::chrono::milliseconds> Runtime::interrupt_remaining() const noexcept {
  if (interrupt_ == nullptr || !interrupt_->armed) {
    return std::nullopt;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= interrupt_->deadline) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(interrupt_->deadline - now);
}

bool Runtime::interrupt_armed() const noexcept {
  return interrupt_ != nullptr && interrupt_->armed;
}

VoidResult Runtime::drain_jobs() {
  auto n = drain_jobs_budgeted(static_cast<std::size_t>(-1));
  if (!n) return std::unexpected(std::move(n.error()));
  return {};
}

Result<std::size_t> Runtime::drain_jobs_budgeted(std::size_t max_jobs) {
  if (rt_ == nullptr) {
    return std::unexpected(Error{"runtime not open"});
  }
  if (max_jobs == 0) {
    return std::size_t{0};
  }

  if (draining_jobs_) {
    drain_requested_ = true;
    return std::size_t{0};
  }

  draining_jobs_ = true;
  drain_requested_ = false;
  std::size_t ran = 0;

  do {
    drain_requested_ = false;
    while (ran < max_jobs) {
      if (interrupt_expired()) {
        draining_jobs_ = false;
        return std::unexpected(Error{"js interrupted: time budget exceeded"});
      }
      JSContext* ctx = nullptr;
      const int err = JS_ExecutePendingJob(rt_, &ctx);
      if (err == 0) {
        break;  // no more jobs
      }
      if (err < 0) {
        draining_jobs_ = false;
        if (ctx != nullptr) {
          Value ex{ctx, JS_GetException(ctx)};
          return std::unexpected(Error{std::format(
              "JS_ExecutePendingJob failed: {}",
              format_js_exception(ctx, ex.get()))});
        }
        return std::unexpected(Error{"JS_ExecutePendingJob failed"});
      }
      ++ran;
    }
  } while (drain_requested_ && ran < max_jobs);

  draining_jobs_ = false;
  return ran;
}

}  // namespace vacps::js
