#include "fs/async.hpp"
#include "fs/io_uring_probe.hpp"

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

  vacps::fs::AsyncOptions opts(bool stream = false) {
    return vacps::fs::AsyncOptions{pool_, stream};
  }

  fs::path root_;
  asio::thread_pool pool_{2};
};

TEST_F(FsAsyncTest, ProbeDoesNotCrash) {
  // Result depends on seccomp/kernel; both true and false are valid.
  (void)vacps::fs::probe_io_uring();
}

TEST_F(FsAsyncTest, WriteReadTextRoundTripPool) {
  const auto path = root_ / "a.txt";
  auto w = sync_await(
      vacps::fs::async_write_text(opts(false), path, std::string{"hello-async"}));
  ASSERT_TRUE(w) << w.error().message;

  auto r = sync_await(vacps::fs::async_read_text(opts(false), path));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(*r, "hello-async");

  auto ex = sync_await(vacps::fs::async_exists(opts(false), path));
  EXPECT_TRUE(ex);
}

TEST_F(FsAsyncTest, WriteReadViaProbeBackend) {
  // If probe ok, exercise stream_file path; else same as pool (must not hang).
  const bool stream = vacps::fs::probe_io_uring();
  const auto path = root_ / "probe.txt";
  auto w = sync_await(
      vacps::fs::async_write_text(opts(stream), path, std::string{"via-probe"}));
  ASSERT_TRUE(w) << w.error().message;
  auto r = sync_await(vacps::fs::async_read_text(opts(stream), path));
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(*r, "via-probe");
}

TEST_F(FsAsyncTest, AppendListRemove) {
  const auto dir = root_ / "sub";
  auto m = sync_await(vacps::fs::async_mkdir(opts(), dir));
  ASSERT_TRUE(m) << m.error().message;

  const auto path = dir / "b.txt";
  ASSERT_TRUE(sync_await(vacps::fs::async_write_text(opts(), path, std::string{"x"})));
  ASSERT_TRUE(sync_await(vacps::fs::async_append_text(opts(), path, std::string{"y"})));

  auto text = sync_await(vacps::fs::async_read_text(opts(), path));
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "xy");

  auto list = sync_await(vacps::fs::async_list(opts(), dir));
  ASSERT_TRUE(list) << list.error().message;
  ASSERT_EQ(list->size(), 1u);
  EXPECT_EQ((*list)[0].name, "b.txt");
  EXPECT_TRUE((*list)[0].is_file);

  ASSERT_TRUE(sync_await(vacps::fs::async_remove(opts(), path)));
  EXPECT_FALSE(sync_await(vacps::fs::async_exists(opts(), path)));
}

TEST_F(FsAsyncTest, Rename) {
  const auto from = root_ / "old.txt";
  const auto to = root_ / "new.txt";
  ASSERT_TRUE(sync_await(vacps::fs::async_write_text(opts(), from, std::string{"z"})));
  ASSERT_TRUE(sync_await(vacps::fs::async_rename(opts(), from, to)));
  EXPECT_FALSE(sync_await(vacps::fs::async_exists(opts(), from)));
  auto r = sync_await(vacps::fs::async_read_text(opts(), to));
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, "z");
}
