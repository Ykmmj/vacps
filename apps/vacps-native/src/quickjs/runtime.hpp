#pragma once

#include "app/error.hpp"

#include <quickjs.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace vacps::js {

/** Design defaults §23.4 */
inline constexpr std::size_t kDefaultHeapLimitBytes = 32u * 1024u * 1024u;
inline constexpr std::size_t kDefaultStackLimitBytes = 1u * 1024u * 1024u;

/**
 * Default CPU wall-clock budget for a single synchronous JS turn
 * (JS_Call / drain_jobs batch). Does **not** cover native I/O wait time.
 * 0 disables the watchdog (tests that intentionally run unbounded work).
 */
inline constexpr std::chrono::milliseconds kDefaultJsTimeBudget{30'000};

/**
 * Owns JSRuntime. All QuickJS access must stay on the Asio main thread.
 * Job drain is non-recursive (design §23.3).
 *
 * Interrupt watchdog (security):
 * - JS_SetCanBlock(false) so Atomics.wait cannot park the event loop
 * - JS_SetInterruptHandler deadline; pure JS busy-loops throw "interrupted"
 */
class Runtime {
 public:
  Runtime() noexcept = default;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  Runtime(Runtime&& other) noexcept
      : rt_(std::exchange(other.rt_, nullptr)),
        interrupt_(std::move(other.interrupt_)),
        draining_jobs_(other.draining_jobs_),
        drain_requested_(other.drain_requested_) {
    other.draining_jobs_ = false;
    other.drain_requested_ = false;
  }

  Runtime& operator=(Runtime&& other) noexcept {
    if (this != &other) {
      reset();
      rt_ = std::exchange(other.rt_, nullptr);
      interrupt_ = std::move(other.interrupt_);
      draining_jobs_ = other.draining_jobs_;
      drain_requested_ = other.drain_requested_;
      other.draining_jobs_ = false;
      other.drain_requested_ = false;
    }
    return *this;
  }

  ~Runtime() { reset(); }

  void reset() noexcept {
    if (rt_ != nullptr) {
      JS_SetInterruptHandler(rt_, nullptr, nullptr);
      JS_FreeRuntime(rt_);
      rt_ = nullptr;
    }
    interrupt_.reset();
    draining_jobs_ = false;
    drain_requested_ = false;
  }

  [[nodiscard]] static Result<Runtime> create(
      std::size_t heap_limit_bytes = kDefaultHeapLimitBytes,
      std::size_t stack_limit_bytes = kDefaultStackLimitBytes);

  [[nodiscard]] JSRuntime* get() const noexcept { return rt_; }
  [[nodiscard]] bool ok() const noexcept { return rt_ != nullptr; }

  /**
   * Arm wall-clock deadline for JS_SetInterruptHandler.
   * budget <= 0 clears the deadline (watchdog off until re-armed).
   * Not re-entrant: a new arm replaces any prior deadline.
   */
  void arm_interrupt(std::chrono::milliseconds budget) noexcept;

  /** Disarm interrupt deadline (no-op if not armed). */
  void clear_interrupt() noexcept;

  /** True when a deadline is armed and steady_clock::now() has passed it. */
  [[nodiscard]] bool interrupt_expired() const noexcept;

  /** Remaining budget if armed; nullopt if unarmed; 0ms if already expired. */
  [[nodiscard]] std::optional<std::chrono::milliseconds> interrupt_remaining() const noexcept;

  [[nodiscard]] bool interrupt_armed() const noexcept;

  /**
   * Execute pending promise jobs until the queue is empty.
   * Safe against re-entry: nested requests set drain_requested_ only.
   */
  VoidResult drain_jobs();

  /**
   * Execute at most `max_jobs` pending jobs (microtask fairness budget).
   * Returns the number of jobs executed, or error if a job throws.
   * Used by await_settled so long then-chains can yield to the io_context.
   */
  Result<std::size_t> drain_jobs_budgeted(std::size_t max_jobs);

  /** Request a drain; if already draining, just set the flag. */
  VoidResult request_drain() { return drain_jobs(); }

 private:
  /**
   * Heap-stable interrupt state (opaque for JS_SetInterruptHandler).
   * Survives Runtime moves; never stored as `this` pointer.
   */
  struct InterruptState {
    std::chrono::steady_clock::time_point deadline{};
    bool armed{false};
  };

  explicit Runtime(JSRuntime* rt, std::unique_ptr<InterruptState> interrupt) noexcept
      : rt_(rt), interrupt_(std::move(interrupt)) {}

  static int interrupt_handler(JSRuntime* rt, void* opaque) noexcept;

  JSRuntime* rt_{nullptr};
  std::unique_ptr<InterruptState> interrupt_;
  bool draining_jobs_{false};
  bool drain_requested_{false};
};

/**
 * RAII arm/clear for a single Host JS entry. Clears even if eval throws.
 * budget <= 0 leaves the watchdog disarmed (no-op scope).
 * Do not nest: outer clear would disarm an inner arm (use separate blocks).
 */
class InterruptScope {
 public:
  InterruptScope(Runtime& rt, std::chrono::milliseconds budget) noexcept
      : rt_(&rt), active_(budget.count() > 0) {
    if (active_) {
      rt_->arm_interrupt(budget);
    }
  }
  InterruptScope(const InterruptScope&) = delete;
  InterruptScope& operator=(const InterruptScope&) = delete;
  ~InterruptScope() {
    if (active_ && rt_ != nullptr) {
      rt_->clear_interrupt();
    }
  }

 private:
  Runtime* rt_{nullptr};
  bool active_{false};
};

}  // namespace vacps::js
