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

TEST_F(FsTest, AbsoluteAllowedExceptKernelFs) {
  // Node path-guard: absolute paths OK (e.g. /etc).
  auto etc = vacps::fs::assert_safe_absolute_path("/etc/passwd");
  ASSERT_TRUE(etc) << etc.error().message;
  EXPECT_TRUE(etc->is_absolute());

  auto via_resolve = vacps::fs::resolve_path(root_, "/etc/hosts");
  ASSERT_TRUE(via_resolve) << via_resolve.error().message;

  EXPECT_FALSE(vacps::fs::assert_safe_absolute_path("/proc/self/status"));
  EXPECT_FALSE(vacps::fs::assert_safe_absolute_path("/sys/kernel"));
  EXPECT_FALSE(vacps::fs::assert_safe_absolute_path("/dev/null"));
  EXPECT_FALSE(vacps::fs::resolve_path(root_, "/proc/cpuinfo"));
}

TEST_F(FsTest, RelativeMustStayInWorkspace) {
  auto up = vacps::fs::resolve_path(root_, "../escape");
  EXPECT_FALSE(up);

  auto ok = vacps::fs::resolve_path(root_, "a/b.txt");
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(ok->filename(), "b.txt");
  // Must be under root_
  const auto rel = fs::relative(*ok, fs::absolute(root_));
  EXPECT_FALSE(rel.string().starts_with(".."));
}

TEST_F(FsTest, RelativeRejectsAbsoluteRequirementOnAssert) {
  EXPECT_FALSE(vacps::fs::assert_safe_absolute_path("relative.txt"));
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

  // C string literals truncate at \0 — build an explicit embedded-null path.
  std::string abs = "/tmp/x";
  abs.push_back('\0');
  abs += "y";
  EXPECT_FALSE(vacps::fs::assert_safe_absolute_path(abs));
}

TEST_F(FsTest, EmptyPathRejected) {
  EXPECT_FALSE(vacps::fs::resolve_path(root_, ""));
  EXPECT_FALSE(vacps::fs::assert_safe_absolute_path(""));
}

TEST_F(FsTest, SymlinkEscapeRejected) {
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
  EXPECT_FALSE(r) << (r ? r->string() : r.error().message);

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
