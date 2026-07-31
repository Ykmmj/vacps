#pragma once

#include "app/error.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vacps::fs {

struct DirEntry {
  std::string name;
  bool is_dir{false};
  bool is_file{false};
  std::uint64_t size{0};
};

/** Metadata for vacps:fs.stat. */
struct FileStat {
  std::string path;
  /** "file" | "directory" | "symlink" | "other" */
  std::string type;
  std::uint64_t size_bytes{0};
  /** Unix epoch milliseconds (mtime). */
  std::int64_t modified_at_ms{0};
  bool readable{false};
  bool writable{false};
  bool is_symlink{false};
};

/**
 * Pure path resolution (no product policy / no allowlist).
 *
 * - Empty path / embedded NUL → error (cannot open).
 * - Absolute: returned lexically normalized.
 * - Relative: joined under workspace_root, then lexically normalized.
 *
 * Path allowlist (dataDir, /tmp, reject /proc, …) is JS only:
 * `script/src/runtime/path-guard.ts` at MCP/tool boundaries.
 * C++ vacps:fs is pure I/O.
 */
[[nodiscard]] Result<std::filesystem::path> resolve_path(
    const std::filesystem::path& workspace_root,
    std::string_view user_path);

[[nodiscard]] inline Result<std::filesystem::path> resolve_under(
    const std::filesystem::path& root,
    std::string_view user_path) {
  return resolve_path(root, user_path);
}

// Namespace / path ops (content I/O is File).
[[nodiscard]] VoidResult mkdir_p(const std::filesystem::path& path);
[[nodiscard]] bool exists(const std::filesystem::path& path);
[[nodiscard]] VoidResult remove_path(const std::filesystem::path& path);
[[nodiscard]] VoidResult rename_path(
    const std::filesystem::path& from,
    const std::filesystem::path& to);
[[nodiscard]] Result<std::vector<DirEntry>> list_dir(const std::filesystem::path& path);
[[nodiscard]] Result<FileStat> file_stat(const std::filesystem::path& path);

}  // namespace vacps::fs
