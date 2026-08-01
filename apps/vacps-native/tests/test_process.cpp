#include "process/process.hpp"
#include "process/runtime.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;

namespace {

vacps::Result<vacps::process::RunResult> sync_run(
    std::vector<std::string> argv,
    vacps::process::StartOptions opts = {}) {
  opts.close_stdin = true;
  std::optional<vacps::Result<vacps::process::RunResult>> out;
  asio::io_context ioc{1};
  vacps::process::ProcessRuntime runtime(ioc.get_executor());
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        auto created = runtime.create(std::move(argv), opts);
        if (!created) {
          out = std::unexpected(std::move(created.error()));
          co_return;
        }
        auto proc = std::move(*created);
        auto started = co_await proc->start();
        if (!started) {
          out = std::unexpected(std::move(started.error()));
          co_return;
        }
        auto waited = co_await proc->wait();
        (void)proc->close();
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

TEST(ProcessTest, DestructorKillsWithoutRuntimeTable) {
  asio::io_context ioc{1};
  vacps::process::ProcessRuntime runtime(ioc.get_executor());
  auto created = runtime.create({"/bin/sleep", "30"});
  ASSERT_TRUE(created);
  auto proc = std::move(*created);
  bool started_ok = false;
  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        auto s = co_await proc->start();
        started_ok = static_cast<bool>(s);
        co_return;
      },
      asio::detached);
  ioc.poll();
  ASSERT_TRUE(started_ok);
  // Destroy handle while child runs — dispose/dtor kills process group.
  proc.reset();
  ioc.poll();
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
  opts.timeout = std::chrono::milliseconds{200};
  const auto t0 = std::chrono::steady_clock::now();
  auto r = sync_run({"/bin/sleep", "10"}, opts);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_TRUE(r->timed_out);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST(ProcessTest, TimeoutNotHitOnFastCommand) {
  vacps::process::StartOptions opts;
  opts.timeout = std::chrono::milliseconds{5000};
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
  auto r = sync_run({"/bin/sh", "-c", "printf '%0100d' 0 | tr '0' 'x'"}, opts);
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->exit_code, 0);
  EXPECT_LE(r->stdout_str.size(), 16u);
  EXPECT_TRUE(r->stdout_truncated);
  EXPECT_GE(r->stdout_produced, 100u);
}

TEST(ProcessTest, ExitCodeNineIsNotTimeout) {
  auto r = sync_run({"/bin/sh", "-c", "exit 9"});
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->exit_code, 9);
  EXPECT_FALSE(r->timed_out);
}

TEST(ProcessTest, TimeoutKillsProcessGroup) {
  vacps::process::StartOptions opts;
  opts.timeout = std::chrono::milliseconds{300};
  const auto t0 = std::chrono::steady_clock::now();
  auto r = sync_run(
      {"/bin/sh", "-c", "sleep 30 & sleep 30 & wait"},
      opts);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_TRUE(r->timed_out);
  EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST(ProcessTest, AsyncWriteToCatAndClose) {
  asio::io_context ioc{1};
  vacps::process::ProcessRuntime runtime(ioc.get_executor());
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = false;
        so.timeout = std::chrono::milliseconds{5'000};
        auto created = runtime.create({"/bin/cat"}, so);
        if (!created) {
          err = created.error().message;
          co_return;
        }
        auto proc = std::move(*created);
        auto started = co_await proc->start();
        if (!started) {
          err = started.error().message;
          co_return;
        }
        vacps::process::WriteOptions wo;
        wo.close_stdin = true;
        auto wr = co_await proc->write("hello", wo);
        if (!wr) {
          err = wr.error().message;
          co_return;
        }
        std::string acc;
        for (int i = 0; i < 50; ++i) {
          auto rd = co_await proc->read(
              "stdout", std::chrono::milliseconds{2'000}, 1'000'000);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          acc += rd->data;
          if (rd->eof) break;
        }
        if (acc.find("hello") == std::string::npos) {
          err = "missing hello: " + acc;
          co_return;
        }
        (void)proc->close();
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessTest, HardMaxSetsTruncatedAndProduced) {
  asio::io_context ioc{1};
  vacps::process::ProcessRuntime runtime(ioc.get_executor());
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = true;
        so.timeout = std::chrono::milliseconds{10'000};
        so.hard_max_stdout = 64;
        so.hard_max_stderr = 64;
        const std::string payload(200, 'x');
        auto created = runtime.create(
            {"/bin/sh", "-c", "printf '%s\\n' '" + payload + "'"}, so);
        if (!created) {
          err = created.error().message;
          co_return;
        }
        auto proc = std::move(*created);
        auto started = co_await proc->start();
        if (!started) {
          err = started.error().message;
          co_return;
        }

        vacps::process::ReadResult last{};
        for (int i = 0; i < 100; ++i) {
          auto rd = co_await proc->read(
              "stdout", std::chrono::milliseconds{2'000}, 1'000'000);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          last = *rd;
          if (rd->eof) break;
        }
        if (!last.eof) {
          err = "never eof";
          (void)proc->terminate("SIGKILL", std::chrono::milliseconds{0});
          co_return;
        }
        auto waited = co_await proc->wait();
        if (!waited) {
          err = waited.error().message;
          co_return;
        }
        if (waited->stdout_str.size() > 64) {
          err = "stored more than hard_max: " +
                std::to_string(waited->stdout_str.size());
          co_return;
        }
        if (!waited->stdout_truncated) {
          err = "expected stdout_truncated";
          co_return;
        }
        if (waited->stdout_produced < 100) {
          err = "produced too small: " + std::to_string(waited->stdout_produced);
          co_return;
        }
        (void)proc->close();
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessTest, MaxRunningRejectsWithoutReclaim) {
  asio::io_context ioc{1};
  vacps::process::ProcessLimits lim;
  lim.max_running = 1;
  vacps::process::ProcessRuntime runtime(ioc.get_executor(), lim);
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = true;
        so.timeout = std::chrono::milliseconds{5'000};
        auto a = runtime.create({"/bin/sleep", "2"}, so);
        if (!a) {
          err = a.error().message;
          co_return;
        }
        auto p1 = std::move(*a);
        auto s1 = co_await p1->start();
        if (!s1) {
          err = s1.error().message;
          co_return;
        }
        auto b = runtime.create({"/bin/true"}, so);
        if (!b) {
          err = b.error().message;
          co_return;
        }
        auto p2 = std::move(*b);
        auto s2 = co_await p2->start();
        if (s2) {
          err = "expected start reject at max_running";
          (void)p1->close();
          (void)p2->close();
          co_return;
        }
        (void)p1->close();
        // After close, slot free — second start may succeed.
        auto s3 = co_await p2->start();
        if (!s3) {
          err = "retry after close: " + s3.error().message;
          co_return;
        }
        (void)co_await p2->wait();
        (void)p2->close();
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessTest, CloseFreesSlot) {
  asio::io_context ioc{1};
  vacps::process::ProcessRuntime runtime(ioc.get_executor());
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        auto created = runtime.create({"/bin/true"});
        if (!created) {
          err = created.error().message;
          co_return;
        }
        auto proc = std::move(*created);
        auto started = co_await proc->start();
        if (!started) {
          err = started.error().message;
          co_return;
        }
        for (int i = 0; i < 20; ++i) {
          auto rd = co_await proc->read(
              "stdout", std::chrono::milliseconds{500}, 1024);
          if (!rd) {
            err = rd.error().message;
            co_return;
          }
          if (rd->eof) break;
        }
        auto closed = proc->close();
        if (!closed) {
          err = closed.error().message;
          co_return;
        }
        if (!proc->closed()) {
          err = "not closed";
          co_return;
        }
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}

TEST(ProcessTest, WriteRejectsOversizedPayload) {
  asio::io_context ioc{1};
  vacps::process::ProcessRuntime runtime(ioc.get_executor());
  bool ok = false;
  std::string err;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        vacps::process::StartOptions so;
        so.close_stdin = false;
        auto created = runtime.create({"/bin/cat"}, so);
        if (!created) {
          err = created.error().message;
          co_return;
        }
        auto proc = std::move(*created);
        auto started = co_await proc->start();
        if (!started) {
          err = started.error().message;
          co_return;
        }
        vacps::process::WriteOptions wo;
        wo.max_bytes = 4;
        auto wr = co_await proc->write("too-long", wo);
        if (wr) {
          err = "expected write reject";
          co_return;
        }
        (void)proc->close();
        ok = true;
        co_return;
      },
      asio::detached);

  ioc.run();
  ASSERT_TRUE(ok) << err;
}
