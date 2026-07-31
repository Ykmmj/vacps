#pragma once

#include "app/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vacps::process {

namespace asio = boost::asio;

struct StartOptions {
  std::string cwd;
  /** 0 = no timeout. */
  std::int32_t timeout_ms{0};
  bool close_stdin{true};
  std::size_t hard_max_stdout{16 * 1024 * 1024};
  std::size_t hard_max_stderr{16 * 1024 * 1024};
};

struct StartInfo {
  std::string id;
  std::int32_t pid{0};
};

struct ReadOptions {
  std::int32_t wait_ms{0};
  std::size_t max_bytes{65'536};
  std::size_t stdout_offset{0};
  std::size_t stderr_offset{0};
};

struct ReadInfo {
  std::string status;  // running | exited | timed_out | cancelled
  std::int32_t exit_code{0};
  bool timed_out{false};
  /** True only when process exited AND both pipes EOF AND no unread buffer. */
  bool eof{false};
  bool stdin_open{false};
  std::string stdout_slice;
  std::string stderr_slice;
  /** Bytes retained in the registry buffer (may be < produced after hard_max). */
  std::size_t stdout_total{0};
  std::size_t stderr_total{0};
  /** Total bytes observed on the pipe (includes discarded after hard_max). */
  std::size_t stdout_produced{0};
  std::size_t stderr_produced{0};
  bool stdout_truncated{false};
  bool stderr_truncated{false};
  std::size_t next_stdout_offset{0};
  std::size_t next_stderr_offset{0};
};

struct WriteOptions {
  bool close_stdin{false};
  /** 0 = no timeout. Default 30s so a stalled peer cannot hang the io_context forever. */
  std::int32_t timeout_ms{30'000};
  /** Reject payloads larger than this (default 1 MiB). */
  std::size_t max_bytes{1 * 1024 * 1024};
};

struct RegistryLimits {
  /** Max tracked processes (running + retained finished). */
  std::size_t max_entries{128};
  /** Sum of retained stdout+stderr buffers across all entries. */
  std::size_t max_total_buffer_bytes{64 * 1024 * 1024};
  /** After finished, auto-close if client does not call close() (ms). 0 = never. */
  std::int32_t retention_ms{60'000};
};

/**
 * Internal subprocess registry (Boost.Process v2 + Asio).
 * Not a product API — process::Process is the handle; JS never sees entry ids.
 * Single-threaded: all methods co_awaited / called on the host io_context.
 * Process groups: setpgid + kill(-pgid) on timeout/terminate (design §19.2).
 *
 * Lifecycle reclaim:
 * - close(id): free buffers immediately
 * - finished + retention_ms TTL: auto-erase if not closed
 * - start may reclaim oldest finished entries when over max_entries / buffer budget
 *
 * Owned by ScriptServices; Process holds a non-owning Registry&.
 */
class Registry {
 public:
  explicit Registry(asio::any_io_executor ex, RegistryLimits limits = {});
  ~Registry();

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  [[nodiscard]] const RegistryLimits& limits() const noexcept { return limits_; }

  [[nodiscard]] asio::awaitable<Result<StartInfo>> start(
      std::vector<std::string> argv,
      StartOptions opts = {});

  [[nodiscard]] asio::awaitable<Result<ReadInfo>> read(
      std::string id,
      ReadOptions opts = {});

  /**
   * Async stdin write (never blocks the io_context thread on a full pipe).
   * Serializes concurrent writes to the same process. Owns `data` until complete.
   */
  [[nodiscard]] asio::awaitable<Result<std::size_t>> write(
      std::string id,
      std::string data,
      WriteOptions opts = {});

  /** signal: SIGTERM | SIGINT | SIGKILL (case-insensitive). */
  [[nodiscard]] Result<bool> terminate(
      const std::string& id,
      std::string_view signal = "SIGTERM",
      std::int32_t grace_ms = 3000);

  /**
   * Drop a process entry and free buffers. Idempotent: unknown id → false.
   * Running processes are killed first.
   */
  [[nodiscard]] Result<bool> close(const std::string& id);

  [[nodiscard]] Result<ReadInfo> snapshot(const std::string& id) const;

  [[nodiscard]] std::size_t entry_count() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t total_buffered_bytes() const noexcept;

  /** Best-effort kill all children (host shutdown). */
  void shutdown() noexcept;

 private:
  struct Entry;

  [[nodiscard]] std::shared_ptr<Entry> find(const std::string& id) const;
  void notify_waiters(Entry& e) noexcept;
  void notify_write_waiters(Entry& e) noexcept;
  void schedule_timeout(std::shared_ptr<Entry> e, std::int32_t timeout_ms);
  void schedule_grace_kill(std::shared_ptr<Entry> e, std::int32_t grace_ms);
  void schedule_retention(std::shared_ptr<Entry> e);
  void kill_group(Entry& e, int sig) noexcept;
  /** Process wait completed; finished only after both pipe EOFs. */
  void on_process_exit(Entry& e, std::int32_t code, bool timed_out, bool cancelled);
  /** Mark fully complete when process_exited && out_eof && err_eof. */
  void try_finish(Entry& e) noexcept;
  /** Erase from map; cancels timers; returns true if removed. */
  bool erase_entry(const std::string& id) noexcept;
  /** Drop oldest finished entries until under entry/buffer limits. */
  void reclaim_finished_for_limits() noexcept;
  [[nodiscard]] bool reclaim_one_finished_oldest() noexcept;

  asio::any_io_executor ex_;
  RegistryLimits limits_{};
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
  std::uint64_t seq_{0};
};

}  // namespace vacps::process
