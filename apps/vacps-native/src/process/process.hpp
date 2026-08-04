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
  TimedOut,
  Cancelled,
  Closing,
  Closed,
};

/** Result of Process::wait() / one-shot run (public fields only). */
struct RunResult {
  std::int32_t exit_code{0};
  bool timed_out{false};
  std::string stdout_str;
  std::string stderr_str;
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

  /** Serialized stdin write. Does not close stdin. */
  [[nodiscard]] asio::awaitable<Result<std::size_t>> write(std::string data);

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
  [[nodiscard]] VoidResult terminate(int signal);

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
