#pragma once

#include "app/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace vacps {

/** One bound / result cell. No domain meaning — pure SQL types. */
using SqlValue = std::variant<std::monostate, std::int64_t, double, std::string, std::vector<std::uint8_t>>;

inline SqlValue sql_null() { return std::monostate{}; }
inline SqlValue sql_int(std::int64_t v) { return v; }
inline SqlValue sql_real(double v) { return v; }
inline SqlValue sql_text(std::string v) { return v; }
inline SqlValue sql_blob(std::vector<std::uint8_t> v) { return v; }

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<SqlValue>> rows;
};

/**
 * SQLite connection infrastructure only (design §22.1 / §22.2 vacps:store).
 * No domain tables or task logic.
 */
class Database {
 public:
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&&) noexcept;
  Database& operator=(Database&&) noexcept;
  ~Database();

  [[nodiscard]] static Result<Database> open(std::string path);

  [[nodiscard]] bool ok() const { return db_ != nullptr; }
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] sqlite3* handle() const { return db_; }

  /** Multi-statement script (no parameters). */
  [[nodiscard]] VoidResult exec(std::string_view sql);

  /** Single statement with optional bound parameters (? placeholders). */
  [[nodiscard]] VoidResult execute(std::string_view sql, const std::vector<SqlValue>& params = {});

  /** Default max rows for query(); oversized results return an error. */
  static constexpr std::size_t kDefaultMaxQueryRows = 10'000;

  /** SELECT (or any statement with a result set) + optional binds. */
  [[nodiscard]] Result<QueryResult> query(
      std::string_view sql,
      const std::vector<SqlValue>& params = {},
      std::size_t max_rows = kDefaultMaxQueryRows);

  [[nodiscard]] VoidResult begin();
  [[nodiscard]] VoidResult commit();
  [[nodiscard]] VoidResult rollback();

  [[nodiscard]] std::int64_t last_insert_rowid() const;
  [[nodiscard]] std::int64_t changes() const;

 private:
  Database(sqlite3* db, std::string path) noexcept;

  void close() noexcept;
  [[nodiscard]] VoidResult apply_pragmas();
  [[nodiscard]] Result<sqlite3_stmt*> prepare(std::string_view sql);
  [[nodiscard]] VoidResult bind_all(sqlite3_stmt* stmt, const std::vector<SqlValue>& params);
  [[nodiscard]] static SqlValue column_value(sqlite3_stmt* stmt, int col);

  sqlite3* db_{nullptr};
  std::string path_;
};

}  // namespace vacps
