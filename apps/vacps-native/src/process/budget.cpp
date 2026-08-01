#include "process/budget.hpp"

#include <format>

namespace vacps::process {

ProcessBudget::ProcessBudget(ProcessLimits limits) : limits_(limits) {
  if (limits_.max_running == 0) {
    limits_.max_running = 128;
  }
  if (limits_.max_total_buffer_bytes == 0) {
    limits_.max_total_buffer_bytes = 64 * 1024 * 1024;
  }
}

Result<void> ProcessBudget::try_acquire_process() {
  for (;;) {
    auto cur = running_.load(std::memory_order_relaxed);
    if (cur >= limits_.max_running) {
      return std::unexpected(Error{std::format(
          "process.start: too many concurrent processes (max_running={})",
          limits_.max_running)});
    }
    if (running_.compare_exchange_weak(
            cur, cur + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      return {};
    }
  }
}

void ProcessBudget::release_process() noexcept {
  auto cur = running_.load(std::memory_order_relaxed);
  while (cur > 0) {
    if (running_.compare_exchange_weak(
            cur, cur - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      return;
    }
  }
}

std::size_t ProcessBudget::global_buffer_room() const noexcept {
  const auto used = buffered_.load(std::memory_order_relaxed);
  if (used >= limits_.max_total_buffer_bytes) {
    return 0;
  }
  return limits_.max_total_buffer_bytes - used;
}

void ProcessBudget::add_buffered(std::size_t n) noexcept {
  if (n == 0) {
    return;
  }
  buffered_.fetch_add(n, std::memory_order_relaxed);
}

void ProcessBudget::sub_buffered(std::size_t n) noexcept {
  if (n == 0) {
    return;
  }
  auto cur = buffered_.load(std::memory_order_relaxed);
  for (;;) {
    const auto next = cur > n ? cur - n : 0;
    if (buffered_.compare_exchange_weak(
            cur, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
      return;
    }
  }
}

}  // namespace vacps::process
