#include "fs/async.hpp"
#include "fs/executor.hpp"
#include "fs/file.hpp"
#include "fs/open_options.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>

namespace asio = boost::asio;
namespace fs = std::filesystem;
using vacps::Result;
using vacps::VoidResult;
using vacps::fs::OpenMode;

namespace {

template <class Awaitable>
auto sync_await(Awaitable aw) {
  using R = typename Awaitable::value_type;
  std::optional<R> out;
  asio::io_context ioc{1};
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        out = co_await std::move(aw);
        co_return;
      },
      asio::detached);
  ioc.run();
  return std::move(*out);
}

asio::awaitable<VoidResult> async_write_text_via_file(
    vacps::fs::FsExecutor& fs,
    fs::path path,
    std::string data,
    bool append = false) {
  vacps::fs::OpenOptions o{
      .mode = append ? OpenMode::append : OpenMode::write,
  };
  auto opened = co_await vacps::fs::File::async_open(
      fs, path.string(), o, path.parent_path());
  if (!opened) co_return std::unexpected(std::move(opened.error()));
  auto n = co_await (*opened)->async_write_text(std::move(data));
  if (!n) co_return std::unexpected(std::move(n.error()));
  co_return co_await (*opened)->async_close();
}

asio::awaitable<Result<std::string>> async_read_text_via_file(
    vacps::fs::FsExecutor& fs,
    fs::path path) {
  vacps::fs::OpenOptions o{.mode = OpenMode::read};
  auto opened = co_await vacps::fs::File::async_open(
      fs, path.string(), o, path.parent_path());
  if (!opened) co_return std::unexpected(std::move(opened.error()));
  auto text = co_await (*opened)->async_read_text();
  if (!text) co_return std::unexpected(std::move(text.error()));
  auto closed = co_await (*opened)->async_close();
  if (!closed) co_return std::unexpected(std::move(closed.error()));
  co_return *text;
}

}  // namespace

class FsAsyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / "vacps_fs_async_test" /
            std::to_string(::getpid()) /
            std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(root_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  vacps::fs::AsyncOptions opts() { return vacps::fs::AsyncOptions{pool_}; }

  vacps::fs::FsExecutor fs_exec() {
    return vacps::fs::FsExecutor{
        asio::any_io_executor{},
        pool_,
        false,
        root_,
    };
  }

  fs::path root_;
  asio::thread_pool pool_{2};
};

TEST_F(FsAsyncTest, WriteReadTextRoundTripPool) {
  const auto path = root_ / "a.txt";
  auto fs = fs_exec();
  auto w = sync_await(
      async_write_text_via_file(fs, path, std::string{"hello-async"}));
  ASSERT_TRUE(w) << w.error().message;

  auto r = sync_await(async_read_text_via_file(fs, path));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(*r, "hello-async");

  auto ex = sync_await(vacps::fs::async_exists(opts(), path));
  ASSERT_TRUE(ex) << ex.error().message;
  EXPECT_TRUE(*ex);
}

TEST_F(FsAsyncTest, AppendListRemove) {
  const auto dir = root_ / "sub";
  auto m = sync_await(vacps::fs::async_mkdir(opts(), dir));
  ASSERT_TRUE(m) << m.error().message;

  const auto path = dir / "b.txt";
  auto fs = fs_exec();
  ASSERT_TRUE(sync_await(
      async_write_text_via_file(fs, path, std::string{"x"})));
  ASSERT_TRUE(sync_await(
      async_write_text_via_file(fs, path, std::string{"y"}, true)));

  auto text = sync_await(async_read_text_via_file(fs, path));
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "xy");

  auto list = sync_await(vacps::fs::async_list(opts(), dir));
  ASSERT_TRUE(list) << list.error().message;
  ASSERT_EQ(list->size(), 1u);
  EXPECT_EQ((*list)[0].name, "b.txt");
  EXPECT_TRUE((*list)[0].is_file);

  ASSERT_TRUE(sync_await(vacps::fs::async_remove(opts(), path)));
  auto gone = sync_await(vacps::fs::async_exists(opts(), path));
  ASSERT_TRUE(gone) << gone.error().message;
  EXPECT_FALSE(*gone);
}

TEST_F(FsAsyncTest, Rename) {
  const auto from = root_ / "old.txt";
  const auto to = root_ / "new.txt";
  auto fs = fs_exec();
  ASSERT_TRUE(sync_await(
      async_write_text_via_file(fs, from, std::string{"z"})));
  ASSERT_TRUE(sync_await(vacps::fs::async_rename(opts(), from, to)));
  auto gone = sync_await(vacps::fs::async_exists(opts(), from));
  ASSERT_TRUE(gone) << gone.error().message;
  EXPECT_FALSE(*gone);
  auto r = sync_await(async_read_text_via_file(fs, to));
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, "z");
}

TEST_F(FsAsyncTest, RemoveRecursiveAndRenameReplace) {
  const auto dir = root_ / "tree";
  const auto nested = dir / "a";
  ASSERT_TRUE(sync_await(vacps::fs::async_mkdir(
      opts(), nested, vacps::fs::MkdirOptions{.recursive = true})));
  const auto path = nested / "f.txt";
  auto fs = fs_exec();
  ASSERT_TRUE(sync_await(
      async_write_text_via_file(fs, path, std::string{"x"})));

  EXPECT_FALSE(sync_await(vacps::fs::async_remove(opts(), dir)));
  ASSERT_TRUE(sync_await(vacps::fs::async_remove(
      opts(), dir, vacps::fs::RemoveOptions{.recursive = true})));

  const auto a = root_ / "a.txt";
  const auto b = root_ / "b.txt";
  ASSERT_TRUE(sync_await(
      async_write_text_via_file(fs, a, std::string{"A"})));
  ASSERT_TRUE(sync_await(
      async_write_text_via_file(fs, b, std::string{"B"})));
  EXPECT_FALSE(sync_await(vacps::fs::async_rename(opts(), a, b)));
  ASSERT_TRUE(sync_await(vacps::fs::async_rename(
      opts(), a, b, vacps::fs::RenameOptions{.replace = true})));
  auto text = sync_await(async_read_text_via_file(fs, b));
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "A");
}
