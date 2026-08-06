#pragma once

/**
 * vacps::process::Process — JS-owned child handle.
 *
 * Owns child/pipes/timers/buffers directly (Process::State).
 * ProcessRuntime supplies executor + shared ProcessBudget only.
 * No global Registry / string id / TTL reclaim.
 *
 * Thread / executor model:
 * - start / write / wait / terminate / async_close and all pipe/timer work run
 *   on the owner executor (Runtime::Impl::main_executor). Never the worker pool
 *   and never another io_context.
 * - started() / closed() / status() are owner-executor-only snapshots.
 * - dispose() / destructor may run on the owner JS thread as a finalizer
 *   fallback: they only request group SIGKILL and cancel pipes/timers (via
 *   post onto the owner executor when needed) and never block. Detached State
 *   operations self-retain until real reap + drain completions.
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
  /** 0 = no timeout. */
  std::chrono::milliseconds timeout{0};
  /**
   * true  → close stdin immediately after spawn (run default).
   * false → leave stdin open for write() (Process class default).
   */
  bool close_stdin{false};
  /** Cap retained stdout; 0 captures nothing (produced may still grow). */
  std::size_t max_stdout_bytes{16 * 1024 * 1024};
  /** Cap retained stderr; 0 captures nothing. */
  std::size_t max_stderr_bytes{16 * 1024 * 1024};
};

enum class ProcessStatus : std::uint8_t {
  Created = 0,
  Starting,
  Running,
  Exited,
  Signaled,
  TimedOut,
  Cancelled,
  Closing,
  Closed,
};

enum class ProcessStream : std::uint8_t {
  Stdout = 0,
  Stderr,
};

struct OutputCursor {
  std::uint64_t sequence{1};
  std::size_t byte_offset{0};
};

struct ReadOptions {
  OutputCursor cursor{};
  std::size_t max_bytes{64 * 1024};
  std::chrono::milliseconds wait{0};
};

struct OutputChunk {
  std::uint64_t sequence{0};
  ProcessStream stream{ProcessStream::Stdout};
  std::string data;
  std::int64_t observed_at_ms{0};
  std::size_t offset_start{0};
  std::size_t offset_end{0};
};

struct ExitResult {
  ProcessStatus status{ProcessStatus::Created};
  std::optional<std::int32_t> exit_code;
  std::optional<int> signal;
  bool timed_out{false};
};

struct ExitWaitResult {
  ExitResult exit;
  bool completed{false};
};

struct ReadResult {
  ExitResult exit;
  std::vector<OutputChunk> chunks;
  OutputCursor next_cursor{};
  bool eof{false};
  std::size_t returned_bytes{0};
};

struct ProcessSnapshot {
  ExitResult exit;
  bool stdin_available{false};
  std::string stdout_str;
  std::string stderr_str;
  std::uint64_t stdout_bytes{0};
  std::uint64_t stderr_bytes{0};
  bool stdout_truncated{false};
  bool stderr_truncated{false};
};

/** Result of Process::wait() / one-shot run (public fields only). */
struct RunResult {
  std::int32_t exit_code{0};
  bool timed_out{false};
  std::string stdout_str;
  std::string stderr_str;
  /** Total bytes drained from each pipe, including bytes not retained. */
  std::uint64_t stdout_bytes{0};
  std::uint64_t stderr_bytes{0};
  bool stdout_truncated{false};
  bool stderr_truncated{false};
};

/**
 * Decode terminate signal name. Accepts only SIGTERM / SIGINT / SIGKILL
 * (case-sensitive, exact). Empty → SIGTERM.
 */
[[nodiscard]] Result<int> decode_terminate_signal(std::string_view signal);

class Process final : public std::enable_shared_from_this<Process> {
 public:
  /**
   * @param executor  Host main executor (spawn/pipes/timers).
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

  /** Kill still-running child (process group); cancel timers/pipes (nonblocking). */
  ~Process();

  [[nodiscard]] asio::awaitable<VoidResult> start();

  /**
   * Contract: Narrow
   * Preconditions: owner executor; successful start; explicit close not begun.
   * Errors: expected pipe I/O failure.
   * Threading: owner executor only.
   * Lifetime: the coroutine retains the pipe and Process state.
   *
   * Serialized stdin write. When close_stdin is true, the pipe is closed in
   * the same serialized operation after the payload is written.
   */
  [[nodiscard]] asio::awaitable<Result<std::size_t>> write(
      std::string data,
      bool close_stdin = false);

  /**
   * Contract: Narrow
   * Preconditions: owner executor; successful start; explicit close not begun;
   *   cursor/ranges were validated by the caller.
   * Errors: none; deadline expiry is an empty successful read.
   * Threading: owner executor only.
   * Lifetime: returned chunks own their copied bytes.
   */
  [[nodiscard]] asio::awaitable<ReadResult> read(ReadOptions options);

  /**
   * Contract: Narrow
   * Preconditions: owner executor; successful start; explicit close not begun.
   * Errors: none.
   * Threading: owner executor only.
   * Lifetime: returned value owns no Process resources.
   */
  [[nodiscard]] asio::awaitable<ExitWaitResult> wait_for_exit(
      std::optional<std::chrono::milliseconds> timeout = std::nullopt);

  /**
   * Contract: Narrow
   * Preconditions: owner executor; preview sizes are caller-validated.
   * Errors: none.
   * Threading: owner executor only.
   * Lifetime: returned previews own their bytes.
   */
  [[nodiscard]] ProcessSnapshot snapshot(
      std::size_t stdout_preview_bytes,
      std::size_t stderr_preview_bytes) const;

  /**
   * Wait until process exit and both stdout/stderr drains finish.
   * Multiple callers join the same barrier. Returns captured data only while
   * the handle is still open — async_close releases buffers and may cause an
   * outstanding concurrent wait to fail (no retained snapshot outside budget).
   */
  [[nodiscard]] asio::awaitable<Result<RunResult>> wait();

  /**
   * Signal the process group. Resolves after the kill request, not after exit.
   * No-op success if already exited.
   */
  [[nodiscard]] VoidResult terminate(
      int signal,
      std::chrono::milliseconds grace = std::chrono::milliseconds{3000});

  /**
   * Idempotent close: request SIGKILL, cancel stdin+read pipes, await real
   * child-reap and stdout/stderr drain barrier, then release slot/buffers.
   * Concurrent callers join the same barrier. close-before-start → Closed
   * immediately. Never stops the executor.
   */
  [[nodiscard]] asio::awaitable<VoidResult> async_close();

  /**
   * Finalizer/dtor path: request group SIGKILL and cancel pipes/timers on the
   * owner executor; never throws; never blocks. Does not lie about exit/eof
   * flags — detached State ops complete on real I/O/reap.
   */
  void dispose() noexcept;

  [[nodiscard]] bool started() const noexcept;
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] ProcessStatus status() const noexcept;

  [[nodiscard]] const std::vector<std::string>& argv() const noexcept {
    return argv_;
  }
  [[nodiscard]] const StartOptions& options() const noexcept { return opts_; }

 private:
  struct State;

  std::vector<std::string> argv_;
  StartOptions opts_{};
  std::shared_ptr<State> state_;
};

}  // namespace vacps::process
