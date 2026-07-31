#include "storage/store.hpp"

#include <format>
#include <type_traits>
#include <utility>

namespace vacps::storage {
namespace {

VoidResult unexpected_closed() {
  return std::unexpected(Error{"store closed"});
}

OpenMode resolve_open_mode(const OpenOptions& options) {
  return options.mode.value_or(OpenMode::ReadWriteCreate);
}

}  // namespace

Store::Store(std::unique_ptr<Database> db, std::string path, OpenOptions options)
    : db_(std::move(db)), path_(std::move(path)), options_(std::move(options)) {}

Store::~Store() {
  std::lock_guard lock(mutex_);
  db_.reset();
}

Result<std::shared_ptr<Store>> Store::open(std::string path, OpenOptions options) {
  const OpenMode mode = resolve_open_mode(options);
  auto db = Database::open(path, mode);
  if (!db) {
    return std::unexpected(std::move(db.error()));
  }
  auto store = std::shared_ptr<Store>(new Store(
      std::make_unique<Database>(std::move(*db)), std::move(path), std::move(options)));
  return store;
}

bool Store::closed() const noexcept {
  std::lock_guard lock(mutex_);
  return db_ == nullptr || !db_->ok();
}

VoidResult Store::ensure_open() const {
  if (db_ == nullptr || !db_->ok()) {
    return unexpected_closed();
  }
  return {};
}

VoidResult Store::check_expected_changes(const ExpectedChanges& exp, std::int64_t changes) {
  switch (exp.kind) {
    case ExpectedChanges::Kind::Exactly:
      if (changes != exp.n) {
        return std::unexpected(Error{std::format(
            "store.transaction: expectedChanges exactly {} but got {}", exp.n, changes)});
      }
      break;
    case ExpectedChanges::Kind::AtLeast:
      if (changes < exp.n) {
        return std::unexpected(Error{std::format(
            "store.transaction: expectedChanges atLeast {} but got {}", exp.n, changes)});
      }
      break;
    case ExpectedChanges::Kind::AtMost:
      if (changes > exp.n) {
        return std::unexpected(Error{std::format(
            "store.transaction: expectedChanges atMost {} but got {}", exp.n, changes)});
      }
      break;
  }
  return {};
}

std::size_t Store::estimate_query_bytes(const QueryResult& qr) {
  std::size_t n = 0;
  for (const auto& col : qr.columns) {
    n += col.size();
  }
  for (const auto& row : qr.rows) {
    for (const auto& cell : row) {
      std::visit(
          [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
              // null
            } else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
              n += 8;
            } else if constexpr (std::is_same_v<T, std::string>) {
              n += v.size();
            } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
              n += v.size();
            }
          },
          cell);
    }
  }
  return n;
}

VoidResult Store::check_max_bytes(
    const QueryResult& qr,
    std::optional<std::size_t> max_bytes) {
  if (!max_bytes) {
    return {};
  }
  const auto used = estimate_query_bytes(qr);
  if (used > *max_bytes) {
    return std::unexpected(Error{std::format(
        "sqlite query exceeded max_bytes={} (approx {})", *max_bytes, used)});
  }
  return {};
}

VoidResult Store::exec(std::string_view sql) {
  std::lock_guard lock(mutex_);
  if (auto o = ensure_open(); !o) {
    return o;
  }
  return db_->exec(sql);
}

Result<RunResult> Store::run(std::string_view sql, const std::vector<SqlValue>& params) {
  std::lock_guard lock(mutex_);
  if (auto o = ensure_open(); !o) {
    return std::unexpected(std::move(o.error()));
  }
  if (auto ex = db_->execute(sql, params); !ex) {
    return std::unexpected(std::move(ex.error()));
  }
  return RunResult{db_->changes(), db_->last_insert_rowid()};
}

Result<QueryResult> Store::query(
    std::string_view sql,
    const std::vector<SqlValue>& params,
    std::size_t max_rows,
    std::optional<std::size_t> max_bytes) {
  std::lock_guard lock(mutex_);
  if (auto o = ensure_open(); !o) {
    return std::unexpected(std::move(o.error()));
  }
  auto qr = db_->query(sql, params, max_rows);
  if (!qr) {
    return std::unexpected(std::move(qr.error()));
  }
  if (auto b = check_max_bytes(*qr, max_bytes); !b) {
    return std::unexpected(std::move(b.error()));
  }
  return qr;
}

Result<std::vector<TransactionResult>> Store::transaction(
    const std::vector<TransactionStep>& steps) {
  std::lock_guard lock(mutex_);
  if (auto o = ensure_open(); !o) {
    return std::unexpected(std::move(o.error()));
  }

  // Entire unit under mutex_ so no other Store method interleaves this connection.
  // with_transaction: BEGIN IMMEDIATE … COMMIT; any step error → ROLLBACK.
  // expectedChanges is checked after EACH step; mismatch aborts before later steps.
  return db_->with_transaction([&](Database& self) -> Result<std::vector<TransactionResult>> {
    std::vector<TransactionResult> out;
    out.reserve(steps.size());

    for (const auto& step : steps) {
      switch (step.type) {
        case StepType::Query: {
          auto qr = self.query(step.sql, step.params, step.max_rows);
          if (!qr) {
            return std::unexpected(std::move(qr.error()));
          }
          if (step.expected_changes) {
            if (auto c = check_expected_changes(*step.expected_changes, self.changes()); !c) {
              return std::unexpected(std::move(c.error()));
            }
          }
          out.emplace_back(std::move(*qr));
          break;
        }
        case StepType::Run: {
          if (auto r = self.execute(step.sql, step.params); !r) {
            return std::unexpected(std::move(r.error()));
          }
          RunResult rr{self.changes(), self.last_insert_rowid()};
          if (step.expected_changes) {
            if (auto c = check_expected_changes(*step.expected_changes, rr.changes); !c) {
              return std::unexpected(std::move(c.error()));
            }
          }
          out.emplace_back(rr);
          break;
        }
      }
    }
    return out;
  });
}

VoidResult Store::close() {
  std::lock_guard lock(mutex_);
  db_.reset();
  return {};
}

}  // namespace vacps::storage
