#include "fs/executor.hpp"
#include "fs/file.hpp"
#include "fs/open_options.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace fs = std::filesystem;
using vacps::fs::OpenMode;

class FileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / "vacps_file_test" /
            std::to_string(::getpid()) /
            std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(root_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  fs::path root_;
};

TEST_F(FileTest, OpenWriteWriteTextCloseOpenRead) {
  const auto path = (root_ / "hello.txt").string();
  {
    vacps::fs::OpenOptions opts{.mode = OpenMode::write};
    // Force pool path (no Asio executor / probe).
    auto f = vacps::fs::File::open(path, opts, root_);
    ASSERT_TRUE(f) << f.error().message;
    EXPECT_FALSE((*f)->uses_asio_file());
    EXPECT_EQ((*f)->open_mode(), OpenMode::write);
    auto n = (*f)->write_text("hello-file");
    ASSERT_TRUE(n) << n.error().message;
    EXPECT_EQ(*n, 10u);
    ASSERT_TRUE((*f)->close());
    EXPECT_TRUE((*f)->closed());
  }
  {
    vacps::fs::OpenOptions opts{.mode = OpenMode::read};
    auto f = vacps::fs::File::open(path, opts, root_);
    ASSERT_TRUE(f) << f.error().message;
    EXPECT_EQ((*f)->open_mode(), OpenMode::read);
    auto text = (*f)->read_text();
    ASSERT_TRUE(text) << text.error().message;
    EXPECT_EQ(*text, "hello-file");
  }
}

TEST_F(FileTest, WriteNewFailsIfExists) {
  const auto path = (root_ / "exclusive.txt").string();
  vacps::fs::OpenOptions create{.mode = OpenMode::write_new};
  auto first = vacps::fs::File::open(path, create, root_);
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE((*first)->write_text("one"));
  ASSERT_TRUE((*first)->close());

  auto second = vacps::fs::File::open(path, create, root_);
  ASSERT_FALSE(second);
}

TEST_F(FileTest, ReadFailsIfMissing) {
  const auto path = (root_ / "missing.txt").string();
  vacps::fs::OpenOptions opts{.mode = OpenMode::read};
  auto f = vacps::fs::File::open(path, opts, root_);
  ASSERT_FALSE(f);
}

TEST_F(FileTest, OpenWithFsExecutor) {
  // FsExecutor convenience open uses pool + optional data_dir base.
  asio::thread_pool pool{2};
  vacps::fs::FsExecutor fs{
      asio::any_io_executor{},
      pool,
      false,
      root_,
  };
  vacps::fs::OpenOptions opts{.mode = OpenMode::write};
  auto f = vacps::fs::File::open(fs, "via-executor.txt", opts);
  ASSERT_TRUE(f) << f.error().message;
  EXPECT_FALSE((*f)->uses_asio_file());
  ASSERT_TRUE((*f)->write_text("executor"));
  ASSERT_TRUE((*f)->close());

  vacps::fs::OpenOptions ro{.mode = OpenMode::read};
  auto r = vacps::fs::File::open(fs, "via-executor.txt", ro);
  ASSERT_TRUE(r) << r.error().message;
  auto text = (*r)->read_text();
  ASSERT_TRUE(text) << text.error().message;
  EXPECT_EQ(*text, "executor");
  ASSERT_TRUE((*r)->close());
  pool.join();
}

TEST_F(FileTest, ReadAtWriteAtDoNotMoveCursor) {
  const auto path = (root_ / "cursor.bin").string();
  // Need read-write: sequential read after write uses the same handle.
  // write first to create, then reopen read-write — or use write then reopen.
  // OpenMode::write is write-only; create via write then reopen read-write
  // after re-writing with truncate via a two-step open is awkward. Use
  // append_read which is O_RDWR|O_CREAT|O_APPEND then truncate.
  // Simpler: open write, write seed, close; open read-write for cursor test.
  {
    vacps::fs::OpenOptions wopts{.mode = OpenMode::write};
    auto wf = vacps::fs::File::open(path, wopts, root_);
    ASSERT_TRUE(wf) << wf.error().message;
    ASSERT_TRUE((*wf)->write_text("ABCDEFGH"));
    ASSERT_TRUE((*wf)->close());
  }
  // Re-open read-write without truncate — but read_write does not create.
  // Content already exists.
  vacps::fs::OpenOptions wopts{.mode = OpenMode::read_write};
  auto f = vacps::fs::File::open(path, wopts, root_);
  ASSERT_TRUE(f) << f.error().message;
  // Cursor starts at 0; write_at should not move it for sequential reads later.
  // First advance sequential cursor by reading nothing useful — write at start
  // via write_at without moving sequential cursor.
  const std::uint8_t patch[] = {'X', 'Y'};
  auto n = (*f)->write_at(0, std::span<const std::uint8_t>(patch, 2));
  ASSERT_TRUE(n) << n.error().message;
  EXPECT_EQ(*n, 2u);
  // Sequential cursor still at 0 → read sees patched content
  auto head_seq = (*f)->read(8);
  ASSERT_TRUE(head_seq) << head_seq.error().message;
  ASSERT_EQ(head_seq->size(), 8u);
  EXPECT_EQ((*head_seq)[0], 'X');
  EXPECT_EQ((*head_seq)[1], 'Y');
  // After sequential read, cursor at EOF; another sequential read → empty
  auto empty = (*f)->read(16);
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());
  // pread still sees patched content
  auto head = (*f)->read_at(0, 8);
  ASSERT_TRUE(head) << head.error().message;
  ASSERT_EQ(head->size(), 8u);
  EXPECT_EQ((*head)[0], 'X');
  EXPECT_EQ((*head)[1], 'Y');
  EXPECT_EQ((*head)[2], 'C');
  ASSERT_TRUE((*f)->close());
}

TEST_F(FileTest, CloseThenReadFails) {
  const auto path = (root_ / "closed.txt").string();
  vacps::fs::OpenOptions opts{.mode = OpenMode::write};
  auto f = vacps::fs::File::open(path, opts, root_);
  ASSERT_TRUE(f);
  ASSERT_TRUE((*f)->write_text("z"));
  ASSERT_TRUE((*f)->close());
  auto r = (*f)->read(4);
  ASSERT_FALSE(r);
}

TEST_F(FileTest, DoubleCloseIdempotent) {
  // Expected: second (and further) close() succeeds; closed() stays true.
  // No crash, no double-free of the FD.
  const auto path = (root_ / "double-close.txt").string();
  vacps::fs::OpenOptions opts{.mode = OpenMode::write};
  auto f = vacps::fs::File::open(path, opts, root_);
  ASSERT_TRUE(f) << f.error().message;
  ASSERT_TRUE((*f)->write_text("x"));
  ASSERT_TRUE((*f)->close());
  EXPECT_TRUE((*f)->closed());
  ASSERT_TRUE((*f)->close());
  EXPECT_TRUE((*f)->closed());
  ASSERT_TRUE((*f)->close());
}

TEST_F(FileTest, ReadMaxBytesHardReject) {
  // Expected: maxBytes above 64 MiB hard limit is rejected before I/O.
  const auto path = (root_ / "big-limit.txt").string();
  // Create file then reopen read-write for read after write on same handle:
  // use write then reopen with read_write after seeding via write.
  {
    vacps::fs::OpenOptions w{.mode = OpenMode::write};
    auto wf = vacps::fs::File::open(path, w, root_);
    ASSERT_TRUE(wf) << wf.error().message;
    ASSERT_TRUE((*wf)->write_text("hi"));
    ASSERT_TRUE((*wf)->close());
  }
  vacps::fs::OpenOptions opts{.mode = OpenMode::read};
  auto f = vacps::fs::File::open(path, opts, root_);
  ASSERT_TRUE(f) << f.error().message;
  const std::size_t over =
      vacps::fs::File::kHardMaxReadBytes + 1;
  auto r = (*f)->read(over);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().message.find("hard limit"), std::string::npos);
}

namespace {

/**
 * Drive an awaitable to completion on a private io_context.
 * Used by concurrency tests that exercise File::async_* + strand.
 */
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

/** Open via FsExecutor (pool backend) for concurrency tests. */
vacps::Result<std::shared_ptr<vacps::fs::File>> open_pool(
    asio::thread_pool& pool,
    std::string_view path,
    vacps::fs::OpenOptions opts,
    const fs::path& root) {
  vacps::fs::FsExecutor fs{asio::any_io_executor{}, pool, false, root};
  return vacps::fs::File::open(fs, path, opts, root);
}

}  // namespace

/**
 * Concurrent async read + write on the same File (pool backend + strand).
 *
 * Expected behavior:
 * - Must not crash / UAF / data race on offset_, FD, or life state.
 * - Ops are serialized on the per-File strand (order is defined, not parallel
 *   on the FD); many co_spawned callers may race at post time.
 * - Both kinds of ops eventually complete with success or a coherent error.
 *
 * Note: Asio awaitables are lazy until co_awaited, so true overlap is obtained
 * by co_spawning independent coroutines that each co_await one File op.
 */
TEST_F(FileTest, ConcurrentAsyncReadWriteSerialized) {
  const auto path = (root_ / "concurrent-rw.bin").string();
  asio::thread_pool pool{4};

  // Seed with write, then open append_read (RDWR|CREAT|APPEND) for concurrent
  // read_at/write_at — write_at/read_at ignore append for positioned I/O.
  {
    vacps::fs::OpenOptions seed{.mode = OpenMode::write};
    auto s = vacps::fs::File::open(path, seed, root_);
    ASSERT_TRUE(s) << s.error().message;
    std::vector<std::uint8_t> seed_buf(4096, static_cast<std::uint8_t>('A'));
    ASSERT_TRUE((*s)->write(
        std::span<const std::uint8_t>(seed_buf.data(), seed_buf.size())));
    ASSERT_TRUE((*s)->close());
  }

  vacps::fs::OpenOptions opts{.mode = OpenMode::read_write};
  auto opened = open_pool(pool, path, opts, root_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto file = *opened;
  ASSERT_FALSE(file->uses_asio_file());

  std::atomic<int> remaining{0};
  std::atomic<int> ok_read{0};
  std::atomic<int> ok_write{0};

  asio::io_context ioc{1};
  constexpr int kPairs = 32;
  remaining.store(kPairs * 2, std::memory_order_relaxed);

  for (int i = 0; i < kPairs; ++i) {
    asio::co_spawn(
        ioc,
        [file, &ok_read, &remaining]() -> asio::awaitable<void> {
          auto r = co_await file->async_read_at(0, 256);
          if (r) {
            ok_read.fetch_add(1, std::memory_order_relaxed);
          }
          remaining.fetch_sub(1, std::memory_order_acq_rel);
          co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [file, i, &ok_write, &remaining]() -> asio::awaitable<void> {
          std::vector<std::uint8_t> chunk(
              128, static_cast<std::uint8_t>('B' + (i % 26)));
          auto w = co_await file->async_write_at(
              0, std::span<const std::uint8_t>(chunk.data(), chunk.size()));
          if (w) {
            ok_write.fetch_add(1, std::memory_order_relaxed);
          }
          remaining.fetch_sub(1, std::memory_order_acq_rel);
          co_return;
        },
        asio::detached);
  }

  ioc.run();
  EXPECT_EQ(remaining.load(), 0);
  // All ops should complete successfully while the file is open; strand must
  // not deadlock under concurrent post.
  EXPECT_EQ(ok_read.load(), kPairs);
  EXPECT_EQ(ok_write.load(), kPairs);

  auto closed = sync_await(file->async_close());
  ASSERT_TRUE(closed) << closed.error().message;
  EXPECT_TRUE(file->closed());
  pool.join();
}

/**
 * Concurrent async_read with async_close on the same File.
 *
 * Expected behavior:
 * - Must not crash.
 * - Read either succeeds (ran before close on the strand) or fails with
 *   "file is closed" (ran after close entered Closing/Closed).
 * - close is successful; file.closed() is true afterwards.
 */
TEST_F(FileTest, ConcurrentAsyncReadWithClose) {
  const auto path = (root_ / "concurrent-read-close.bin").string();
  asio::thread_pool pool{4};

  {
    vacps::fs::OpenOptions seed{.mode = OpenMode::write};
    auto s = vacps::fs::File::open(path, seed, root_);
    ASSERT_TRUE(s) << s.error().message;
    std::vector<std::uint8_t> seed_buf(8192, static_cast<std::uint8_t>('R'));
    ASSERT_TRUE((*s)->write(
        std::span<const std::uint8_t>(seed_buf.data(), seed_buf.size())));
    ASSERT_TRUE((*s)->close());
  }

  vacps::fs::OpenOptions opts{.mode = OpenMode::read};
  auto opened = open_pool(pool, path, opts, root_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto file = *opened;

  std::optional<vacps::Result<std::vector<std::uint8_t>>> read_result;
  std::optional<vacps::VoidResult> close_result;
  std::atomic<int> pending{2};

  asio::io_context ioc{1};
  // Separate co_spawns so both ops are posted before either fully completes.
  asio::co_spawn(
      ioc,
      [file, &read_result, &pending]() -> asio::awaitable<void> {
        read_result = co_await file->async_read_at(0, 1024);
        pending.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
      },
      asio::detached);
  asio::co_spawn(
      ioc,
      [file, &close_result, &pending]() -> asio::awaitable<void> {
        close_result = co_await file->async_close();
        pending.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
      },
      asio::detached);
  ioc.run();

  EXPECT_EQ(pending.load(), 0);
  ASSERT_TRUE(read_result.has_value());
  ASSERT_TRUE(close_result.has_value());
  ASSERT_TRUE(*close_result) << close_result->error().message;
  EXPECT_TRUE(file->closed());
  // Read may win or lose the race; either is valid under strand ordering.
  if (!*read_result) {
    EXPECT_NE(read_result->error().message.find("closed"), std::string::npos);
  }
  // Second close is idempotent.
  auto again = sync_await(file->async_close());
  ASSERT_TRUE(again) << again.error().message;
  pool.join();
}

/**
 * Concurrent async_write with async_close on the same File.
 *
 * Expected behavior:
 * - Must not crash / double-close FD.
 * - Write either succeeds or fails with closed; close always succeeds.
 * - Double close after remains idempotent.
 */
TEST_F(FileTest, ConcurrentAsyncWriteWithClose) {
  const auto path = (root_ / "concurrent-write-close.bin").string();
  asio::thread_pool pool{4};

  vacps::fs::OpenOptions opts{.mode = OpenMode::write};
  auto opened = open_pool(pool, path, opts, root_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto file = *opened;

  std::optional<vacps::Result<std::size_t>> write_result;
  std::optional<vacps::VoidResult> close_result;
  std::vector<std::uint8_t> payload(2048, static_cast<std::uint8_t>('W'));
  std::atomic<int> pending{2};

  asio::io_context ioc{1};
  asio::co_spawn(
      ioc,
      [file, &payload, &write_result, &pending]() -> asio::awaitable<void> {
        write_result = co_await file->async_write_at(
            0, std::span<const std::uint8_t>(payload.data(), payload.size()));
        pending.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
      },
      asio::detached);
  asio::co_spawn(
      ioc,
      [file, &close_result, &pending]() -> asio::awaitable<void> {
        close_result = co_await file->async_close();
        pending.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
      },
      asio::detached);
  ioc.run();

  EXPECT_EQ(pending.load(), 0);
  ASSERT_TRUE(write_result.has_value());
  ASSERT_TRUE(close_result.has_value());
  ASSERT_TRUE(*close_result) << close_result->error().message;
  EXPECT_TRUE(file->closed());
  if (!*write_result) {
    EXPECT_NE(write_result->error().message.find("closed"), std::string::npos);
  }
  ASSERT_TRUE(file->close());  // sync double-close still idempotent
  pool.join();
}

/**
 * Multiple concurrent async_close calls.
 *
 * Expected: all succeed; closed() true; no crash.
 */
TEST_F(FileTest, ConcurrentAsyncDoubleCloseIdempotent) {
  const auto path = (root_ / "concurrent-double-close.txt").string();
  asio::thread_pool pool{2};

  vacps::fs::OpenOptions opts{.mode = OpenMode::write};
  auto opened = open_pool(pool, path, opts, root_);
  ASSERT_TRUE(opened) << opened.error().message;
  auto file = *opened;
  ASSERT_TRUE(file->write_text("z"));

  std::optional<vacps::VoidResult> c1;
  std::optional<vacps::VoidResult> c2;
  std::atomic<int> pending{2};

  asio::io_context ioc{1};
  asio::co_spawn(
      ioc,
      [file, &c1, &pending]() -> asio::awaitable<void> {
        c1 = co_await file->async_close();
        pending.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
      },
      asio::detached);
  asio::co_spawn(
      ioc,
      [file, &c2, &pending]() -> asio::awaitable<void> {
        c2 = co_await file->async_close();
        pending.fetch_sub(1, std::memory_order_acq_rel);
        co_return;
      },
      asio::detached);
  ioc.run();

  EXPECT_EQ(pending.load(), 0);
  ASSERT_TRUE(c1.has_value());
  ASSERT_TRUE(c2.has_value());
  ASSERT_TRUE(*c1) << c1->error().message;
  ASSERT_TRUE(*c2) << c2->error().message;
  EXPECT_TRUE(file->closed());
  pool.join();
}
