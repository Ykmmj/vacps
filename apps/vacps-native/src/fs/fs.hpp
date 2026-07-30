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
 * Pure path resolution (no product policy).
 *
 * - Empty path / embedded NUL → error (cannot open).
 * - Absolute: returned lexically normalized.
 * - Relative: joined under workspace_root, then lexically normalized.
 *
 * MCP / tool path policy (forbid /proc, workspace escape, …) lives in JS
 * `runtime/path-guard.ts`, not here.
 */
[[nodiscard]] Result<std::filesystem::path> resolve_path(
    const std::filesystem::path& workspace_root,
    std::string_view user_path);

[[nodiscard]] inline Result<std::filesystem::path> resolve_under(
    const std::filesystem::path& root,
    std::string_view user_path) {
  return resolve_path(root, user_path);
}

[[nodiscard]] Result<std::string> read_text(const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_bytes(const std::filesystem::path& path);
[[nodiscard]] VoidResult write_text(const std::filesystem::path& path, std::string_view data);
[[nodiscard]] VoidResult write_bytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data);
[[nodiscard]] VoidResult append_text(const std::filesystem::path& path, std::string_view data);
[[nodiscard]] VoidResult mkdir_p(const std::filesystem::path& path);
[[nodiscard]] bool exists(const std::filesystem::path& path);
[[nodiscard]] VoidResult remove_path(const std::filesystem::path& path);
[[nodiscard]] VoidResult rename_path(
    const std::filesystem::path& from,
    const std::filesystem::path& to);
[[nodiscard]] Result<std::vector<DirEntry>> list_dir(const std::filesystem::path& path);
[[nodiscard]] Result<FileStat> file_stat(const std::filesystem::path& path);

}  // namespace vacps::fs
