#include "fs/sandbox.hpp"
#include "fs/fs.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

class FsSandboxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / "vacps_sandbox_test" /
            std::to_string(::getpid()) /
            std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(root_);
    outside_ = fs::temp_directory_path() / "vacps_sandbox_outside" /
               std::to_string(::getpid()) /
               std::to_string(reinterpret_cast<std::uintptr_t>(this));
    fs::create_directories(outside_);
    sandbox_ = vacps::fs::PathSandbox::create(root_, {});
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::remove_all(outside_, ec);
  }

  fs::path root_;
  fs::path outside_;
  vacps::fs::PathSandbox sandbox_;
};

TEST_F(FsSandboxTest, AllowsUnderDataDir) {
  const auto f = root_ / "ok.txt";
  {
    std::ofstream out(f);
    out << "hello";
  }
  auto a = sandbox_.authorize(f.string(), root_);
  ASSERT_TRUE(a) << a.error().message;
  EXPECT_TRUE(a->string().find(root_.filename().string()) != std::string::npos ||
              fs::equivalent(*a, f));
}

TEST_F(FsSandboxTest, RelativeJoinsDataDir) {
  auto a = sandbox_.authorize("rel/x.txt", root_);
  ASSERT_TRUE(a) << a.error().message;
  EXPECT_EQ(a->filename(), "x.txt");
}

TEST_F(FsSandboxTest, RejectsEtcPasswd) {
  auto a = sandbox_.authorize("/etc/passwd", root_);
  ASSERT_FALSE(a);
  EXPECT_NE(a.error().message.find("outside"), std::string::npos)
      << a.error().message;
}

TEST_F(FsSandboxTest, RejectsProc) {
  auto a = sandbox_.authorize("/proc/self/status", root_);
  ASSERT_FALSE(a);
  EXPECT_NE(a.error().message.find("kernel"), std::string::npos)
      << a.error().message;
}

TEST_F(FsSandboxTest, RejectsSymlinkEscape) {
  // Symlink inside root pointing at a file outside the allowlist.
  const auto secret = outside_ / "secret.txt";
  {
    std::ofstream out(secret);
    out << "top-secret";
  }
  const auto link = root_ / "escape-link";
  std::error_code ec;
  fs::create_symlink(secret, link, ec);
  ASSERT_FALSE(ec) << ec.message();

  auto a = sandbox_.authorize(link.string(), root_);
  ASSERT_FALSE(a) << "symlink escape should be rejected, got " << a->string();
}

TEST_F(FsSandboxTest, AllowsTmp) {
  const auto f = fs::temp_directory_path() / ("vacps_sb_" + std::to_string(::getpid()));
  {
    std::ofstream out(f);
    out << "tmp";
  }
  auto a = sandbox_.authorize(f.string(), root_);
  // /tmp is always a root; path under temp_directory_path should be allowed.
  ASSERT_TRUE(a) << a.error().message;
  std::error_code ec;
  fs::remove(f, ec);
}

TEST_F(FsSandboxTest, KernelFsHelper) {
  EXPECT_TRUE(vacps::fs::is_kernel_filesystem("/proc/self"));
  EXPECT_TRUE(vacps::fs::is_kernel_filesystem("/sys/class"));
  EXPECT_TRUE(vacps::fs::is_kernel_filesystem("/dev/null"));
  EXPECT_FALSE(vacps::fs::is_kernel_filesystem("/etc/passwd"));
  EXPECT_FALSE(vacps::fs::is_kernel_filesystem(root_));
}
