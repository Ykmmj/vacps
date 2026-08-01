#pragma once

#include "app/error.hpp"
#include "fs/async.hpp"
#include "fs/executor.hpp"
#include "fs/fs.hpp"
#include "fs/open_options.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::fs {

/**
 * Open file handle — primary I/O API for vacps:fs.
 *
 * ## Dual backend (product requirement — keep both)
 *
 * Selected once at open; exclusive per instance for the lifetime of the File:
 *
 * 1. **Asio** — `boost::asio::random_access_file` when `use_asio_file` is true
 *    (runtime `probe_io_uring()` succeeded) and an ioc executor is provided.
 *    Preferred path for non-blocking file I/O on hosts with working io_uring.
 * 2. **Pool** — private FD + sync POSIX I/O offloaded on `thread_pool`.
 *    Required fallback when io_uring is unavailable or blocked (e.g. Docker
 *    default seccomp → setup EPERM). Constructing Asio file objects in that
 *    environment throws; we never probe-fail and still open Asio.
 *
 * This is **not** transitional dual-path junk and must not be “simplified” to
 * a single backend. Review feedback that asks for “blocking POSIX only first”
 * rejects the product constraint: we need io_uring where it works **and** a
 * reliable pool path where it does not, behind one JS/C++ File surface.
 *
 * ## Concurrency (per-File serialization)
 *
 * Every async op (read/write/readAt/writeAt/truncate/stat/flush/close) acquires
 * a per-File exclusive async lock (wait queue on `strand_`) for the whole
 * logical operation, including nested awaits. That prevents races on
 * `offset_`, the FD / `random_access_file`, and the life state when multiple
 * promises touch the same handle.
 *
 * - **Pool backend:** lock held while blocking POSIX I/O runs on the host
 *   `thread_pool` (lambdas capture `shared_ptr<File>`, never bare `this` alone).
 * - **Asio backend:** lock held across `async_read_some_at` / `async_write_some_at`
 *   awaits so sequential cursor updates and handle lifetime stay coherent.
 *
 * Lifetime state: Open → Closing → Closed. Entering Closing/Closed rejects new
 * I/O; close waits for the exclusive lock (after prior ops) then releases the
 * handle; close is idempotent.
 *
 * Known Asio API limit (document, do not pretend to unify via fake wrappers):
 * pool open passes create permissions to `open(2)`; Asio
 * `random_access_file::open(path, flags)` does not take user mode bits and
 * uses the library default for create. Callers that need exact create mode
 * on both backends must accept that Asio limitation until Asio exposes mode.
 *
 * Path policy is JS-only (`script/src/runtime/path-guard.ts` at tool
 * boundaries). C++ resolves paths and does pure open/read/write — no
 * allowlist in native.
 *
 * Primary open API uses `OpenMode` string modes (mapped to Asio Flags /
 * POSIX bits inside open). Primary I/O API is awaitable `async_*`; sync
 * methods are for the pool backend and unit tests.
 *
 * Namespace ops (mkdir/list/rename/…) stay on path helpers in async.hpp.
 * Content I/O is only through this class. Pool FD ownership is private (not a
 * public type or JS binding).
 */
class File final : public std::enable_shared_from_this<File> {
 public:
  /** Default cap when maxBytes is omitted / SIZE_MAX (16 MiB). */
  static constexpr std::size_t kDefaultMaxReadBytes = 16ull * 1024 * 1024;
  /** Hard reject for any read maxBytes above this (64 MiB). */
  static constexpr std::size_t kHardMaxReadBytes = 64ull * 1024 * 1024;

  /**
   * Open a file with host FS context (primary public API).
   *
   * Uses `fs.ioc_executor` / `fs.use_asio_file` for Asio backend selection,
   * `fs.pool` for pool-backend I/O. When `relative_base` is empty, resolves
   * relative paths under `fs.data_dir`.
   */
  [[nodiscard]] static Result<std::shared_ptr<File>> open(
      FsExecutor& fs,
      std::string_view path,
      const OpenOptions& options,
      const std::filesystem::path& relative_base = {});

  /**
   * Convenience: force pool backend (unit tests). No host pool → sync-only
   * until a pool is provided via the FsExecutor overload.
   */
  [[nodiscard]] static Result<std::shared_ptr<File>> open(
      std::string_view path,
      const OpenOptions& options,
      const std::filesystem::path& relative_base = {});

  /**
   * Async open using FsExecutor.
   * Asio path runs on the ioc executor; pool path offloads blocking open.
   */
  [[nodiscard]] static asio::awaitable<Result<std::shared_ptr<File>>> async_open(
      FsExecutor& fs,
      std::string path,
      OpenOptions options,
      std::filesystem::path relative_base = {});

  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&&) = delete;
  File& operator=(File&&) = delete;
  ~File();

  // ── Primary API: awaitable I/O ─────────────────────────────────

  /**
   * @param max_bytes  SIZE_MAX → kDefaultMaxReadBytes; > kHardMaxReadBytes rejected.
   */
  [[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> async_read(
      std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)());
  [[nodiscard]] asio::awaitable<Result<std::vector<std::uint8_t>>> async_read_at(
      std::uint64_t offset,
      std::size_t max_bytes);
  [[nodiscard]] asio::awaitable<Result<std::string>> async_read_text(
      std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)());
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

  [[nodiscard]] Result<std::vector<std::uint8_t>> read(
      std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)());
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_at(
      std::uint64_t offset,
      std::size_t max_bytes);
  [[nodiscard]] Result<std::size_t> write(std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::size_t> write_at(
      std::uint64_t offset,
      std::span<const std::uint8_t> data);
  [[nodiscard]] Result<std::string> read_text(
      std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)());
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
  /** Open mode used at open (string OpenMode). */
  [[nodiscard]] OpenMode open_mode() const noexcept { return open_mode_; }
  /** Internal Asio/POSIX flags derived from open_mode (for tests / diagnostics). */
  [[nodiscard]] Flags flags() const noexcept { return flags_; }

 private:
  enum class Life : std::uint8_t { Open = 0, Closing = 1, Closed = 2 };

  struct PrivateTag {};
  /** Opaque Asio random_access_file holder (defined in file.cpp). */
  struct AsioState;
  /**
   * Pool-backend FD owner (defined only in file.cpp).
   * Not part of the public FS surface — never expose to bindings / JS.
   */
  struct PoolFd;

  /**
   * Implementation open (detail). Prefer FsExecutor overloads.
   * Kept for unit tests that pass pool + use_asio_file explicitly.
   */
  [[nodiscard]] static Result<std::shared_ptr<File>> open_impl(
      asio::any_io_executor ex,
      asio::thread_pool* pool_fallback,
      bool use_asio_file,
      std::string_view path,
      const OpenOptions& options,
      const std::filesystem::path& relative_base);

  File(
      PrivateTag,
      std::unique_ptr<PoolFd> fd,
      asio::thread_pool* pool,
      asio::any_io_executor strand_ex,
      std::string display_path,
      OpenMode open_mode,
      Flags flags);
  File(
      PrivateTag,
      std::unique_ptr<AsioState> asio_state,
      asio::thread_pool* pool,
      asio::any_io_executor strand_ex,
      std::string display_path,
      OpenMode open_mode,
      Flags flags);

  [[nodiscard]] VoidResult ensure_open() const;
  [[nodiscard]] bool flags_append() const noexcept;
  [[nodiscard]] Result<std::uint64_t> current_size() const;
  /** Resolve SIZE_MAX → default; reject above hard max. */
  [[nodiscard]] static Result<std::size_t> resolve_read_max(std::size_t max_bytes);
  /** Release FD / Asio handle (no life_ transition). */
  void close_handles() noexcept;
  /** Open → Closing → close_handles → Closed (idempotent). */
  VoidResult close_impl();

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
  /**
   * Asio backend: serialize ops on this strand (ioc executor).
   * Pool backend: strand unused for I/O; pool_mu_ serializes pool_* + life_.
   */
  asio::strand<asio::any_io_executor> strand_;
  std::string display_path_;
  OpenMode open_mode_{OpenMode::read};
  Flags flags_{};
  /** Sequential cursor for read/write (not moved by read_at/write_at). */
  std::uint64_t offset_{0};
  /** Open → Closing → Closed; Closing/Closed reject new I/O. */
  std::atomic<Life> life_{Life::Open};
  /**
   * Serializes pool-backend I/O and close_impl. Never held across co_await —
   * only inside async_offload lambdas / sync methods (avoids strand+timer hang).
   */
  mutable std::mutex pool_mu_;
};

}  // namespace vacps::fs
