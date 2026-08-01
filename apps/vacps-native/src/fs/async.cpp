#include "fs/async.hpp"

#include <utility>

namespace vacps::fs {

asio::awaitable<VoidResult> async_mkdir(
    AsyncOptions opts,
    std::filesystem::path path,
    MkdirOptions mkdir_opts) {
  co_return co_await async_offload(
      opts.pool, [path = std::move(path), mkdir_opts] {
        return mkdir(path, mkdir_opts);
      });
}

asio::awaitable<Result<bool>> async_exists(AsyncOptions opts, std::filesystem::path path) {
  co_return co_await async_offload(opts.pool, [path = std::move(path)] {
    return vacps::fs::exists(path);
  });
}

asio::awaitable<VoidResult> async_remove(
    AsyncOptions opts,
    std::filesystem::path path,
    RemoveOptions remove_opts) {
  co_return co_await async_offload(
      opts.pool, [path = std::move(path), remove_opts] {
        return vacps::fs::remove_path(path, remove_opts);
      });
}

asio::awaitable<VoidResult> async_rename(
    AsyncOptions opts,
    std::filesystem::path from,
    std::filesystem::path to,
    RenameOptions rename_opts) {
  co_return co_await async_offload(
      opts.pool,
      [from = std::move(from), to = std::move(to), rename_opts] {
        return rename_path(from, to, rename_opts);
      });
}

asio::awaitable<Result<std::vector<DirEntry>>> async_list(
    AsyncOptions opts,
    std::filesystem::path path) {
  co_return co_await async_offload(opts.pool, [path = std::move(path)] {
    return list_dir(path);
  });
}

asio::awaitable<Result<FileStat>> async_stat(
    AsyncOptions opts,
    std::filesystem::path path) {
  co_return co_await async_offload(opts.pool, [path = std::move(path)] {
    return file_stat(path);
  });
}

}  // namespace vacps::fs
