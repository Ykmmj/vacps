#pragma once

#include "app/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace vacps::process {

namespace asio = boost::asio;

inline constexpr std::size_t kDefaultRunMaxStdoutBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kDefaultRunMaxStderrBytes = 16u * 1024u * 1024u;

struct RunOptions {
  std::string cwd;               // empty = inherit
  std::vector<std::string> env;  // empty = inherit (v1 unused replace-all)
  /** 0 = no timeout; >0 cancels with termination after ms. */
  std::int32_t timeout_ms{0};
  /** Max retained stdout bytes (0 = default 16 MiB). Excess is discarded. */
  std::size_t max_stdout_bytes{kDefaultRunMaxStdoutBytes};
  /** Max retained stderr bytes (0 = default 16 MiB). */
  std::size_t max_stderr_bytes{kDefaultRunMaxStderrBytes};
};

struct RunResult {
  std::int32_t exit_code{0};
  /** True only when cancel_after / timeout timer aborted the wait — not exit codes. */
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
 * Boost.Process v2 + Asio coroutines: capture stdout/stderr, optional timeout.
 * Must be co_awaited on the host io_context executor (no nested reactor).
 */
[[nodiscard]] asio::awaitable<Result<RunResult>> async_run(
    std::vector<std::string> argv,
    RunOptions opts = {});

}  // namespace vacps::process
