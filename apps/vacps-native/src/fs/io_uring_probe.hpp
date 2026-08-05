#pragma once

namespace vacps::fs {

/**
 * Process-wide cached check that io_uring is usable end-to-end.
 * Validates setup + submit(NOP) + wait_cqe before any Asio
 * random_access_file is created. Docker seccomp failures select the POSIX
 * fallback backend.
 *
 * Evaluated once on first call (before the first Asio file object).
 */
[[nodiscard]] bool io_uring_available() noexcept;

}  // namespace vacps::fs
