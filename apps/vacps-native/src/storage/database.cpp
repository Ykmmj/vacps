#include "storage/database.hpp"

#include "app/log.hpp"

#include <sqlite3.h>

#include <format>
#include <type_traits>
#include <utility>

namespace vacps::storage {
namespace {

class StatementReset final {
 public:
  StatementReset(sqlite3_stmt* stmt, bool enabled) noexcept
      : stmt_(stmt), enabled_(enabled) {}
  ~StatementReset() {
    if (enabled_) {
      (void)sqlite3_reset(stmt_);
      (void)sqlite3_clear_bindings(stmt_);
    }
  }

  StatementReset(const StatementReset&) = delete;
  StatementReset& operator=(const StatementReset&) = delete;

 private:
  sqlite3_stmt* stmt_;
  bool enabled_;
};

std::size_t cell_bytes(const SqlValue& cell) {
  return std::visit(
      [](const auto& v) -> std::size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return 0;
        } else if constexpr (
            std::is_same_v<T, std::int64_t> ||
            std::is_same_v<T, double>) {
          return 8;
        } else {
          return v.size();
        }
      },
      cell);
}

}  // namespace

Statement::Statement(sqlite3* db, sqlite3_stmt* stmt) noexcept
    : db_(db), stmt_(stmt) {}

Statement::~Statement() { finalize(); }

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)),
      stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    finalize();
    db_ = std::exchange(other.db_, nullptr);
    stmt_ = std::exchange(other.stmt_, nullptr);
  }
  return *this;
}

void Statement::finalize() noexcept {
  if (stmt_ != nullptr) {
    (void)sqlite3_finalize(std::exchange(stmt_, nullptr));
  }
  db_ = nullptr;
}

VoidResult Statement::bind_all(const std::vector<SqlValue>& params) {
  for (int i = 0; i < static_cast<int>(params.size()); ++i) {
    const int idx = i + 1;
    const auto& param = params[static_cast<std::size_t>(i)];
    int rc = SQLITE_OK;
    std::visit(
        [&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            rc = sqlite3_bind_null(stmt_, idx);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            rc = sqlite3_bind_int64(stmt_, idx, value);
          } else if constexpr (std::is_same_v<T, double>) {
            rc = sqlite3_bind_double(stmt_, idx, value);
          } else if constexpr (std::is_same_v<T, std::string>) {
            rc = sqlite3_bind_text(
                stmt_,
                idx,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT);
          } else if constexpr (
              std::is_same_v<T, std::vector<std::uint8_t>>) {
            rc = sqlite3_bind_blob(
                stmt_,
                idx,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT);
          }
        },
        param);
    if (rc != SQLITE_OK) {
      return std::unexpected(Error{std::format(
          "sqlite bind failed at ?{}: {}", idx, sqlite3_errmsg(db_))});
    }
  }
  return {};
}

SqlValue Statement::column_value(sqlite3_stmt* stmt, int col) {
  switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_NULL:
      return sql_null();
    case SQLITE_INTEGER:
      return sql_int(sqlite3_column_int64(stmt, col));
    case SQLITE_FLOAT:
      return sql_real(sqlite3_column_double(stmt, col));
    case SQLITE_BLOB: {
      const auto* data = static_cast<const std::uint8_t*>(
          sqlite3_column_blob(stmt, col));
      const int size = sqlite3_column_bytes(stmt, col);
      if (data == nullptr || size <= 0) {
        return sql_blob({});
      }
      return sql_blob(std::vector<std::uint8_t>(data, data + size));
    }
    case SQLITE_TEXT:
    default: {
      const auto* data = reinterpret_cast<const char*>(
          sqlite3_column_text(stmt, col));
      const int size = sqlite3_column_bytes(stmt, col);
      if (data == nullptr) {
        return sql_text({});
      }
      return sql_text(std::string(data, static_cast<std::size_t>(size)));
    }
  }
}

VoidResult Statement::execute(const std::vector<SqlValue>& params) {
  return execute_impl(params, true);
}

VoidResult Statement::execute_impl(
    const std::vector<SqlValue>& params,
    bool reset_after) {
  StatementReset reset{stmt_, reset_after};
  if (auto bound = bind_all(params); !bound) {
    return bound;
  }
  const int rc = sqlite3_step(stmt_);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    return std::unexpected(Error{std::format(
        "sqlite execute failed: {}", sqlite3_errmsg(db_))});
  }
  return {};
}

Result<QueryResult> Statement::query(
    const std::vector<SqlValue>& params,
    std::size_t max_rows,
    std::optional<std::size_t> max_bytes) {
  return query_impl(params, max_rows, max_bytes, true);
}

Result<QueryResult> Statement::query_impl(
    const std::vector<SqlValue>& params,
    std::size_t max_rows,
    std::optional<std::size_t> max_bytes,
    bool reset_after) {
  StatementReset reset{stmt_, reset_after};
  if (auto bound = bind_all(params); !bound) {
    return std::unexpected(std::move(bound.error()));
  }

  QueryResult out;
  const int column_count = sqlite3_column_count(stmt_);
  out.columns.reserve(static_cast<std::size_t>(column_count));
  std::size_t approximate_bytes = 0;
  for (int column = 0; column < column_count; ++column) {
    const char* name = sqlite3_column_name(stmt_, column);
    out.columns.emplace_back(name != nullptr ? name : "");
    approximate_bytes += out.columns.back().size();
  }
  if (max_bytes && approximate_bytes > *max_bytes) {
    return std::unexpected(Error{std::format(
        "sqlite query exceeded max_bytes={} (approx {})",
        *max_bytes,
        approximate_bytes)});
  }

  for (;;) {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      return std::unexpected(Error{std::format(
          "sqlite query failed: {}", sqlite3_errmsg(db_))});
    }
    if (out.rows.size() >= max_rows) {
      return std::unexpected(Error{std::format(
          "sqlite query exceeded max_rows={}", max_rows)});
    }

    std::vector<SqlValue> row;
    row.reserve(static_cast<std::size_t>(column_count));
    for (int column = 0; column < column_count; ++column) {
      SqlValue cell = column_value(stmt_, column);
      approximate_bytes += cell_bytes(cell);
      row.push_back(std::move(cell));
    }
    if (max_bytes && approximate_bytes > *max_bytes) {
      return std::unexpected(Error{std::format(
          "sqlite query exceeded max_bytes={} (approx {})",
          *max_bytes,
          approximate_bytes)});
    }
    out.rows.push_back(std::move(row));
  }

  return out;
}

Database::Database(sqlite3* db, std::string path) noexcept
    : db_(db), path_(std::move(path)) {}

Database::~Database() { close(); }

Database::Database(Database&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)) {}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    close();
    db_ = std::exchange(other.db_, nullptr);
    path_ = std::move(other.path_);
  }
  return *this;
}

void Database::close() noexcept {
  if (db_ == nullptr) {
    return;
  }
  // Null the member exactly once before close_v2 so a re-entrant or second
  // close never touches a successfully closed handle.
  sqlite3* handle = std::exchange(db_, nullptr);
  const int rc = sqlite3_close_v2(handle);
  if (rc != SQLITE_OK) {
    log::warn("sqlite close_v2: {}", sqlite3_errstr(rc));
  }
}

Result<Database> Database::open(std::string path, OpenMode mode) {
  // Parent directories are the caller's responsibility (Store::open creates them
  // for ReadWriteCreate). Keeping this layer free of mkdir side effects.

  int flags = SQLITE_OPEN_NOMUTEX;
  switch (mode) {
    case OpenMode::ReadOnly:
      flags |= SQLITE_OPEN_READONLY;
      break;
    case OpenMode::ReadWrite:
      flags |= SQLITE_OPEN_READWRITE;
      break;
    case OpenMode::ReadWriteCreate:
      flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
      break;
  }

  sqlite3* raw = nullptr;
  const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
  if (rc != SQLITE_OK) {
    const char* msg = raw != nullptr ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
    const std::string err =
        std::format("sqlite open failed: {}", msg != nullptr ? msg : "unknown");
    if (raw != nullptr) {
      const int close_rc = sqlite3_close_v2(raw);
      raw = nullptr;
      if (close_rc != SQLITE_OK) {
        log::warn("sqlite close_v2 after open failure: {}", sqlite3_errstr(close_rc));
      }
    }
    return std::unexpected(Error{err});
  }

  Database db(raw, std::move(path));
  if (auto r = db.apply_pragmas(mode); !r) {
    return std::unexpected(std::move(r.error()));
  }

  log::info("sqlite open {} mode={}", db.path(), static_cast<int>(mode));
  return db;
}

VoidResult Database::apply_pragmas(OpenMode mode) {
  // Read-only: skip journal_mode / synchronous (may require write access).
  if (mode == OpenMode::ReadOnly) {
    return exec(
        "PRAGMA foreign_keys = ON;"
        "PRAGMA busy_timeout = 5000;");
  }
  return exec(
      "PRAGMA journal_mode = WAL;"
      "PRAGMA synchronous = NORMAL;"
      "PRAGMA foreign_keys = ON;"
      "PRAGMA busy_timeout = 5000;");
}

VoidResult Database::exec(std::string_view sql) {
  if (sql.empty()) {
    return {};
  }
  char* errmsg = nullptr;
  const std::string sql_owned{sql};
  const int rc = sqlite3_exec(db_, sql_owned.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    Error e{errmsg != nullptr ? errmsg : sqlite3_errstr(rc)};
    if (errmsg != nullptr) {
      sqlite3_free(errmsg);
    }
    return std::unexpected(std::move(e));
  }
  return {};
}

Result<Statement> Database::prepare(std::string_view sql) {
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::unexpected(Error{std::format(
        "sqlite prepare failed: {}", sqlite3_errmsg(db_))});
  }
  if (stmt == nullptr) {
    return std::unexpected(Error{"sqlite prepare produced no statement"});
  }
  return Statement{db_, stmt};
}

VoidResult Database::execute(std::string_view sql, const std::vector<SqlValue>& params) {
  auto statement = prepare(sql);
  if (!statement) {
    return std::unexpected(std::move(statement.error()));
  }
  return statement->execute_impl(params, false);
}

Result<QueryResult> Database::query(
    std::string_view sql,
    const std::vector<SqlValue>& params,
    std::size_t max_rows,
    std::optional<std::size_t> max_bytes) {
  auto statement = prepare(sql);
  if (!statement) {
    return std::unexpected(std::move(statement.error()));
  }
  return statement->query_impl(params, max_rows, max_bytes, false);
}

VoidResult Database::begin() { return exec("BEGIN IMMEDIATE;"); }
VoidResult Database::commit() { return exec("COMMIT;"); }
VoidResult Database::rollback() { return exec("ROLLBACK;"); }

Result<std::vector<Database::TxStepResult>> Database::run_transaction(
    const std::vector<TxStep>& steps) {
  return with_transaction([&](Database& self) -> Result<std::vector<TxStepResult>> {
    std::vector<TxStepResult> out;
    out.reserve(steps.size());
    for (const auto& step : steps) {
      if (step.is_run) {
        if (auto r = self.execute(step.sql, step.params); !r) {
          return std::unexpected(std::move(r.error()));
        }
        out.push_back(TxStepResult{self.changes(), self.last_insert_rowid()});
      } else {
        if (auto r = self.exec(step.sql); !r) {
          return std::unexpected(std::move(r.error()));
        }
        out.push_back(TxStepResult{self.changes(), self.last_insert_rowid()});
      }
    }
    return out;
  });
}

std::int64_t Database::last_insert_rowid() const {
  return static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
}

std::int64_t Database::changes() const {
  return static_cast<std::int64_t>(sqlite3_changes(db_));
}

}  // namespace vacps::storage
