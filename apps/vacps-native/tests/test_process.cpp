#include "process/process.hpp"
#include "process/registry.hpp"

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

TEST(ProcessRegistryTest, AsyncWriteToCatAndClose) {
  asio::io_context ioc{1};
  vacps::process::Registry reg(ioc.get_executor());
  bool ok = false;
  std::string err;
  std::string out;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = false;
        so.timeout_ms = 10'000;
        auto started = co_await reg.start({"/bin/cat"}, so);
        if (!started) {
          err = started.error().message;
          co_return;
        }
        vacps::process::WriteOptions wo;
        wo.close_stdin = false;
        wo.timeout_ms = 5'000;
        auto w1 = co_await reg.write(started->id, "hello-async-write\n", wo);
        if (!w1) {
          err = w1.error().message;
          co_return;
        }
        EXPECT_EQ(*w1, std::string("hello-async-write\n").size());

        wo.close_stdin = true;
        auto w2 = co_await reg.write(started->id, "", wo);
        if (!w2) {
          err = w2.error().message;
          co_return;
        }

        vacps::process::ReadOptions ro;
        ro.wait_ms = 3'000;
        ro.max_bytes = 65'536;
        for (int i = 0; i < 50; ++i) {
          auto rd = co_await reg.read(started->id, ro);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          out += rd->stdout_slice;
          if (rd->eof || rd->status != "running") break;
          ro.stdout_offset = rd->next_stdout_offset;
          ro.stderr_offset = rd->next_stderr_offset;
        }
        if (out.find("hello-async-write") == std::string::npos) {
          err = "missing stdout: " + out;
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessRegistryTest, WriteRejectsOversizedPayload) {
  asio::io_context ioc{1};
  vacps::process::Registry reg(ioc.get_executor());
  bool rejected = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = false;
        so.timeout_ms = 5'000;
        auto started = co_await reg.start({"/bin/cat"}, so);
        if (!started) {
          err = started.error().message;
          co_return;
        }
        vacps::process::WriteOptions wo;
        wo.max_bytes = 16;
        wo.timeout_ms = 2'000;
        auto w = co_await reg.write(started->id, std::string(64, 'x'), wo);
        if (w) {
          err = "expected reject for oversized write";
          co_return;
        }
        err = w.error().message;
        rejected = err.find("max_bytes") != std::string::npos;
        (void)reg.terminate(started->id, "SIGKILL", 0);
        co_return;
      },
      asio::detached);

  ioc.run();
  EXPECT_TRUE(rejected) << err;
}
