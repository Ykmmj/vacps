#pragma once

#include "app/error.hpp"
#include "fs/async.hpp"
#include "fs/fs.hpp"
#include "fs/open_options.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::fs {

/**
 * Open file handle — primary I/O API for vacps:fs.
 *
 * Dual backend (selected at open; exclusive per instance):
 * 1. Asio: random_access_file when use_asio_file (probe ok) + executor
 * 2. Pool: private FD + sync I/O offloaded via thread_pool
 *
 * Path policy is JS-only (`script/src/runtime/path-guard.ts` at tool boundaries).
 * C++ resolves paths and does pure open/read/write — no allowlist in native.
 *
 * Flags are Asio file_base bitmasks (numeric values match POSIX open flags on
 * Linux), not string modes. Primary API is awaitable async_*; sync methods are
 * for the pool backend and unit tests.
 *
 * Namespace ops (mkdir/list/rename/…) stay on path helpers in async.hpp.
 * Content I/O is only through this class. Pool FD ownership is private (not a
 * public type or JS binding).
 */
class File final {
 public:
  /**
   * Open a file.
   *
   * @param ex            ioc executor for random_access_file (unused if pool-only)
   * @param pool_fallback host thread_pool; required for pool-backend async I/O
   * @param use_asio_file true only when probe_io_uring() succeeded — never construct
   *                      Asio file objects when false (Docker seccomp throws)
   */
  [[nodiscard]] static Result<std::shared_ptr<File>> open(
      asio::any_io_executor ex,
      asio::thread_pool* pool_fallback,
      bool use_asio_file,
      std::string_view path,
      const OpenOptions& options,
      const std::filesystem::path& relative_base = {});

  /** Convenience: force pool backend (unit tests). */
  [[nodiscard]] static Result<std::shared_ptr<File>> open(
      std::string_view path,
      const OpenOptions& options,
      const std::filesystem::path& relative_base = {}) {
    return open(asio::any_io_executor{}, nullptr, false, path, options, relative_base);
  }

  /**
   * Async open using host context (executor + pool + use_asio_file).
   * Asio path runs on the ioc executor; pool path offloads blocking open.
   */
  [[nodiscard]] static asio::awaitable<Result<std::shared_ptr<File>>> async_open(
      AsyncOptions opts,
      std::string path,
      OpenOptions options,
      std::filesystem::path relative_base = {});

  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&&) = delete;
  File& operator=(File&&) = delete;
  ~File();

  // ── Primary API: awaitable I/O ─────────────────────────────────

  [[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> async_read(
      std::size_t max_bytes);
  [[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> async_read_at(
      std::uint64_t offset,
      std::size_t max_bytes);
  [[nodiscard]] asio::awaitable<Result<std::string>> async_read_text(
      std::size_t max_bytes = SIZE_MAX);
  [[nodiscard]] asio::awaitable<Result<std::size_t>> async_write(
      std::span<const std::uint8_t> data);
  [[nodiscard]] asio::awaitable<Result<std::size_t>> async_write_at(
      std::uint64_t offset,
      std::span<const std::uint8_t> data);
  [[nodiscard]] asio::awaitable<Result<std::size_t>> async_write_text(std::string data);
  [[nodiscard]] asio::awaitable<VoidResult> async_truncate(std::uint64_t size);
  [[nodiscard]] asio::awaitable<Result<FileStat>> async_stat();
  [[nodiscard]] asio::awaitable<VoidResult> async_flush();
  [[nodiscard]] asio::awaitable<VoidResult> async_close();

  // ── Sync I/O (pool backend + unit tests) ───────────────────────

  [[nodiscard]] Result<std::vector<std::uint8_t>> read(std::size_t max_bytes);
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_at(
      std::uint64_t offset,
      std::size_t max_bytes);
  [[nodiscard]] Result<std::size_t> write(std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::size_t> write_at(
      std::uint64_t offset,
      std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::string> read_text(std::size_t max_bytes = SIZE_MAX);
  [[nodiscard]] Result<std::size_t> write_text(std::string_view data);
  [[nodiscard]] VoidResult truncate(std::uint64_t size);
  [[nodiscard]] Result<FileStat> stat();
  [[nodiscard]] VoidResult flush();
  [[nodiscard]] VoidResult close();

  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] bool uses_asio_file() const noexcept { return use_asio_; }
  [[nodiscard]] const std::string& display_path() const noexcept {
    return display_path_;
  }
  /** Open flags used at open (Asio Flags / open(2) bit values on Linux). */
  [[nodiscard]] Flags flags() const noexcept { return flags_; }

 private:
  struct PrivateTag {};
  /** Opaque Asio random_access_file holder (defined in file.cpp). */
  struct AsioState;
  /**
   * Pool-backend FD owner (defined only in file.cpp).
   * Not part of the public FS surface — never expose to bindings / JS.
   */
  struct PoolFd;

  File(
      PrivateTag,
      std::unique_ptr<PoolFd> fd,
      asio::thread_pool* pool,
      std::string display_path,
      Flags flags);
  File(
      PrivateTag,
      std::unique_ptr<AsioState> asio_state,
      asio::thread_pool* pool,
      std::string display_path,
      Flags flags);

  [[nodiscard]] VoidResult ensure_open() const;
  [[nodiscard]] bool flags_append() const noexcept;
  [[nodiscard]] Result<std::uint64_t> current_size() const;

  [[nodiscard]] Result<std::vector<std::uint8_t>> pool_read(std::size_t max_bytes);
  [[nodiscard]] Result<std::vector<std::uint8_t>> pool_read_at(
      std::uint64_t offset,
      std::size_t max_bytes);
  [[nodiscard]] Result<std::size_t> pool_write(std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::size_t> pool_write_at(
      std::uint64_t offset,
      std::span<const std::uint8_t> data);

  [[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> asio_read(
      std::size_t max_bytes);
  [[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> asio_read_at(
      std::uint64_t offset,
      std::size_t max_bytes);
  [[nodiscard]] asio::awaitable<Result<std::size_t>> asio_write(
      std::vector<std::uint8_t> data);
  [[nodiscard]] asio::awaitable<Result<std::size_t>> asio_write_at(
      std::uint64_t offset,
      std::vector<std::uint8_t> data);

  bool use_asio_{false};
  std::unique_ptr<PoolFd> pool_fd_;
  std::unique_ptr<AsioState> asio_;
  asio::thread_pool* pool_{nullptr};
  std::string display_path_;
  Flags flags_{};
  /** Sequential cursor for read/write (not moved by read_at/write_at). */
  std::uint64_t offset_{0};
};

}  // namespace vacps::fs
