#include "fs/async.hpp"

#include <utility>

namespace vacps::fs {

asio::awaitable<VoidResult> async_mkdir(AsyncOptions opts, std::filesystem::path path) {
  co_return co_await async_offload(opts.pool, [path = std::move(path)] {
    return mkdir_p(path);
  });
}

asio::awaitable<bool> async_exists(AsyncOptions opts, std::filesystem::path path) {
  co_return co_await async_offload(opts.pool, [path = std::move(path)] {
    return vacps::fs::exists(path);
  });
}

asio::awaitable<VoidResult> async_remove(AsyncOptions opts, std::filesystem::path path) {
  co_return co_await async_offload(opts.pool, [path = std::move(path)] {
    return vacps::fs::remove_path(path);
  });
}

asio::awaitable<VoidResult> async_rename(
    AsyncOptions opts,
    std::filesystem::path from,
    std::filesystem::path to) {
  co_return co_await async_offload(
      opts.pool, [from = std::move(from), to = std::move(to)] {
        return rename_path(from, to);
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
