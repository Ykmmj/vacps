#include "fs/fs.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

class FsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / "vacps_fs_test" /
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

TEST_F(FsTest, AbsolutePathsIncludingKernelFs) {
  auto etc = vacps::fs::resolve_path(root_, "/etc/passwd");
  ASSERT_TRUE(etc) << etc.error().message;
  EXPECT_TRUE(etc->is_absolute());

  // Pure I/O: no product ban on /proc /sys /dev (JS path-guard owns that).
  auto proc = vacps::fs::resolve_path(root_, "/proc/self/status");
  ASSERT_TRUE(proc) << proc.error().message;
  EXPECT_EQ(proc->string(), "/proc/self/status");

  auto sys = vacps::fs::resolve_path(root_, "/sys/kernel");
  ASSERT_TRUE(sys) << sys.error().message;

  auto dev = vacps::fs::resolve_path(root_, "/dev/null");
  ASSERT_TRUE(dev) << dev.error().message;
}

TEST_F(FsTest, RelativeJoinsWorkspaceNoPolicy) {
  // ".." is allowed at the I/O layer; tool APIs must filter in JS.
  auto up = vacps::fs::resolve_path(root_, "../escape");
  ASSERT_TRUE(up) << up.error().message;
  EXPECT_EQ(up->filename(), "escape");

  auto ok = vacps::fs::resolve_path(root_, "a/b.txt");
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(ok->filename(), "b.txt");
  const auto rel = fs::relative(*ok, fs::absolute(root_));
  EXPECT_FALSE(rel.string().starts_with(".."));
}

TEST_F(FsTest, FileStatMetadata) {
  const auto f = root_ / "stat-me.txt";
  ASSERT_TRUE(vacps::fs::write_text(f, "hello-stat"));
  auto st = vacps::fs::file_stat(f);
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(st->type, "file");
  EXPECT_EQ(st->size_bytes, 10u);
  EXPECT_GT(st->modified_at_ms, 0);
  EXPECT_TRUE(st->readable);

  auto dst = vacps::fs::file_stat(root_);
  ASSERT_TRUE(dst) << dst.error().message;
  EXPECT_EQ(dst->type, "directory");
}

TEST_F(FsTest, WriteReadTextAndList) {
  auto path = vacps::fs::resolve_path(root_, "dir/hello.txt");
  ASSERT_TRUE(path) << path.error().message;

  auto wr = vacps::fs::write_text(*path, "hello-vacps");
  ASSERT_TRUE(wr) << wr.error().message;

  auto rd = vacps::fs::read_text(*path);
  ASSERT_TRUE(rd) << rd.error().message;
  EXPECT_EQ(*rd, "hello-vacps");

  EXPECT_TRUE(vacps::fs::exists(*path));

  auto dir = vacps::fs::resolve_path(root_, "dir");
  ASSERT_TRUE(dir);
  auto listing = vacps::fs::list_dir(*dir);
  ASSERT_TRUE(listing) << listing.error().message;
  ASSERT_EQ(listing->size(), 1u);
  EXPECT_EQ(listing->front().name, "hello.txt");
  EXPECT_TRUE(listing->front().is_file);
}

TEST_F(FsTest, AbsoluteWriteUnderTemp) {
  const auto abs = (root_ / "abs-write.txt").string();
  auto path = vacps::fs::resolve_path(root_, abs);
  ASSERT_TRUE(path) << path.error().message;
  ASSERT_TRUE(vacps::fs::write_text(*path, "via-abs"));
  auto rd = vacps::fs::read_text(*path);
  ASSERT_TRUE(rd);
  EXPECT_EQ(*rd, "via-abs");
}

TEST_F(FsTest, AppendAndRemove) {
  auto path = vacps::fs::resolve_path(root_, "a.txt");
  ASSERT_TRUE(path);
  ASSERT_TRUE(vacps::fs::write_text(*path, "a"));
  ASSERT_TRUE(vacps::fs::append_text(*path, "b"));
  auto rd = vacps::fs::read_text(*path);
  ASSERT_TRUE(rd);
  EXPECT_EQ(*rd, "ab");
  ASSERT_TRUE(vacps::fs::remove_path(*path));
  EXPECT_FALSE(vacps::fs::exists(*path));
}

TEST_F(FsTest, NullByteRejected) {
  std::string rel = "a";
  rel.push_back('\0');
  rel += "b";
  EXPECT_FALSE(vacps::fs::resolve_path(root_, rel));

  std::string abs = "/tmp/x";
  abs.push_back('\0');
  abs += "y";
  EXPECT_FALSE(vacps::fs::resolve_path(root_, abs));
}

TEST_F(FsTest, EmptyPathRejected) {
  EXPECT_FALSE(vacps::fs::resolve_path(root_, ""));
}

TEST_F(FsTest, SymlinkResolveIsPureIo) {
  // I/O layer does not inspect or reject symlink targets.
  const auto outside = fs::temp_directory_path() / "vacps_fs_outside" /
                       std::to_string(::getpid());
  fs::create_directories(outside);
  const auto secret = outside / "secret.txt";
  ASSERT_TRUE(vacps::fs::write_text(secret, "nope"));

  const auto link = root_ / "escape-link";
  std::error_code ec;
  fs::create_symlink(secret, link, ec);
  if (ec) {
    GTEST_SKIP() << "symlink not supported: " << ec.message();
  }

  auto r = vacps::fs::resolve_path(root_, "escape-link");
  ASSERT_TRUE(r) << r.error().message;
  auto text = vacps::fs::read_text(*r);
  ASSERT_TRUE(text) << text.error().message;
  EXPECT_EQ(*text, "nope");

  fs::remove_all(outside, ec);
}

TEST_F(FsTest, NestedRelativeOk) {
  auto p = vacps::fs::resolve_path(root_, "x/y/z.txt");
  ASSERT_TRUE(p) << p.error().message;
  ASSERT_TRUE(vacps::fs::write_text(*p, "z"));
  auto rd = vacps::fs::read_text(*p);
  ASSERT_TRUE(rd);
  EXPECT_EQ(*rd, "z");
}

TEST_F(FsTest, ReadProcLoadavgWhenPresent) {
  auto path = vacps::fs::resolve_path(root_, "/proc/loadavg");
  ASSERT_TRUE(path);
  auto text = vacps::fs::read_text(*path);
  if (!text) {
    GTEST_SKIP() << "no /proc/loadavg: " << text.error().message;
  }
  EXPECT_FALSE(text->empty());
}
