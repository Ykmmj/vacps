#pragma once

#include "app/error.hpp"
#include "fs/fs.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace vacps::fs {

namespace asio = boost::asio;

/** Runtime choice: stream_file only when probe_io_uring() succeeded. */
struct AsyncOptions {
  asio::thread_pool& pool;
  /** true only after probe_io_uring(); never trust compile-time alone. */
  bool use_stream_file{false};
};

/**
 * Run blocking work on the host thread_pool; resume the caller (ioc) when done.
 * Used for mkdir/list/rename/exists and content fallback when io_uring unusable.
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
 * Content I/O: Asio stream_file when opts.use_stream_file; else thread_pool.
 * Directory/metadata ops always use thread_pool.
 * Must be co_awaited on the host io_context.
 *
 * Note: Asio has no automatic file→thread_pool fallback. Docker default seccomp
 * blocks io_uring_setup → stream_file ctor throws; incomplete profiles that
 * allow setup but block enter can leave ops permanently pending — so we probe
 * setup+submit+wait before ever enabling stream_file.
 */
[[nodiscard]] asio::awaitable<Result<std::string>> async_read_text(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> async_read_bytes(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> async_read_range(
    AsyncOptions opts,
    std::filesystem::path path,
    std::uint64_t offset,
    std::size_t max_bytes);

[[nodiscard]] asio::awaitable<Result<FileDigest>> async_hash_file(
    AsyncOptions opts,
    std::filesystem::path path);

[[nodiscard]] asio::awaitable<VoidResult> async_write_text(
    AsyncOptions opts,
    std::filesystem::path path,
    std::string data);

[[nodiscard]] asio::awaitable<VoidResult> async_write_bytes(
    AsyncOptions opts,
    std::filesystem::path path,
    std::vector<std::uint8_t> data);

[[nodiscard]] asio::awaitable<VoidResult> async_append_text(
    AsyncOptions opts,
    std::filesystem::path path,
    std::string data);

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
