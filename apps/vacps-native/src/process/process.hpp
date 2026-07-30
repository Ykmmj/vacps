#pragma once

#include "app/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vacps::process {

namespace asio = boost::asio;

struct RunOptions {
  std::string cwd;               // empty = inherit
  std::vector<std::string> env;  // empty = inherit (v1 unused replace-all)
  /** 0 = no timeout; >0 cancels with termination after ms. */
  std::int32_t timeout_ms{0};
};

struct RunResult {
  std::int32_t exit_code{0};
  bool timed_out{false};
  std::string stdout_str;
  std::string stderr_str;
};

/**
 * Boost.Process v2 + Asio coroutines: capture stdout/stderr, optional timeout.
 * Must be co_awaited on the host io_context executor (no nested reactor).
 */
[[nodiscard]] asio::awaitable<Result<RunResult>> async_run(
    std::vector<std::string> argv,
    RunOptions opts = {});

}  // namespace vacps::process
