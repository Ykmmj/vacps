#pragma once

/**
 * vacps::storage::Store — one SQLite connection owned by a JS Store instance.
 *
 * Serialization: every public method holds `mutex_` for the duration of the
 * Database call. This is the per-Store serialization mechanism so concurrent
 * offloaded jobs for the same Store cannot interleave SQL on the connection.
 * Different Store instances may still run in parallel on the shared host pool.
 */

#include "app/error.hpp"
#include "storage/database.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vacps::storage {

struct OpenOptions {
  /** Defaults to ReadWriteCreate when absent. */
  std::optional<OpenMode> mode;
};

struct RunResult {
  std::int64_t changes{0};
  std::int64_t last_insert_rowid{0};
};

struct ExpectedChanges {
  enum class Kind { Exactly, AtLeast, AtMost };
  Kind kind{Kind::Exactly};
  std::int64_t n{0};
};

enum class StepType {
  Run,    // single statement with optional binds (DML/DDL)
  Query,  // SELECT / result set
};

struct TransactionStep {
  std::string sql;
  std::vector<SqlValue> params;
  StepType type{StepType::Run};
  /**
   * Only valid for Run steps. Query + expected_changes is rejected by the
   * binding; domain skips the changes() check for Query steps.
   */
  std::optional<ExpectedChanges> expected_changes;
  /** Only used for Query steps; default matches Database::kDefaultMaxQueryRows. */
  std::size_t max_rows{Database::kDefaultMaxQueryRows};
};

/** Per-step result: RunResult for run, QueryResult for query. */
using TransactionResult = std::variant<RunResult, QueryResult>;

class Store {
 public:
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;
  ~Store();

  /**
   * Open a new connection via Database::open, applying OpenOptions::mode
   * (default ReadWriteCreate). Creates parent directories when mode is
   * ReadWriteCreate so the DB file can be created under a new tree.
   */
  [[nodiscard]] static Result<std::shared_ptr<Store>> open(
      std::string path,
      OpenOptions options = {});

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool closed() const noexcept;

  [[nodiscard]] VoidResult exec(std::string_view sql);
  [[nodiscard]] Result<RunResult> run(
      std::string_view sql,
      const std::vector<SqlValue>& params = {});
  /**
   * @param max_rows  Cap on returned rows (Database enforces; default 10_000).
   * @param max_bytes Optional approximate payload budget (columns + cell bytes).
   *                  Enforced during materialization (not only after full result).
   *                  Pair with max_rows for a hard row cap.
   */
  [[nodiscard]] Result<QueryResult> query(
      std::string_view sql,
      const std::vector<SqlValue>& params = {},
      std::size_t max_rows = Database::kDefaultMaxQueryRows,
      std::optional<std::size_t> max_bytes = std::nullopt);

  /**
   * BEGIN IMMEDIATE … steps … COMMIT on this connection under mutex_.
   * After each Run step with expected_changes, check immediately;
   * mismatch → ROLLBACK (via with_transaction) and error — no later steps run.
   * expected_changes on Query steps is ignored (binding rejects it for JS).
   */
  [[nodiscard]] Result<std::vector<TransactionResult>> transaction(
      const std::vector<TransactionStep>& steps);

  /**
   * Idempotent: release the sqlite connection.
   * Thread-safe (mutex_). Safe to call from a GC finalizer without ScriptRuntime —
   * sync sqlite close on the caller thread is intentional.
   */
  [[nodiscard]] VoidResult close();

 private:
  explicit Store(std::unique_ptr<Database> db, std::string path, OpenOptions options);

  [[nodiscard]] VoidResult ensure_open() const;
  [[nodiscard]] static VoidResult check_expected_changes(
      const ExpectedChanges& exp,
      std::int64_t changes);

  // Locked around every Database use — per-Store serialization.
  mutable std::mutex mutex_;
  std::unique_ptr<Database> db_;
  std::string path_;
  OpenOptions options_;
};

}  // namespace vacps::storage
