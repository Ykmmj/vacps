#pragma once

#include "app/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::fs {

/**
 * Product path sandbox for vacps:fs.
 *
 * - Default deny except configured absolute roots (data_dir, /tmp, extras).
 * - Always reject /proc, /sys, /dev (even if listed as roots).
 * - Linux: openat2(RESOLVE_BENEATH) from the matched root dirfd so symlink /
 *   ".." escapes cannot leave the root. Falls back to realpath + prefix check
 *   when openat2 is unavailable (ENOSYS).
 *
 * Pure I/O helpers in fs.hpp stay policy-free for unit tests; Host wires this
 * sandbox into resolve_user_path for the JS module surface.
 */
class PathSandbox {
 public:
  PathSandbox() = default;

  /**
   * Build from data_dir + always /tmp + extra absolute roots.
   * Roots are absolutized and realpath'd when they exist.
   */
  [[nodiscard]] static PathSandbox create(
      const std::filesystem::path& data_dir,
      std::vector<std::string> extra_roots = {});

  /** Empty roots → every path rejected (fail-closed). */
  [[nodiscard]] bool empty() const noexcept { return roots_.empty(); }

  [[nodiscard]] const std::vector<std::filesystem::path>& roots() const noexcept {
    return roots_;
  }

  /**
   * Join relative paths under `relative_base` (usually data_dir), then authorize.
   * Returns a path safe to open with normal open/fstream (real path when known).
   */
  [[nodiscard]] Result<std::filesystem::path> authorize(
      std::string_view user_path,
      const std::filesystem::path& relative_base) const;

 private:
  explicit PathSandbox(std::vector<std::filesystem::path> roots) : roots_(std::move(roots)) {}

  [[nodiscard]] Result<std::filesystem::path> authorize_absolute(
      const std::filesystem::path& abs) const;

  std::vector<std::filesystem::path> roots_;
};

/** True for /proc, /sys, /dev and descendants. */
[[nodiscard]] bool is_kernel_filesystem(const std::filesystem::path& abs) noexcept;

}  // namespace vacps::fs
