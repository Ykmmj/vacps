#pragma once

#include "app/error.hpp"

#include <quickjs.h>

#include <cstddef>
#include <utility>

namespace vacps::js {

/** Design defaults §23.4 */
inline constexpr std::size_t kDefaultHeapLimitBytes = 32u * 1024u * 1024u;
inline constexpr std::size_t kDefaultStackLimitBytes = 1u * 1024u * 1024u;

/**
 * Owns JSRuntime. All QuickJS access must stay on the Asio main thread.
 * Job drain is non-recursive (design §23.3).
 */
class Runtime {
 public:
  Runtime() noexcept = default;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  Runtime(Runtime&& other) noexcept
      : rt_(std::exchange(other.rt_, nullptr)),
        draining_jobs_(other.draining_jobs_),
        drain_requested_(other.drain_requested_) {
    other.draining_jobs_ = false;
    other.drain_requested_ = false;
  }

  Runtime& operator=(Runtime&& other) noexcept {
    if (this != &other) {
      reset();
      rt_ = std::exchange(other.rt_, nullptr);
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
      JS_FreeRuntime(rt_);
      rt_ = nullptr;
    }
    draining_jobs_ = false;
    drain_requested_ = false;
  }

  [[nodiscard]] static Result<Runtime> create(
      std::size_t heap_limit_bytes = kDefaultHeapLimitBytes,
      std::size_t stack_limit_bytes = kDefaultStackLimitBytes);

  [[nodiscard]] JSRuntime* get() const noexcept { return rt_; }
  [[nodiscard]] bool ok() const noexcept { return rt_ != nullptr; }

  /**
   * Execute pending promise jobs until queue.
   * Safe against re-entry: nested requests set drain_requested_ only.
   */
  VoidResult drain_jobs();

  /** Request a drain; if already draining, just set the flag. */
  VoidResult request_drain() { return drain_jobs(); }

 private:
  explicit Runtime(JSRuntime* rt) noexcept : rt_(rt) {}

  JSRuntime* rt_{nullptr};
  bool draining_jobs_{false};
  bool drain_requested_{false};
};

}  // namespace vacps::js
