#include "process/process.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;

namespace {

vacps::Result<vacps::process::RunResult> sync_run(
    std::vector<std::string> argv,
    vacps::process::RunOptions opts = {}) {
  std::optional<vacps::Result<vacps::process::RunResult>> out;
  asio::io_context ioc{1};
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        out = co_await vacps::process::async_run(std::move(argv), std::move(opts));
        co_return;
      },
      asio::detached);
  ioc.run();
  return std::move(*out);
}

}  // namespace

TEST(ProcessTest, RunTrue) {
  auto r = sync_run({"/bin/true"});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->exit_code, 0);
  EXPECT_FALSE(r->timed_out);
}

TEST(ProcessTest, RunFalse) {
  auto r = sync_run({"/bin/false"});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_NE(r->exit_code, 0);
}

TEST(ProcessTest, CaptureStdout) {
  auto r = sync_run({"/bin/echo", "vacps-hi"});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->exit_code, 0);
  EXPECT_NE(r->stdout_str.find("vacps-hi"), std::string::npos);
}

TEST(ProcessTest, EmptyArgvFails) {
  auto r = sync_run({});
  EXPECT_FALSE(r);
}

TEST(ProcessTest, EmptyCommandFails) {
  auto r = sync_run({""});
  EXPECT_FALSE(r);
}

TEST(ProcessTest, TimeoutKillsSleep) {
  vacps::process::RunOptions opts;
  opts.timeout_ms = 200;
  const auto t0 = std::chrono::steady_clock::now();
  auto r = sync_run({"/bin/sleep", "10"}, opts);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_TRUE(r->timed_out);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST(ProcessTest, TimeoutNotHitOnFastCommand) {
  vacps::process::RunOptions opts;
  opts.timeout_ms = 5000;
  auto r = sync_run({"/bin/true"}, opts);
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_FALSE(r->timed_out);
  EXPECT_EQ(r->exit_code, 0);
}

TEST(ProcessTest, CaptureStderr) {
  auto r = sync_run({"/bin/sh", "-c", "echo err-msg >&2"});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->exit_code, 0);
  EXPECT_NE(r->stderr_str.find("err-msg"), std::string::npos);
}

// Design §19.2: timeout must kill process group (shell grandchildren), not just /bin/sh.
TEST(ProcessTest, TimeoutKillsProcessGroup) {
  vacps::process::RunOptions opts;
  opts.timeout_ms = 300;
  // Marker file written by grandchild; if group kill works, sleep dies and never leaves
  // a long-running orphan (we only assert timed_out + quick return).
  const auto t0 = std::chrono::steady_clock::now();
  auto r = sync_run(
      {"/bin/sh",
       "-c",
       "sleep 30 & sleep 30 & wait"},
      opts);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_TRUE(r->timed_out);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}
