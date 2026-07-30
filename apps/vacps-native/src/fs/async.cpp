#include "fs/async.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <array>
#include <format>
#include <utility>

#if defined(BOOST_ASIO_HAS_IO_URING)
#include <boost/asio/stream_file.hpp>
#define VACPS_FS_HAS_STREAM_FILE 1
#else
#define VACPS_FS_HAS_STREAM_FILE 0
#endif

namespace vacps::fs {
namespace {

#if VACPS_FS_HAS_STREAM_FILE

asio::awaitable<Result<std::string>> stream_read_all(const std::filesystem::path& path) {
  auto executor = co_await asio::this_coro::executor;
  // May throw system_error if io_uring_queue_init fails (e.g. EPERM).
  asio::stream_file file(executor);
  boost::system::error_code ec;
  file.open(path.string(), asio::stream_file::read_only, ec);
  if (ec) {
    co_return std::unexpected(
        Error{std::format("read failed ({}): {}", path.string(), ec.message())});
  }

  std::string out;
  for (;;) {
    std::array<char, 16384> buf{};
    auto [rec, n] = co_await file.async_read_some(
        asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
    if (rec == asio::error::eof) {
      break;
    }
    if (rec) {
      co_return std::unexpected(
          Error{std::format("read failed ({}): {}", path.string(), rec.message())});
    }
    out.append(buf.data(), n);
  }
  co_return out;
}

asio::awaitable<VoidResult> stream_write_all(
    const std::filesystem::path& path,
    std::string_view data,
    bool append) {
  auto executor = co_await asio::this_coro::executor;
  asio::stream_file file(executor);
  boost::system::error_code ec;
  const auto flags = append
                         ? (asio::stream_file::write_only | asio::stream_file::create |
                            asio::stream_file::append)
                         : (asio::stream_file::write_only | asio::stream_file::create |
                            asio::stream_file::truncate);
  file.open(path.string(), flags, ec);
  if (ec) {
    co_return std::unexpected(Error{std::format(
        "{} failed ({}): {}", append ? "append" : "write", path.string(), ec.message())});
  }
  if (data.empty()) {
    co_return VoidResult{};
  }
  auto [wec, n] = co_await asio::async_write(
      file, asio::buffer(data.data(), data.size()), asio::as_tuple(asio::use_awaitable));
  if (wec) {
    co_return std::unexpected(Error{std::format(
        "{} failed ({}): {}", append ? "append" : "write", path.string(), wec.message())});
  }
  (void)n;
  co_return VoidResult{};
}

asio::awaitable<VoidResult> ensure_parent_dirs(
    asio::thread_pool& pool,
    const std::filesystem::path& path) {
  if (!path.has_parent_path()) {
    co_return VoidResult{};
  }
  const auto parent = path.parent_path();
  co_return co_await async_offload(pool, [parent] { return mkdir_p(parent); });
}

#endif  // VACPS_FS_HAS_STREAM_FILE

}  // namespace

asio::awaitable<Result<std::string>> async_read_text(
    AsyncOptions opts,
    std::filesystem::path path) {
#if VACPS_FS_HAS_STREAM_FILE
  if (opts.use_stream_file) {
    try {
      co_return co_await stream_read_all(path);
    } catch (const boost::system::system_error& e) {
      // Fail-closed to pool for this call (e.g. late EPERM); never leave Promise hanging.
      (void)e;
    }
  }
#endif
  co_return co_await async_offload(
      opts.pool, [path = std::move(path)] { return read_text(path); });
}

asio::awaitable<Result<std::vector<std::uint8_t>>> async_read_bytes(
    AsyncOptions opts,
    std::filesystem::path path) {
  auto text = co_await async_read_text(opts, std::move(path));
  if (!text) {
    co_return std::unexpected(std::move(text.error()));
  }
  co_return std::vector<std::uint8_t>(text->begin(), text->end());
}

asio::awaitable<VoidResult> async_write_text(
    AsyncOptions opts,
    std::filesystem::path path,
    std::string data) {
#if VACPS_FS_HAS_STREAM_FILE
  if (opts.use_stream_file) {
    try {
      if (auto p = co_await ensure_parent_dirs(opts.pool, path); !p) {
        co_return p;
      }
      co_return co_await stream_write_all(path, data, /*append=*/false);
    } catch (const boost::system::system_error&) {
      // fall through to pool
    }
  }
#endif
  co_return co_await async_offload(
      opts.pool, [path = std::move(path), data = std::move(data)] {
        return write_text(path, data);
      });
}

asio::awaitable<VoidResult> async_write_bytes(
    AsyncOptions opts,
    std::filesystem::path path,
    std::vector<std::uint8_t> data) {
  std::string s(reinterpret_cast<const char*>(data.data()), data.size());
  co_return co_await async_write_text(opts, std::move(path), std::move(s));
}

asio::awaitable<VoidResult> async_append_text(
    AsyncOptions opts,
    std::filesystem::path path,
    std::string data) {
#if VACPS_FS_HAS_STREAM_FILE
  if (opts.use_stream_file) {
    try {
      if (auto p = co_await ensure_parent_dirs(opts.pool, path); !p) {
        co_return p;
      }
      co_return co_await stream_write_all(path, data, /*append=*/true);
    } catch (const boost::system::system_error&) {
      // fall through to pool
    }
  }
#endif
  co_return co_await async_offload(
      opts.pool, [path = std::move(path), data = std::move(data)] {
        return append_text(path, data);
      });
}

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
