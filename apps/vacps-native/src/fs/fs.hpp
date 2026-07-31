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
 * Product policy for vacps:fs JS module is PathSandbox (openat2 + allowlist)
 * in resolve_user_path. MCP tool paths also use JS `runtime/path-guard.ts`.
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

/**
 * Read at most `max_bytes` starting at `offset` (does not load the whole file).
 * Short read at EOF is success with smaller vector.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> read_range(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::size_t max_bytes);

/** Streaming SHA-256 over the full file without buffering all content. */
struct FileDigest {
  std::uint64_t size_bytes{0};
  std::string sha256_hex;
};
[[nodiscard]] Result<FileDigest> hash_file(const std::filesystem::path& path);
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
