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
  bool is_symlink{false};
  std::uint64_t size{0};
};

/** Metadata for vacps:fs.stat / File.stat. */
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

struct MkdirOptions {
  /** false (default): create last component only; true: create_directories. */
  bool recursive{false};
};

struct RemoveOptions {
  /** false (default): file or empty dir only; true: remove_all. */
  bool recursive{false};
};

struct RenameOptions {
  /** false (default): fail if target exists; true: allow replace. */
  bool replace{false};
};

/**
 * Pure path resolution (no product path policy).
 *
 * - Empty path / embedded NUL → error (cannot open).
 * - Absolute: returned lexically normalized.
 * - Relative: joined under workspace_root, then lexically normalized.
 *
 * C++ vacps:fs is pure I/O — no path allowlist here or in the JS module surface.
 */
[[nodiscard]] Result<std::filesystem::path> resolve_path(
    const std::filesystem::path& workspace_root,
    std::string_view user_path);

[[nodiscard]] inline Result<std::filesystem::path> resolve_under(
    const std::filesystem::path& root,
    std::string_view user_path) {
  return resolve_path(root, user_path);
}

/** Build a structured I/O error (message + operation + errno). */
[[nodiscard]] Error make_io_error(
    std::string_view operation,
    int errnum,
    std::string_view path = {});

[[nodiscard]] Error make_io_error(
    std::string_view operation,
    const std::error_code& ec,
    std::string_view path = {});

// Namespace / path ops (content I/O is File).
[[nodiscard]] VoidResult mkdir(
    const std::filesystem::path& path,
    MkdirOptions opts = {});

/** Convenience: mkdir with recursive=true (create_directories). */
[[nodiscard]] inline VoidResult mkdir_p(const std::filesystem::path& path) {
  return mkdir(path, MkdirOptions{.recursive = true});
}

/**
 * true if path exists; false for ENOENT / ENOTDIR;
 * error for permission and other failures.
 */
[[nodiscard]] Result<bool> exists(const std::filesystem::path& path);

[[nodiscard]] VoidResult remove_path(
    const std::filesystem::path& path,
    RemoveOptions opts = {});

[[nodiscard]] VoidResult rename_path(
    const std::filesystem::path& from,
    const std::filesystem::path& to,
    RenameOptions opts = {});

[[nodiscard]] Result<std::vector<DirEntry>> list_dir(
    const std::filesystem::path& path);
[[nodiscard]] Result<FileStat> file_stat(const std::filesystem::path& path);

}  // namespace vacps::fs
