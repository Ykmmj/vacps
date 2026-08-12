#pragma once

/**
 * Linux PTY-backed interactive terminal.
 *
 * A Terminal owns one UNIX 98 PTY master and one child session/process group.
 * stdout and stderr are intentionally merged by the terminal slave. Output is
 * retained as raw bytes in a bounded rolling buffer addressed by absolute byte
 * offsets; callers can detect loss when their cursor falls behind.
 */

#include "app/error.hpp"
#include "process/budget.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace vacps::terminal {

namespace asio = boost::asio;

struct EnvironmentVariable {
  std::string name;
  std::string value;
};

struct StartOptions {
  std::string cwd;
  /** Inherited process environment overrides; names/values are caller-validated. */
  std::vector<EnvironmentVariable> environment;
  std::uint16_t columns{80};
  std::uint16_t rows{24};
  /** 0 = no runtime deadline. */
  std::chrono::milliseconds timeout{0};
  /** Rolling raw-output retention for this terminal. */
  std::size_t max_buffer_bytes{4 * 1024 * 1024};
};

enum class TerminalStatus : std::uint8_t {
  Created = 0,
  Starting,
  Running,
  Exited,
  Signaled,
  TimedOut,
};

struct ExitResult {
  TerminalStatus status{TerminalStatus::Created};
  std::optional<std::int32_t> exit_code;
  std::optional<int> signal;
  bool timed_out{false};
};

struct ExitWaitResult {
  ExitResult exit;
  bool completed{false};
};

struct ReadOptions {
  std::uint64_t offset{0};
  std::size_t max_bytes{64 * 1024};
  std::chrono::milliseconds wait{0};
};

struct ReadResult {
  ExitResult exit;
  std::vector<std::uint8_t> data;
  std::uint64_t next_offset{0};
  std::uint64_t available_from{0};
  std::uint64_t dropped_bytes{0};
  bool dropped{false};
  bool eof{false};
};

struct CloseResult {
  ExitResult exit;
  bool escalated{false};
  std::optional<int> final_signal;
};

struct TerminalSnapshot {
  ExitResult exit;
  std::uint16_t columns{0};
  std::uint16_t rows{0};
  /** Current PTY slave VERASE byte used to encode semantic Backspace input. */
  std::uint8_t erase_character{0x7f};
  std::uint64_t next_offset{0};
  std::uint64_t available_from{0};
  std::size_t buffered_bytes{0};
};

/** Exact, case-sensitive terminal signal names. */
[[nodiscard]] Result<int> decode_signal(std::string_view signal);

class Terminal final : public std::enable_shared_from_this<Terminal> {
 public:
  Terminal(
      asio::any_io_executor executor,
      std::shared_ptr<process::ProcessBudget> budget,
      std::vector<std::string> argv,
      StartOptions options = {});

  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;
  Terminal(Terminal&&) = delete;
  Terminal& operator=(Terminal&&) = delete;

  /** Nonblocking finalizer fallback: kill the process group and cancel PTY I/O. */
  ~Terminal();

  /**
   * Contract: Narrow
   * Preconditions: owner executor; called exactly once; argv[0] is executable.
   */
  [[nodiscard]] asio::awaitable<VoidResult> start();

  /**
   * Contract: Narrow
   * Preconditions: owner executor; live started terminal; data size was
   * validated by the caller. Writes are serialized.
   */
  [[nodiscard]] asio::awaitable<Result<std::size_t>> write(
      std::vector<std::uint8_t> data,
      std::stop_token stop = {});

  /**
   * Contract: Narrow
   * Preconditions: owner executor; started and not explicitly closed;
   * max_bytes/wait are caller-validated.
   */
  [[nodiscard]] asio::awaitable<ReadResult> read(
      ReadOptions options,
      std::stop_token stop = {});

  /** Set kernel PTY window size; the kernel delivers SIGWINCH. */
  [[nodiscard]] VoidResult resize(std::uint16_t columns, std::uint16_t rows);

  /** Send an exact supported signal to the PTY foreground process group. */
  [[nodiscard]] VoidResult signal(int signo);

  [[nodiscard]] TerminalSnapshot snapshot() const;

  [[nodiscard]] asio::awaitable<ExitWaitResult> wait_for_exit(
      std::optional<std::chrono::milliseconds> timeout = std::nullopt,
      std::stop_token stop = {});

  /** SIGHUP, grace, SIGKILL, then await real reap and PTY EOF. Idempotent. */
  [[nodiscard]] asio::awaitable<CloseResult> async_close(
      std::chrono::milliseconds grace = std::chrono::milliseconds{1000});

  void dispose() noexcept;

 private:
  struct State;

  std::vector<std::string> argv_;
  StartOptions options_;
  std::shared_ptr<State> state_;
};

}  // namespace vacps::terminal
