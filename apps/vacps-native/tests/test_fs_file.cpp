#include "fs/file.hpp"
#include "fs/open_options.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using vacps::fs::Flags;

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
    vacps::fs::OpenOptions opts;
    opts.flags = Flags::write_only | Flags::create | Flags::truncate;
    // Force pool path (no Asio executor / probe).
    auto f = vacps::fs::File::open(path, opts, root_);
    ASSERT_TRUE(f) << f.error().message;
    EXPECT_FALSE((*f)->uses_asio_file());
    EXPECT_EQ(
        vacps::fs::flags_to_int((*f)->flags()) &
            vacps::fs::flags_to_int(
                Flags::write_only | Flags::create | Flags::truncate),
        vacps::fs::flags_to_int(
            Flags::write_only | Flags::create | Flags::truncate));
    auto n = (*f)->write_text("hello-file");
    ASSERT_TRUE(n) << n.error().message;
    EXPECT_EQ(*n, 10u);
    ASSERT_TRUE((*f)->close());
    EXPECT_TRUE((*f)->closed());
  }
  {
    vacps::fs::OpenOptions opts;
    opts.flags = Flags::read_only;
    auto f = vacps::fs::File::open(path, opts, root_);
    ASSERT_TRUE(f) << f.error().message;
    auto text = (*f)->read_text();
    ASSERT_TRUE(text) << text.error().message;
    EXPECT_EQ(*text, "hello-file");
  }
}

TEST_F(FileTest, WriteNewFailsIfExists) {
  const auto path = (root_ / "exclusive.txt").string();
  vacps::fs::OpenOptions create;
  create.flags = Flags::write_only | Flags::create | Flags::exclusive;
  auto first = vacps::fs::File::open(path, create, root_);
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE((*first)->write_text("one"));
  ASSERT_TRUE((*first)->close());

  auto second = vacps::fs::File::open(path, create, root_);
  ASSERT_FALSE(second);
}

TEST_F(FileTest, ReadFailsIfMissing) {
  const auto path = (root_ / "missing.txt").string();
  vacps::fs::OpenOptions opts;
  opts.flags = Flags::read_only;
  auto f = vacps::fs::File::open(path, opts, root_);
  ASSERT_FALSE(f);
}

TEST_F(FileTest, ReadAtWriteAtDoNotMoveCursor) {
  const auto path = (root_ / "cursor.bin").string();
  // Need read-write: sequential read after write uses the same handle.
  vacps::fs::OpenOptions wopts;
  wopts.flags = Flags::read_write | Flags::create | Flags::truncate;
  auto f = vacps::fs::File::open(path, wopts, root_);
  ASSERT_TRUE(f) << f.error().message;
  ASSERT_TRUE((*f)->write_text("ABCDEFGH"));
  // Overwrite first two bytes via pwrite without moving sequential cursor.
  // After write_text cursor is at EOF; write_at uses pwrite.
  const std::uint8_t patch[] = {'X', 'Y'};
  auto n = (*f)->write_at(0, std::span<const std::uint8_t>(patch, 2));
  ASSERT_TRUE(n) << n.error().message;
  EXPECT_EQ(*n, 2u);
  // Sequential read from current cursor (EOF) → empty
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
  vacps::fs::OpenOptions opts;
  opts.flags = Flags::write_only | Flags::create | Flags::truncate;
  auto f = vacps::fs::File::open(path, opts, root_);
  ASSERT_TRUE(f);
  ASSERT_TRUE((*f)->write_text("z"));
  ASSERT_TRUE((*f)->close());
  auto r = (*f)->read(4);
  ASSERT_FALSE(r);
}
