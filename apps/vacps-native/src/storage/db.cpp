#include "storage/db.hpp"

#include "app/log.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <format>
#include <type_traits>
#include <utility>

namespace vacps {

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
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

Result<Database> Database::open(std::string path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
      return std::unexpected(Error{std::format(
          "create data dir failed: {} ({})", ec.message(), p.parent_path().string())});
    }
  }

  sqlite3* raw = nullptr;
  const int rc = sqlite3_open_v2(
      path.c_str(),
      &raw,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  if (rc != SQLITE_OK) {
    const char* msg = raw != nullptr ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
    if (raw != nullptr) {
      sqlite3_close(raw);
    }
    return std::unexpected(
        Error{std::format("sqlite open failed: {}", msg != nullptr ? msg : "unknown")});
  }

  Database db(raw, std::move(path));
  if (auto r = db.apply_pragmas(); !r) {
    return std::unexpected(std::move(r.error()));
  }

  log::info("sqlite open {}", db.path());
  return db;
}

VoidResult Database::apply_pragmas() {
  return exec(
      "PRAGMA journal_mode = WAL;"
      "PRAGMA synchronous = NORMAL;"
      "PRAGMA foreign_keys = ON;"
      "PRAGMA busy_timeout = 5000;");
}

VoidResult Database::exec(std::string_view sql) {
  if (db_ == nullptr) {
    return std::unexpected(Error{"database not open"});
  }
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

Result<sqlite3_stmt*> Database::prepare(std::string_view sql) {
  if (db_ == nullptr) {
    return std::unexpected(Error{"database not open"});
  }
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::unexpected(Error{std::format(
        "sqlite prepare failed: {}", sqlite3_errmsg(db_))});
  }
  return stmt;
}

VoidResult Database::bind_all(sqlite3_stmt* stmt, const std::vector<SqlValue>& params) {
  for (int i = 0; i < static_cast<int>(params.size()); ++i) {
    const int idx = i + 1;
    const auto& p = params[static_cast<std::size_t>(i)];
    int rc = SQLITE_OK;
    std::visit(
        [&](const auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            rc = sqlite3_bind_null(stmt, idx);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            rc = sqlite3_bind_int64(stmt, idx, v);
          } else if constexpr (std::is_same_v<T, double>) {
            rc = sqlite3_bind_double(stmt, idx, v);
          } else if constexpr (std::is_same_v<T, std::string>) {
            rc = sqlite3_bind_text(
                stmt, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
          } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
            rc = sqlite3_bind_blob(
                stmt,
                idx,
                v.data(),
                static_cast<int>(v.size()),
                SQLITE_TRANSIENT);
          }
        },
        p);
    if (rc != SQLITE_OK) {
      return std::unexpected(Error{std::format(
          "sqlite bind failed at ?{}: {}", idx, sqlite3_errmsg(db_))});
    }
  }
  return {};
}

SqlValue Database::column_value(sqlite3_stmt* stmt, int col) {
  switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_NULL:
      return sql_null();
    case SQLITE_INTEGER:
      return sql_int(sqlite3_column_int64(stmt, col));
    case SQLITE_FLOAT:
      return sql_real(sqlite3_column_double(stmt, col));
    case SQLITE_BLOB: {
      const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, col));
      const int n = sqlite3_column_bytes(stmt, col);
      if (p == nullptr || n <= 0) {
        return sql_blob({});
      }
      return sql_blob(std::vector<std::uint8_t>(p, p + n));
    }
    case SQLITE_TEXT:
    default: {
      const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
      const int n = sqlite3_column_bytes(stmt, col);
      if (p == nullptr) {
        return sql_text({});
      }
      return sql_text(std::string(p, static_cast<std::size_t>(n)));
    }
  }
}

VoidResult Database::execute(std::string_view sql, const std::vector<SqlValue>& params) {
  auto stmt_r = prepare(sql);
  if (!stmt_r) {
    return std::unexpected(std::move(stmt_r.error()));
  }
  sqlite3_stmt* stmt = *stmt_r;
  auto cleanup = [&] { sqlite3_finalize(stmt); };

  if (auto b = bind_all(stmt, params); !b) {
    cleanup();
    return b;
  }
  const int rc = sqlite3_step(stmt);
  cleanup();
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    return std::unexpected(Error{std::format(
        "sqlite execute failed: {}", sqlite3_errmsg(db_))});
  }
  return {};
}

Result<QueryResult> Database::query(
    std::string_view sql,
    const std::vector<SqlValue>& params,
    std::size_t max_rows) {
  auto stmt_r = prepare(sql);
  if (!stmt_r) {
    return std::unexpected(std::move(stmt_r.error()));
  }
  sqlite3_stmt* stmt = *stmt_r;
  auto cleanup = [&] { sqlite3_finalize(stmt); };

  if (auto b = bind_all(stmt, params); !b) {
    cleanup();
    return std::unexpected(std::move(b.error()));
  }

  QueryResult out;
  const int ncols = sqlite3_column_count(stmt);
  out.columns.reserve(static_cast<std::size_t>(ncols));
  for (int c = 0; c < ncols; ++c) {
    const char* name = sqlite3_column_name(stmt, c);
    out.columns.emplace_back(name != nullptr ? name : "");
  }

  for (;;) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      cleanup();
      return std::unexpected(Error{std::format(
          "sqlite query failed: {}", sqlite3_errmsg(db_))});
    }
    if (out.rows.size() >= max_rows) {
      cleanup();
      return std::unexpected(Error{std::format(
          "sqlite query exceeded max_rows={}", max_rows)});
    }
    std::vector<SqlValue> row;
    row.reserve(static_cast<std::size_t>(ncols));
    for (int c = 0; c < ncols; ++c) {
      row.push_back(column_value(stmt, c));
    }
    out.rows.push_back(std::move(row));
  }

  cleanup();
  return out;
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
  return db_ != nullptr ? static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_)) : 0;
}

std::int64_t Database::changes() const {
  return db_ != nullptr ? static_cast<std::int64_t>(sqlite3_changes(db_)) : 0;
}

}  // namespace vacps
