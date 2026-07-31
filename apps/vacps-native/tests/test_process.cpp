#include "process/process.hpp"
#include "process/registry.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;

namespace {

/** One-shot: Process construct → start → wait → close (n1 run() path). */
vacps::Result<vacps::process::RunResult> sync_run(
    std::vector<std::string> argv,
    vacps::process::StartOptions opts = {}) {
  opts.close_stdin = true;
  std::optional<vacps::Result<vacps::process::RunResult>> out;
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.retention_ms = 0;
  vacps::process::Registry reg(ioc.get_executor(), lim);
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::Process proc(reg, std::move(argv), opts);
        auto started = co_await proc.start();
        if (!started) {
          out = std::unexpected(std::move(started.error()));
          co_return;
        }
        auto waited = co_await proc.wait();
        (void)proc.close();
        out = std::move(waited);
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
  vacps::process::StartOptions opts;
  opts.timeout_ms = 200;
  const auto t0 = std::chrono::steady_clock::now();
  auto r = sync_run({"/bin/sleep", "10"}, opts);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_TRUE(r->timed_out);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST(ProcessTest, TimeoutNotHitOnFastCommand) {
  vacps::process::StartOptions opts;
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

TEST(ProcessTest, RunStdoutHardCapTruncates) {
  vacps::process::StartOptions opts;
  opts.hard_max_stdout = 16;
  // ~100 bytes of 'x'
  auto r = sync_run({"/bin/sh", "-c", "printf '%0100d' 0 | tr '0' 'x'"}, opts);
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->exit_code, 0);
  EXPECT_LE(r->stdout_str.size(), 16u);
  EXPECT_TRUE(r->stdout_truncated);
  EXPECT_GE(r->stdout_produced, 100u);
}

TEST(ProcessTest, ExitCodeNineIsNotTimeout) {
  // Programs may legitimately exit with status 9; only timeout timer sets timed_out.
  auto r = sync_run({"/bin/sh", "-c", "exit 9"});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->exit_code, 9);
  EXPECT_FALSE(r->timed_out);
}

// Design §19.2: timeout must kill process group (shell grandchildren), not just /bin/sh.
TEST(ProcessTest, TimeoutKillsProcessGroup) {
  vacps::process::StartOptions opts;
  opts.timeout_ms = 300;
  // If group kill works, sleep children die and wait returns quickly with timed_out.
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
  // Writes "hello" to cat via async stdin, closes stdin, reads until eof.
  // Must call close() (or use retention_ms=0): default 60s TTL keeps ioc.run() alive.
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.retention_ms = 0;  // no TTL timer in unit tests
  vacps::process::Registry reg(ioc.get_executor(), lim);
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
        ro.wait_ms = 500;
        ro.max_bytes = 65'536;
        for (int i = 0; i < 50; ++i) {
          auto rd = co_await reg.read(started->id, ro);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          out += rd->stdout_slice;
          ro.stdout_offset = rd->next_stdout_offset;
          ro.stderr_offset = rd->next_stderr_offset;
          if (rd->eof) break;
        }
        if (out.find("hello-async-write") == std::string::npos) {
          err = "missing stdout: " + out;
          co_return;
        }
        (void)reg.close(started->id);
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessRegistryTest, ReadWaitsUntilPipeEofAfterExit) {
  // Burst 200 lines; must not treat process exit alone as complete (wait for eof).
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.retention_ms = 0;
  vacps::process::Registry reg(ioc.get_executor(), lim);
  bool ok = false;
  std::string err;
  std::string out;
  int line_count = 0;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = true;
        so.timeout_ms = 15'000;
        auto started = co_await reg.start(
            {"/bin/sh", "-c", "i=0; while [ $i -lt 200 ]; do echo line-$i; i=$((i+1)); done"},
            so);
        if (!started) {
          err = started.error().message;
          co_return;
        }

        vacps::process::ReadOptions ro;
        ro.wait_ms = 500;
        ro.max_bytes = 65'536;
        for (int i = 0; i < 200; ++i) {
          auto rd = co_await reg.read(started->id, ro);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          if (!rd->stdout_slice.empty()) {
            out += rd->stdout_slice;
          }
          ro.stdout_offset = rd->next_stdout_offset;
          ro.stderr_offset = rd->next_stderr_offset;
          if (rd->eof) {
            if (rd->status == "running") {
              err = "eof while status still running";
              co_return;
            }
            break;
          }
        }
        for (int i = 0; i < 200; ++i) {
          if (out.find("line-" + std::to_string(i)) != std::string::npos) ++line_count;
        }
        if (line_count < 200) {
          err = "missing lines, got " + std::to_string(line_count) + " out=" + out.substr(0, 200);
          co_return;
        }
        (void)reg.close(started->id);
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
  EXPECT_EQ(line_count, 200);
}

TEST(ProcessRegistryTest, HardMaxSetsTruncatedAndProduced) {
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.retention_ms = 0;
  vacps::process::Registry reg(ioc.get_executor(), lim);
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = true;
        so.timeout_ms = 10'000;
        so.hard_max_stdout = 64;
        so.hard_max_stderr = 64;
        // Literal 200-byte payload (portable; no brace expansion).
        const std::string payload(200, 'x');
        auto started =
            co_await reg.start({"/bin/sh", "-c", "printf '%s\\n' '" + payload + "'"}, so);
        if (!started) {
          err = started.error().message;
          co_return;
        }

        vacps::process::ReadOptions ro;
        ro.wait_ms = 2'000;
        ro.max_bytes = 1'000'000;
        vacps::process::ReadInfo last{};
        for (int i = 0; i < 100; ++i) {
          auto rd = co_await reg.read(started->id, ro);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          last = *rd;
          ro.stdout_offset = rd->next_stdout_offset;
          ro.stderr_offset = rd->next_stderr_offset;
          if (rd->eof) break;
        }
        if (!last.eof) {
          err = "never eof status=" + last.status +
                " total=" + std::to_string(last.stdout_total) +
                " produced=" + std::to_string(last.stdout_produced);
          (void)reg.terminate(started->id, "SIGKILL", 0);
          co_return;
        }
        if (last.stdout_total > 64) {
          err = "stored more than hard_max: " + std::to_string(last.stdout_total);
          co_return;
        }
        if (!last.stdout_truncated) {
          err = "expected stdout_truncated produced=" + std::to_string(last.stdout_produced) +
                " total=" + std::to_string(last.stdout_total);
          co_return;
        }
        if (last.stdout_produced < last.stdout_total) {
          err = "produced < total";
          co_return;
        }
        if (last.stdout_produced < 100) {
          err = "produced too small: " + std::to_string(last.stdout_produced);
          co_return;
        }
        (void)reg.close(started->id);
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessRegistryTest, CloseFreesEntry) {
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.retention_ms = 0;
  vacps::process::Registry reg(ioc.get_executor(), lim);
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = true;
        so.timeout_ms = 5'000;
        auto started = co_await reg.start({"/bin/true"}, so);
        if (!started) {
          err = started.error().message;
          co_return;
        }
        vacps::process::ReadOptions ro;
        ro.wait_ms = 2'000;
        for (int i = 0; i < 20; ++i) {
          auto rd = co_await reg.read(started->id, ro);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          if (rd->eof) break;
        }
        if (reg.entry_count() < 1) {
          err = "entry missing before close";
          co_return;
        }
        auto closed = reg.close(started->id);
        if (!closed || !*closed) {
          err = "close failed";
          co_return;
        }
        if (reg.entry_count() != 0) {
          err = "entry not freed";
          co_return;
        }
        auto closed2 = reg.close(started->id);
        if (!closed2 || *closed2) {
          err = "second close should be false";
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessRegistryTest, RetentionTtlAutoReclaims) {
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.retention_ms = 80;
  lim.max_entries = 32;
  vacps::process::Registry reg(ioc.get_executor(), lim);
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = true;
        so.timeout_ms = 5'000;
        auto started = co_await reg.start({"/bin/true"}, so);
        if (!started) {
          err = started.error().message;
          co_return;
        }
        vacps::process::ReadOptions ro;
        ro.wait_ms = 2'000;
        for (int i = 0; i < 20; ++i) {
          auto rd = co_await reg.read(started->id, ro);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          if (rd->eof) break;
        }
        if (reg.entry_count() != 1) {
          err = "expected 1 entry after finish";
          co_return;
        }
        // Wait past retention TTL on the same io_context.
        asio::steady_timer timer(ioc.get_executor());
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
        if (reg.entry_count() != 0) {
          err = "TTL did not reclaim entry, count=" + std::to_string(reg.entry_count());
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessRegistryTest, MaxEntriesReclaimsFinished) {
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.max_entries = 2;
  lim.retention_ms = 60'000;  // long TTL; reclaim via max_entries on start
  vacps::process::Registry reg(ioc.get_executor(), lim);
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        auto run_true = [&]() -> asio::awaitable<std::string> {
          vacps::process::StartOptions so;
          so.close_stdin = true;
          so.timeout_ms = 5'000;
          auto started = co_await reg.start({"/bin/true"}, so);
          if (!started) co_return std::string{"ERR:"} + started.error().message;
          vacps::process::ReadOptions ro;
          ro.wait_ms = 500;
          for (int i = 0; i < 40; ++i) {
            auto rd = co_await reg.read(started->id, ro);
            if (!rd) co_return std::string{"ERR:"} + rd.error().message;
            if (rd->eof) break;
          }
          co_return started->id;
        };

        auto a = co_await run_true();
        if (a.starts_with("ERR:")) {
          err = a;
          co_return;
        }
        auto b = co_await run_true();
        if (b.starts_with("ERR:")) {
          err = b;
          co_return;
        }
        if (reg.entry_count() != 2) {
          err = "expected 2 finished entries, got " + std::to_string(reg.entry_count());
          co_return;
        }
        // Third start must reclaim oldest finished (a) to free a slot.
        auto c = co_await run_true();
        if (c.starts_with("ERR:") || c.empty()) {
          err = c.empty() ? "third start failed" : c;
          co_return;
        }
        if (reg.entry_count() > 2) {
          err = "over max_entries after reclaim: " + std::to_string(reg.entry_count());
          co_return;
        }
        auto snap_a = reg.snapshot(a);
        if (snap_a) {
          err = "oldest finished entry should have been reclaimed";
          co_return;
        }
        // Drop remaining finished entries so default-style TTL cannot pin ioc.run().
        (void)reg.close(b);
        (void)reg.close(c);
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessRegistryTest, WriteRejectsOversizedPayload) {
  asio::io_context ioc{1};
  vacps::process::RegistryLimits lim;
  lim.retention_ms = 0;
  vacps::process::Registry reg(ioc.get_executor(), lim);
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
