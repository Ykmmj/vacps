#pragma once

/**
 * Process-wide bootstrap initialization (signals, etc.).
 * Call once at the start of production main and the test runner main,
 * before any worker threads are started.
 */

#include "app/error.hpp"

namespace vacps::bootstrap {

/**
 * Install process-wide runtime policy that must be in place before threads
 * start. Currently: SIGPIPE = SIG_IGN via sigaction so a write to a closed
 * pipe/socket yields EPIPE instead of terminating the process.
 *
 * Returns an Error on failure; callers must exit nonzero without starting
 * the rest of the runtime.
 */
[[nodiscard]] VoidResult initialize_process() noexcept;

}  // namespace vacps::bootstrap
