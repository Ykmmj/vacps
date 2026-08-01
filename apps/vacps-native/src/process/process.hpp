#pragma once

/**
 * vacps::process::Process — JS-owned child handle.
 *
 * Owns child/pipes/timers/buffers directly (Process::State).
 * ProcessRuntime supplies executor + shared ProcessBudget only.
 * No global Registry / string id / TTL reclaim.
 *
 * Destructor policy: kill still-running process group (not detach).
 */

#include "app/error.hpp"
#include "process/budget.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::process {

namespace asio = boost::asio;

struct StartOptions {
  std::string cwd;
  /** nullopt / 0 = no timeout. */
  std::chrono::milliseconds timeout{0};
  bool close_stdin{true};
  std::size_t hard_max_stdout{16 * 1024 * 1024};
  std::size_t hard_max_stderr{16 * 1024 * 1024};
};

struct WriteOptions {
  bool close_stdin{false};
  std::chrono::milliseconds timeout{30'000};
  std::size_t max_bytes{1 * 1024 * 1024};
};

enum class ProcessStatus : std::uint8_t {
  Created = 0,
  Starting,
  Running,
  Exited,
  TimedOut,
  Cancelled,
  Closing,
  Closed,
};

enum class ExitReason : std::uint8_t {
  Exited = 0,
  TimedOut,
  Cancelled,
  Signaled,
};

struct BufferStats {
  std::size_t retained{0};
  std::size_t produced{0};
  bool truncated{false};
};

struct ReadResult {
  std::string data;
  bool eof{false};
};

/** Result of Process::wait() / one-shot run. */
struct RunResult {
  std::int32_t exit_code{0};
  ExitReason reason{ExitReason::Exited};
  bool timed_out{false};
  std::string stdout_str;
  std::string stderr_str;
  std::size_t stdout_produced{0};
  std::size_t stderr_produced{0};
  bool stdout_truncated{false};
  bool stderr_truncated{false};
};

class Process final : public std::enable_shared_from_this<Process> {
 public:
  /**
   * @param executor  Host io_context executor (spawn/pipes/timers).
   * @param budget    Shared concurrent/buffer budget (required for start()).
   * @param argv      Executable + args.
   * @param opts      Spawn options.
   */
  Process(
      asio::any_io_executor executor,
      std::shared_ptr<ProcessBudget> budget,
      std::vector<std::string> argv,
      StartOptions opts = {});

  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  Process(Process&&) = delete;
  Process& operator=(Process&&) = delete;

  /** Kill still-running child (process group); cancel timers; free buffers. */
  ~Process();

  [[nodiscard]] asio::awaitable<VoidResult> start();

  /**
   * Progressive read of one stream ("stdout" | "stderr").
   * Cursor is owned by this Process.
   * @param wait  0 = non-blocking; >0 wait for data or finish.
   */
  [[nodiscard]] asio::awaitable<Result<ReadResult>> read(
      std::string_view stream = "stdout",
      std::chrono::milliseconds wait = std::chrono::milliseconds{60'000},
      std::size_t max_bytes = 65'536);

  [[nodiscard]] asio::awaitable<Result<std::size_t>> write(
      std::string data,
      WriteOptions opts = {});

  [[nodiscard]] asio::awaitable<Result<RunResult>> wait();

  /**
   * Signal process group. Returns false if already exited.
   * Grace: escalate to SIGKILL after grace if not SIGKILL.
   */
  [[nodiscard]] Result<bool> terminate(
      std::string_view signal = "SIGTERM",
      std::chrono::milliseconds grace = std::chrono::milliseconds{3000});

  /**
   * Idempotent close: kill if running, free pipes/buffers, release process slot.
   * Prefer awaitable form from JS; this sync path for tests/finalizer.
   */
  [[nodiscard]] VoidResult close();

  /** Finalizer/dtor path: same as close(), never throws. */
  void dispose() noexcept;

  [[nodiscard]] std::optional<std::int32_t> pid() const noexcept;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] bool started() const noexcept;
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] ProcessStatus status() const noexcept;

  [[nodiscard]] const std::vector<std::string>& argv() const noexcept { return argv_; }
  [[nodiscard]] const StartOptions& options() const noexcept { return opts_; }

 private:
  struct State;

  [[nodiscard]] VoidResult ensure_live(std::string_view op) const;

  std::vector<std::string> argv_;
  StartOptions opts_{};
  std::shared_ptr<State> state_;
};

}  // namespace vacps::process
