#include "storage/database.hpp"
#include "app/log.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <variant>

namespace fs = std::filesystem;

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
