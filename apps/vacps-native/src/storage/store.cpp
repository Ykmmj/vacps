#include "storage/store.hpp"

#include <filesystem>
#include <format>
#include <utility>

namespace vacps::storage {
namespace {

VoidResult unexpected_closed() {
  return std::unexpected(Error{"store closed"});
}

OpenMode resolve_open_mode(const OpenOptions& options) {
  return options.mode.value_or(OpenMode::ReadWriteCreate);
}

/** Ensure parent dirs exist when the open may create the DB file. */
VoidResult ensure_parent_dirs(const std::string& path, OpenMode mode) {
  if (mode != OpenMode::ReadWriteCreate) {
    return {};
  }
  namespace fs = std::filesystem;
  const fs::path p(path);
  if (!p.has_parent_path()) {
    return {};
  }
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  if (ec) {
    return std::unexpected(Error{std::format(
        "create data dir failed: {} ({})", ec.message(), p.parent_path().string())});
  }
  return {};
}

}  // namespace

Store::Store(std::unique_ptr<Database> db, std::string path)
    : db_(std::move(db)), path_(std::move(path)) {}

Store::~Store() noexcept {
  // No mutex_: the last shared_ptr drop already guarantees no in-flight legal
  // operation retains this Store. Finalizer must not block on mutex_ and must
  // not call close() (explicit close is JS run_blocking and mutex-serialized).
  closed_.store(true, std::memory_order_relaxed);
  db_.reset();
}

Result<std::shared_ptr<Store>> Store::open(std::string path, OpenOptions options) {
  const OpenMode mode = resolve_open_mode(options);
  if (auto dirs = ensure_parent_dirs(path, mode); !dirs) {
    return std::unexpected(std::move(dirs.error()));
  }
  auto db = Database::open(path, mode);
  if (!db) {
    return std::unexpected(std::move(db.error()));
  }
  auto store = std::shared_ptr<Store>(new Store(
      std::make_unique<Database>(std::move(*db)), std::move(path)));
  return store;
}

bool Store::closed() const noexcept {
  return closed_.load(std::memory_order_relaxed);
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
  // max_bytes enforced inside Database::query while materializing rows.
  return db_->query(sql, params, max_rows, max_bytes);
}

Result<std::vector<TransactionResult>> Store::transaction(
    const std::vector<TransactionStep>& steps) {
  std::lock_guard lock(mutex_);
  if (auto o = ensure_open(); !o) {
    return std::unexpected(std::move(o.error()));
  }

  // Entire unit under mutex_ so no other Store method interleaves this connection.
  // with_transaction: BEGIN IMMEDIATE … COMMIT; any step error → ROLLBACK.
  // expectedChanges is checked after EACH Run step; Query steps ignore it.
  return db_->with_transaction([&](Database& self) -> Result<std::vector<TransactionResult>> {
    std::vector<TransactionResult> out;
    out.reserve(steps.size());

    for (const auto& step : steps) {
      switch (step.type) {
        case StepType::Query: {
          // expected_changes is not meaningful for SELECT / result sets —
          // do not check against sqlite3_changes (binding rejects the combo for JS).
          auto qr = self.query(step.sql, step.params, step.max_rows, step.max_bytes);
          if (!qr) {
            return std::unexpected(std::move(qr.error()));
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
  // Publish closed before releasing the connection so closed() is true for the
  // remainder of teardown (and for concurrent observers) once we own mutex_.
  closed_.store(true, std::memory_order_relaxed);
  db_.reset();
  return {};
}

}  // namespace vacps::storage
