#pragma once

namespace vacps::fs {

/**
 * Runtime check that io_uring is usable end-to-end:
 * setup + submit(NOP) + wait_cqe.
 *
 * Docker default seccomp blocks io_uring_setup → false (EPERM).
 * Incomplete profiles that allow setup but block enter also → false.
 *
 * Does not use Boost.Asio; validates syscalls before any random_access_file
 * is created. Asio itself has no thread_pool fallback for files — callers
 * must branch on this result (or on open() throwing).
 */
[[nodiscard]] bool probe_io_uring() noexcept;

}  // namespace vacps::fs
