#include "storage/database.hpp"
#include "storage/store.hpp"
#include "app/log.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
namespace storage = vacps::storage;

class DbTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // Avoid noisy logs during unit tests.
    vacps::log::init("off");
  }

  void SetUp() override {
    dir_ = fs::temp_directory_path() / "vacps_db_test" /
           std::to_string(::getpid()) /
           std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(dir_);
    db_path_ = (dir_ / "t.db").string();
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  fs::path dir_;
  std::string db_path_;
};

TEST_F(DbTest, OpenPragmaAndRoundTrip) {
  auto db = vacps::storage::Database::open(db_path_);
  ASSERT_TRUE(db) << db.error().message;
  EXPECT_TRUE(db->ok());

  // Infrastructure SQL only — not domain schema.
  auto ex = db->exec(
      "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
      "INSERT INTO t(name) VALUES('alpha');");
  ASSERT_TRUE(ex) << ex.error().message;

  auto q = db->query("SELECT id, name FROM t WHERE name = ?", {vacps::storage::sql_text("alpha")});
  ASSERT_TRUE(q) << q.error().message;
  ASSERT_EQ(q->rows.size(), 1u);
  ASSERT_EQ(q->columns.size(), 2u);
  EXPECT_EQ(q->columns[0], "id");
  EXPECT_EQ(q->columns[1], "name");
  ASSERT_TRUE(std::holds_alternative<std::string>(q->rows[0][1]));
  EXPECT_EQ(std::get<std::string>(q->rows[0][1]), "alpha");
}

TEST_F(DbTest, RunBindAndMeta) {
  auto db = vacps::storage::Database::open(db_path_);
  ASSERT_TRUE(db) << db.error().message;
  ASSERT_TRUE(db->exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER);"));

  auto r = db->execute("INSERT INTO t(v) VALUES(?)", {vacps::storage::sql_int(42)});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(db->changes(), 1);
  EXPECT_GE(db->last_insert_rowid(), 1);

  auto q = db->query("SELECT v FROM t WHERE id = ?", {vacps::storage::sql_int(db->last_insert_rowid())});
  ASSERT_TRUE(q);
  ASSERT_EQ(q->rows.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<std::int64_t>(q->rows[0][0]));
  EXPECT_EQ(std::get<std::int64_t>(q->rows[0][0]), 42);
}

TEST_F(DbTest, TransactionRollback) {
  auto db = vacps::storage::Database::open(db_path_);
  ASSERT_TRUE(db);
  ASSERT_TRUE(db->exec("CREATE TABLE t(x INTEGER);"));

  // begin/commit/rollback are not public API; exercise rollback via with_transaction
  // failing mid-way after a successful INSERT.
  auto r = db->with_transaction([](vacps::storage::Database& self) -> vacps::VoidResult {
    if (auto e = self.execute("INSERT INTO t(x) VALUES(1)"); !e) {
      return e;
    }
    return std::unexpected(vacps::Error{"forced mid-transaction failure"});
  });
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().message, "forced mid-transaction failure");

  auto q = db->query("SELECT COUNT(*) AS c FROM t");
  ASSERT_TRUE(q);
  ASSERT_EQ(q->rows.size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(q->rows[0][0]), 0);
}

// ── Store domain: expectedChanges + finalizer-safe close ───────────────────

TEST_F(DbTest, StoreTransactionExpectedChangesOnRun) {
  auto opened = storage::Store::open(db_path_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto store = std::move(*opened);

  ASSERT_TRUE(store->exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER);"))
      << "create";

  storage::ExpectedChanges exactly_one;
  exactly_one.kind = storage::ExpectedChanges::Kind::Exactly;
  exactly_one.n = 1;

  std::vector<storage::TransactionStep> steps;
  {
    storage::TransactionStep s;
    s.sql = "INSERT INTO t(v) VALUES(?)";
    s.params = {storage::sql_int(7)};
    s.type = storage::StepType::Run;
    s.expected_changes = exactly_one;
    steps.push_back(std::move(s));
  }
  {
    storage::TransactionStep s;
    s.sql = "SELECT v FROM t WHERE v = ?";
    s.params = {storage::sql_int(7)};
    s.type = storage::StepType::Query;
    // no expected_changes — query steps must not check changes()
    steps.push_back(std::move(s));
  }

  auto r = store->transaction(steps);
  ASSERT_TRUE(r) << r.error().message;
  ASSERT_EQ(r->size(), 2u);
  ASSERT_TRUE(std::holds_alternative<storage::RunResult>((*r)[0]));
  EXPECT_EQ(std::get<storage::RunResult>((*r)[0]).changes, 1);
  ASSERT_TRUE(std::holds_alternative<storage::QueryResult>((*r)[1]));
  EXPECT_EQ(std::get<storage::QueryResult>((*r)[1]).rows.size(), 1u);
}

TEST_F(DbTest, StoreTransactionExpectedChangesMismatchRollsBack) {
  auto opened = storage::Store::open(db_path_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto store = std::move(*opened);

  ASSERT_TRUE(store->exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER);"));

  storage::ExpectedChanges exactly_two;
  exactly_two.kind = storage::ExpectedChanges::Kind::Exactly;
  exactly_two.n = 2;  // INSERT will change 1 row → mismatch

  std::vector<storage::TransactionStep> steps;
  {
    storage::TransactionStep s;
    s.sql = "INSERT INTO t(v) VALUES(1)";
    s.type = storage::StepType::Run;
    s.expected_changes = exactly_two;
    steps.push_back(std::move(s));
  }

  auto r = store->transaction(steps);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("expectedChanges"), std::string::npos);

  auto q = store->query("SELECT COUNT(*) AS c FROM t");
  ASSERT_TRUE(q) << q.error().message;
  ASSERT_EQ(q->rows.size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(q->rows[0][0]), 0);
}

TEST_F(DbTest, StoreTransactionQueryStepDoesNotCheckExpectedChanges) {
  // Domain skips expected_changes on Query (never compares against sqlite changes()).
  // JS binding / memory-store reject query + expectedChanges with a clear error
  // before work runs; C++ domain still ignores the field if present.
  auto opened = storage::Store::open(db_path_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto store = std::move(*opened);

  ASSERT_TRUE(store->exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER);"));
  ASSERT_TRUE(store->run("INSERT INTO t(v) VALUES(1)"));

  // Value that would always fail if incorrectly checked against changes().
  storage::ExpectedChanges exactly_999;
  exactly_999.kind = storage::ExpectedChanges::Kind::Exactly;
  exactly_999.n = 999;

  std::vector<storage::TransactionStep> steps;
  {
    storage::TransactionStep s;
    s.sql = "SELECT v FROM t";
    s.type = storage::StepType::Query;
    s.expected_changes = exactly_999;
    steps.push_back(std::move(s));
  }

  auto r = store->transaction(steps);
  ASSERT_TRUE(r) << r.error().message;
  ASSERT_EQ(r->size(), 1u);
  ASSERT_TRUE(std::holds_alternative<storage::QueryResult>((*r)[0]));
  EXPECT_EQ(std::get<storage::QueryResult>((*r)[0]).rows.size(), 1u);
}

TEST_F(DbTest, StoreQueryMaxBytesDuringMaterialize) {
  auto opened = storage::Store::open(db_path_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto store = std::move(*opened);

  ASSERT_TRUE(store->exec("CREATE TABLE t(id INTEGER PRIMARY KEY, blob TEXT);"));
  // One row with a large text cell — max_bytes should trip during materialize.
  const std::string payload(1000, 'x');
  ASSERT_TRUE(store->run(
      "INSERT INTO t(blob) VALUES(?)", {storage::sql_text(payload)}));

  auto ok = store->query("SELECT blob FROM t", {}, 100, /*max_bytes=*/10'000);
  ASSERT_TRUE(ok) << ok.error().message;

  auto too_small = store->query("SELECT blob FROM t", {}, 100, /*max_bytes=*/50);
  ASSERT_FALSE(too_small);
  EXPECT_NE(too_small.error().message.find("max_bytes"), std::string::npos);
}

TEST_F(DbTest, StoreCloseIsIdempotentWithoutRuntime) {
  // Documents that finalizer path needs no ScriptRuntime: close is mutex-safe
  // and may run on any thread (including GC finalizer) via store->close() only.
  auto opened = storage::Store::open(db_path_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto store = std::move(*opened);

  ASSERT_TRUE(store->exec("CREATE TABLE t(x INTEGER);"));
  EXPECT_FALSE(store->closed());

  ASSERT_TRUE(store->close());
  EXPECT_TRUE(store->closed());
  ASSERT_TRUE(store->close());  // idempotent
  EXPECT_TRUE(store->closed());

  auto r = store->run("INSERT INTO t(x) VALUES(1)");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("closed"), std::string::npos);
}
