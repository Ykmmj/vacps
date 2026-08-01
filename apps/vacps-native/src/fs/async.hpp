#pragma once

#include "app/error.hpp"
#include "fs/executor.hpp"
#include "fs/fs.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <filesystem>
#include <type_traits>
#include <utility>
#include <vector>

namespace vacps::fs {

namespace asio = boost::asio;

/**
 * Options for path helpers (namespace async ops).
 *
 * Only needs the FS thread_pool. Construct from FsExecutor when available:
 *   AsyncOptions opts{fs};
 * or from a bare pool in unit tests:
 *   AsyncOptions opts{pool};
 *
 * File open uses FsExecutor directly (File::async_open / File::open).
 */
struct AsyncOptions {
  asio::thread_pool& pool;

  explicit AsyncOptions(asio::thread_pool& p) noexcept : pool(p) {}
  explicit AsyncOptions(FsExecutor& fs) noexcept : pool(fs.pool) {}
};

/**
 * Run blocking work on the host thread_pool; resume the caller (ioc) when done.
 * Used for mkdir/list/rename/exists and File pool-backend I/O.
 *
 * Completion is bound to the awaiting coroutine's executor (typically the host
 * io_context). Without bind_executor, co_spawn(pool, …, use_awaitable) may
 * resume on a pool thread — unsafe for QuickJS settle and can strand waiters
 * that only run on the ioc (JsTasksTest.ExecAndFsRoutes / File.close hang).
 */
template <class F>
[[nodiscard]] asio::awaitable<std::invoke_result_t<std::decay_t<F>>> async_offload(
    asio::thread_pool& pool,
    F&& f) {
  using R = std::invoke_result_t<std::decay_t<F>>;
  auto resume_ex = co_await asio::this_coro::executor;
  co_return co_await asio::co_spawn(
      pool,
      [fn = std::decay_t<F>(std::forward<F>(f))]() mutable -> asio::awaitable<R> {
        co_return fn();
      },
      asio::bind_executor(resume_ex, asio::use_awaitable));
}

/**
 * Path-based async namespace ops — thin pool offload of sync path helpers.
 *
 * Content I/O is `vacps::fs::File` (handle + OpenMode + dual backend).
 * These cover: mkdir / exists / remove / rename / list / stat.
 *
 * All must be co_awaited on the host io_context. No path policy here — JS
 * path-guard owns allowlists at tool boundaries.
 */
[[nodiscard]] asio::awaitable<VoidResult> async_mkdir(
    AsyncOptions opts,
    std::filesystem::path path,
    MkdirOptions mkdir_opts = {});

[[nodiscard]] asio::awaitable<Result<bool>> async_exists(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<VoidResult> async_remove(
    AsyncOptions opts,
    std::filesystem::path path,
    RemoveOptions remove_opts = {});

[[nodiscard]] asio::awaitable<VoidResult> async_rename(
    AsyncOptions opts,
    std::filesystem::path from,
    std::filesystem::path to,
    RenameOptions rename_opts = {});

[[nodiscard]] asio::awaitable<Result<std::vector<DirEntry>>> async_list(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<Result<FileStat>> async_stat(
    AsyncOptions opts,
    std::filesystem::path path);

}  // namespace vacps::fs
