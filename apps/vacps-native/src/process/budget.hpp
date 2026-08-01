#pragma once

#include "app/error.hpp"

#include <atomic>
#include <cstddef>
#include <memory>

namespace vacps::process {

/** Technical limits for concurrent processes and total captured buffer. */
struct ProcessLimits {
  /** Max simultaneous started (not-yet-closed) processes. 0 → default 128. */
  std::size_t max_running{128};
  /** Sum of retained stdout+stderr across all live processes. 0 → 64 MiB. */
  std::size_t max_total_buffer_bytes{64 * 1024 * 1024};
};

/**
 * Shared process/buffer budget. RAII slots return capacity on destroy.
 * Does not track Process instances — only counters.
 */
class ProcessBudget : public std::enable_shared_from_this<ProcessBudget> {
 public:
  explicit ProcessBudget(ProcessLimits limits = {});

  [[nodiscard]] const ProcessLimits& limits() const noexcept { return limits_; }

  /** Try to reserve one running-process slot. Fails if at max_running. */
  [[nodiscard]] Result<void> try_acquire_process();

  void release_process() noexcept;

  /** Bytes still available under the global buffer cap. */
  [[nodiscard]] std::size_t global_buffer_room() const noexcept;

  void add_buffered(std::size_t n) noexcept;
  void sub_buffered(std::size_t n) noexcept;

  [[nodiscard]] std::size_t running() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t buffered() const noexcept {
    return buffered_.load(std::memory_order_relaxed);
  }

 private:
  ProcessLimits limits_{};
  std::atomic<std::size_t> running_{0};
  std::atomic<std::size_t> buffered_{0};
};

/** RAII process slot (released on destroy). */
class ProcessSlot {
 public:
  ProcessSlot() = default;
  explicit ProcessSlot(std::shared_ptr<ProcessBudget> budget) noexcept
      : budget_(std::move(budget)) {}
  ProcessSlot(const ProcessSlot&) = delete;
  ProcessSlot& operator=(const ProcessSlot&) = delete;
  ProcessSlot(ProcessSlot&& o) noexcept : budget_(std::move(o.budget_)) {}
  ProcessSlot& operator=(ProcessSlot&& o) noexcept {
    if (this != &o) {
      reset();
      budget_ = std::move(o.budget_);
    }
    return *this;
  }
  ~ProcessSlot() { reset(); }

  void reset() noexcept {
    if (budget_) {
      budget_->release_process();
      budget_.reset();
    }
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(budget_);
  }

 private:
  std::shared_ptr<ProcessBudget> budget_;
};

}  // namespace vacps::process
