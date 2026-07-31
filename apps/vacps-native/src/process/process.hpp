#pragma once

/**
 * vacps::process::Process — long-lived child handle (n1 §八).
 *
 * Construction stores argv/options only; spawn happens in start().
 * Wraps a Registry entry (JS never sees the registry id).
 * Product surface is this class; Registry is an internal detail owned by
 * ScriptServices.
 */

#include "app/error.hpp"
#include "process/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::process {

namespace asio = boost::asio;

/** Result of Process::wait() / one-shot run convenience. */
struct RunResult {
  std::int32_t exit_code{0};
  /** True only when timeout timer aborted the wait — not from exit codes. */
  bool timed_out{false};
  std::string stdout_str;
  std::string stderr_str;
  /** Bytes observed on each pipe (includes discarded after cap). */
  std::size_t stdout_produced{0};
  std::size_t stderr_produced{0};
  bool stdout_truncated{false};
  bool stderr_truncated{false};
};

/**
 * Domain process handle.
 *
 * Threading: all methods (including close / destructor side-effects) must run
 * on the Registry's io_context (host single-threaded model).
 * Registry outlives Process (ScriptServices owns Registry).
 */
class Process {
 public:
  /**
   * @param registry  Non-owning; must outlive this Process.
   * @param argv      Executable + args (argv[0] required non-empty at start()).
   * @param opts      Spawn options (defaults match StartOptions; callers that
   *                  need interactive stdin should set close_stdin = false).
   */
  Process(Registry& registry, std::vector<std::string> argv, StartOptions opts = {});

  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  Process(Process&&) = delete;
  Process& operator=(Process&&) = delete;

  /**
   * Best-effort close of the registry entry if still open.
   * Safe only while Registry is still alive.
   */
  ~Process();

  /** Asynchronous spawn via Registry. Rejects if already started or closed. */
  [[nodiscard]] asio::awaitable<VoidResult> start();

  /**
   * Progressive read of one stream (default "stdout").
   * Offsets are tracked on this Process; returns empty string when no new data
   * (including after EOF).
   *
   * @param stream  "stdout" or "stderr"
   * @param wait_ms 0 = non-blocking snapshot; >0 wait for data / finish
   * @param max_bytes Cap on returned slice (default 64 KiB)
   */
  [[nodiscard]] asio::awaitable<Result<std::string>> read(
      std::string_view stream = "stdout",
      std::int32_t wait_ms = 60'000,
      std::size_t max_bytes = 65'536);

  /** Async stdin write. Owns `data` until complete. */
  [[nodiscard]] asio::awaitable<Result<std::size_t>> write(
      std::string data,
      WriteOptions opts = {});

  /**
   * Wait until the process is no longer running; returns full captured
   * stdout/stderr (via Registry snapshot).
   */
  [[nodiscard]] asio::awaitable<Result<RunResult>> wait();

  /**
   * Send signal (SIGTERM | SIGINT | SIGKILL). Returns false if already exited.
   * Sync: Registry::terminate is non-blocking.
   */
  [[nodiscard]] Result<bool> terminate(
      std::string_view signal = "SIGTERM",
      std::int32_t grace_ms = 3000);

  /**
   * Drop registry entry and free buffers. Idempotent.
   * Running children are killed first (Registry::close).
   */
  [[nodiscard]] VoidResult close();

  /** nullopt until start() succeeds; nullopt again after close. */
  [[nodiscard]] std::optional<std::int32_t> pid() const noexcept;

  /** True while Registry reports status "running". False if not started/closed. */
  [[nodiscard]] bool running() const;

  [[nodiscard]] bool started() const noexcept {
    return start_called_ && !id_.empty() && !closed_;
  }
  [[nodiscard]] bool closed() const noexcept { return closed_; }

  [[nodiscard]] const std::vector<std::string>& argv() const noexcept { return argv_; }
  [[nodiscard]] const StartOptions& options() const noexcept { return opts_; }

 private:
  [[nodiscard]] VoidResult ensure_live(std::string_view op) const;

  Registry* registry_;  // non-owning
  std::vector<std::string> argv_;
  StartOptions opts_{};
  std::string id_;  // registry entry key; never exposed to JS
  std::int32_t pid_{0};
  bool start_called_{false};
  bool closed_{false};
  std::size_t stdout_offset_{0};
  std::size_t stderr_offset_{0};
};

}  // namespace vacps::process
