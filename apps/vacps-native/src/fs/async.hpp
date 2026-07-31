#pragma once

#include "app/error.hpp"
#include "fs/fs.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <filesystem>
#include <type_traits>
#include <utility>
#include <vector>

namespace vacps::fs {

namespace asio = boost::asio;

/**
 * Options shared by path helpers and File::async_open.
 *
 * - pool: always used for namespace/metadata ops and pool-backend File I/O
 * - executor: ioc executor for Asio random_access_file construction (File only)
 * - use_asio_file: true only after probe_io_uring(); never trust compile-time alone
 *
 * Namespace async_* functions only need `pool`. File handles use the dual backend
 * (Asio random_access_file when use_asio_file + executor; else pool + private FD).
 */
struct AsyncOptions {
  asio::thread_pool& pool;
  asio::any_io_executor executor{};
  bool use_asio_file{false};
};

/**
 * Run blocking work on the host thread_pool; resume the caller (ioc) when done.
 * Used for mkdir/list/rename/exists and File pool-backend I/O.
 */
template <class F>
[[nodiscard]] asio::awaitable<std::invoke_result_t<std::decay_t<F>>> async_offload(
    asio::thread_pool& pool,
    F&& f) {
  using R = std::invoke_result_t<std::decay_t<F>>;
  co_return co_await asio::co_spawn(
      pool,
      [fn = std::decay_t<F>(std::forward<F>(f))]() mutable -> asio::awaitable<R> {
        co_return fn();
      },
      asio::use_awaitable);
}

/**
 * Path-based async namespace ops — thin pool offload of sync path helpers.
 *
 * Content I/O is `vacps::fs::File` (handle + Asio flags + dual backend).
 * These cover: mkdir / exists / remove / rename / list / stat.
 *
 * All must be co_awaited on the host io_context. No path policy here — JS
 * path-guard owns allowlists at tool boundaries.
 */
[[nodiscard]] asio::awaitable<VoidResult> async_mkdir(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<bool> async_exists(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<VoidResult> async_remove(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<VoidResult> async_rename(
    AsyncOptions opts,
    std::filesystem::path from,
    std::filesystem::path to);

[[nodiscard]] asio::awaitable<Result<std::vector<DirEntry>>> async_list(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<Result<FileStat>> async_stat(
    AsyncOptions opts,
    std::filesystem::path path);

}  // namespace vacps::fs
