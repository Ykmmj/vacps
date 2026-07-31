#pragma once

#include "app/error.hpp"
#include "fs/fs.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vacps::fs {

/**
 * RAII file descriptor. Move-only; closes on destroy.
 * Used so sandboxed ops never re-open a path string after openat2.
 */
class OwnedFd {
 public:
  OwnedFd() noexcept = default;
  explicit OwnedFd(int fd) noexcept : fd_(fd) {}
  ~OwnedFd() { reset(); }

  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;

  OwnedFd(OwnedFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  OwnedFd& operator=(OwnedFd&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }

  void reset() noexcept;
  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

 private:
  int fd_{-1};
};

/**
 * Product path sandbox for vacps:fs.
 *
 * - Default deny except configured absolute roots (data_dir, /tmp, extras).
 * - Always reject /proc, /sys, /dev.
 * - Linux: all I/O goes through openat2(RESOLVE_BENEATH) (or openat under a
 *   verified parent dirfd). We never "authorize path then reopen by string".
 * - Fallback when openat2 is unavailable: realpath + prefix (weaker).
 *
 * Roots are always injected by the caller — this type never reads getenv.
 */
class PathSandbox {
 public:
  PathSandbox() = default;

  [[nodiscard]] static PathSandbox create(
      const std::filesystem::path& data_dir,
      std::vector<std::string> extra_roots = {});

  [[nodiscard]] bool empty() const noexcept { return roots_.empty(); }

  [[nodiscard]] const std::vector<std::filesystem::path>& roots() const noexcept {
    return roots_;
  }

  /**
   * Authorize only (tests / diagnostics). Prefer the I/O methods below for
   * production paths — they bind the openat2 FD to the operation.
   */
  [[nodiscard]] Result<std::filesystem::path> authorize(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  // ── FD-relative I/O (no TOCTOU re-open by path string) ───────────

  [[nodiscard]] Result<std::string> read_text(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] Result<std::vector<std::uint8_t>> read_bytes(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] Result<std::vector<std::uint8_t>> read_range(
      std::string_view user_path,
      const std::filesystem::path& relative_base,
      std::uint64_t offset,
      std::size_t max_bytes) const;

  [[nodiscard]] Result<FileDigest> hash_file(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] VoidResult write_text(
      std::string_view user_path,
      const std::filesystem::path& relative_base,
      std::string_view data) const;

  [[nodiscard]] VoidResult write_bytes(
      std::string_view user_path,
      const std::filesystem::path& relative_base,
      const std::vector<std::uint8_t>& data) const;

  [[nodiscard]] VoidResult append_text(
      std::string_view user_path,
      const std::filesystem::path& relative_base,
      std::string_view data) const;

  [[nodiscard]] VoidResult mkdir(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] bool exists(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] VoidResult remove(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] VoidResult rename(
      std::string_view from_path,
      std::string_view to_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] Result<std::vector<DirEntry>> list(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  [[nodiscard]] Result<FileStat> stat(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

 private:
  explicit PathSandbox(std::vector<std::filesystem::path> roots) : roots_(std::move(roots)) {}

  [[nodiscard]] Result<std::filesystem::path> authorize_absolute(
      const std::filesystem::path& abs) const;

  /** Open existing path for read (O_RDONLY) under a root via openat2. */
  [[nodiscard]] Result<OwnedFd> open_read_fd(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

  /**
   * Open for write (create/trunc or append). Uses openat2 with O_CREAT when
   * needed; ensures parent dirs via mkdirat under the same root dirfd.
   */
  [[nodiscard]] Result<OwnedFd> open_write_fd(
      std::string_view user_path,
      const std::filesystem::path& relative_base,
      bool append) const;

  enum class OpenMode { Read, WriteTrunc, WriteAppend, Dir, PathOnly };

  [[nodiscard]] Result<OwnedFd> open_relative(
      const std::filesystem::path& root,
      std::string_view rel,
      OpenMode mode) const;

  std::vector<std::filesystem::path> roots_;
};

[[nodiscard]] bool is_kernel_filesystem(const std::filesystem::path& abs) noexcept;

[[nodiscard]] std::vector<std::string> fs_extra_roots_from_env();

}  // namespace vacps::fs
