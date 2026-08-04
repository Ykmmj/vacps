#pragma once

/**
 * QuickJS engine ownership only.
 * All C API use is confined to the owner (JS) thread.
 */

#include "runtime/error.hpp"
#include "qjs/owned_value.hpp"

#include <quickjs.h>

#include <chrono>
#include <cstddef>
#include <string_view>
#include <utility>

namespace vacps::runtime {

struct EngineOptions {
  std::size_t heap_limit_bytes{64 * 1024 * 1024};
  std::size_t stack_limit_bytes{1 * 1024 * 1024};
  /** 0 = no CPU interrupt watchdog on JS turns. */
  std::chrono::milliseconds js_time_budget{std::chrono::milliseconds{30'000}};
};

enum class JobState { executed,
                      empty };

class JsEngine {
 public:
  JsEngine() noexcept = default;
  ~JsEngine() noexcept;

  JsEngine(const JsEngine&) = delete;
  JsEngine& operator=(const JsEngine&) = delete;

  [[nodiscard]] VoidResult open(const EngineOptions& options);

  /** Narrow: engine is open and caller is the owner thread. */
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept {
    return runtime_ != nullptr && context_ != nullptr;
  }

  [[nodiscard]] JSRuntime* runtime() noexcept { return runtime_; }
  [[nodiscard]] JSContext* context() noexcept { return context_; }

  [[nodiscard]] Result<vacps::qjs::OwnedValue> evaluate(
      std::string_view source,
      std::string_view filename,
      int flags);

  [[nodiscard]] Result<JobState> execute_one_pending_job();
  [[nodiscard]] bool has_pending_jobs() const noexcept;

  /**
   * Extract the current exception from ctx (or the engine context if null).
   * Prefix is prepended when non-empty for a stable Error.message shape.
   */
  [[nodiscard]] Error take_current_exception(
      std::string_view prefix = {},
      JSContext* ctx = nullptr);

  /** Arm/clear CPU interrupt for one synchronous JS turn (budget ≤0 = no-op). */
  void arm_interrupt(std::chrono::milliseconds budget) noexcept;
  void clear_interrupt() noexcept;

 private:
  static int interrupt_handler(JSRuntime* rt, void* opaque) noexcept;

  JSRuntime* runtime_{nullptr};
  JSContext* context_{nullptr};
  std::chrono::steady_clock::time_point interrupt_deadline_{};
  bool interrupt_armed_{false};
};

/**
 * RAII: arm interrupt for the duration of a JS turn.
 * Do not call the destructor explicitly — scope lifetime only.
 */
class InterruptBudget {
 public:
  InterruptBudget(JsEngine& engine, std::chrono::milliseconds budget) noexcept
      : engine_(&engine) {
    engine_->arm_interrupt(budget);
  }

  ~InterruptBudget() noexcept {
    reset();
  }

  InterruptBudget(const InterruptBudget&) = delete;
  InterruptBudget& operator=(const InterruptBudget&) = delete;

  InterruptBudget(InterruptBudget&& other) noexcept
      : engine_(std::exchange(other.engine_, nullptr)) {}

  InterruptBudget& operator=(InterruptBudget&&) = delete;

  void reset() noexcept {
    if (engine_ != nullptr) {
      engine_->clear_interrupt();
      engine_ = nullptr;
    }
  }

 private:
  JsEngine* engine_{nullptr};
};

}  // namespace vacps::runtime
