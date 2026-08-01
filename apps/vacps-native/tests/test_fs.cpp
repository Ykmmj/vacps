#include "crypto/crypto.hpp"
#include "fs/file.hpp"
#include "fs/fs.hpp"
#include "fs/open_options.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;
using vacps::Result;
using vacps::VoidResult;
using vacps::fs::OpenMode;

namespace {

VoidResult write_text_via_file(const fs::path& path, std::string_view data) {
  vacps::fs::OpenOptions opts{.mode = OpenMode::write};
  auto f = vacps::fs::File::open(path.string(), opts);
  if (!f) return std::unexpected(std::move(f.error()));
  auto n = (*f)->write_text(data);
  if (!n) return std::unexpected(std::move(n.error()));
  return (*f)->close();
}

Result<std::string> read_text_via_file(const fs::path& path) {
  vacps::fs::OpenOptions opts{.mode = OpenMode::read};
  auto f = vacps::fs::File::open(path.string(), opts);
  if (!f) return std::unexpected(std::move(f.error()));
  auto text = (*f)->read_text();
  if (!text) return std::unexpected(std::move(text.error()));
  auto closed = (*f)->close();
  if (!closed) return std::unexpected(std::move(closed.error()));
  return *text;
}

VoidResult append_text_via_file(const fs::path& path, std::string_view data) {
  vacps::fs::OpenOptions opts{.mode = OpenMode::append};
  auto f = vacps::fs::File::open(path.string(), opts);
  if (!f) return std::unexpected(std::move(f.error()));
  auto n = (*f)->write_text(data);
  if (!n) return std::unexpected(std::move(n.error()));
  return (*f)->close();
}

}  // namespace

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
  ASSERT_TRUE(write_text_via_file(f, "hello-stat"));
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

TEST_F(FsTest, ReadAtAndContentHash) {
  const auto f = root_ / "range.bin";
  // 100 bytes: "0123456789" repeated 10 times
  std::string payload;
  for (int i = 0; i < 10; ++i) payload += "0123456789";
  ASSERT_TRUE(write_text_via_file(f, payload));

  vacps::fs::OpenOptions ro{.mode = OpenMode::read};
  auto file = vacps::fs::File::open(f.string(), ro);
  ASSERT_TRUE(file) << file.error().message;
  EXPECT_EQ((*file)->open_mode(), OpenMode::read);

  auto range = (*file)->read_at(10, 5);
  ASSERT_TRUE(range) << range.error().message;
  ASSERT_EQ(range->size(), 5u);
  EXPECT_EQ(std::string(range->begin(), range->end()), "01234");

  auto past = (*file)->read_at(1000, 10);
  ASSERT_TRUE(past);
  EXPECT_TRUE(past->empty());

  auto full = (*file)->read_at(0, 1024);
  ASSERT_TRUE(full) << full.error().message;
  ASSERT_EQ(full->size(), 100u);
  auto dig = vacps::crypto::sha256(
      std::string_view(reinterpret_cast<const char*>(full->data()), full->size()));
  EXPECT_EQ(vacps::crypto::to_hex(dig), vacps::crypto::to_hex(vacps::crypto::sha256(payload)));
  ASSERT_TRUE((*file)->close());
}

TEST_F(FsTest, OpenModeFromString) {
  auto r = vacps::fs::open_mode_from_string("read");
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, OpenMode::read);
  auto w = vacps::fs::open_mode_from_string("write");
  ASSERT_TRUE(w);
  EXPECT_EQ(*w, OpenMode::write);
  auto wn = vacps::fs::open_mode_from_string("write-new");
  ASSERT_TRUE(wn);
  EXPECT_EQ(*wn, OpenMode::write_new);
  auto bad = vacps::fs::open_mode_from_string("r+");
  ASSERT_FALSE(bad);
  EXPECT_STREQ(vacps::fs::open_mode_to_string(OpenMode::append_read), "append-read");
}

TEST_F(FsTest, WriteReadTextAndList) {
  auto path = vacps::fs::resolve_path(root_, "dir/hello.txt");
  ASSERT_TRUE(path) << path.error().message;

  // Parent dir must exist for File open (mkdir is a namespace op).
  ASSERT_TRUE(vacps::fs::mkdir_p(path->parent_path()));

  auto wr = write_text_via_file(*path, "hello-vacps");
  ASSERT_TRUE(wr) << wr.error().message;

  auto rd = read_text_via_file(*path);
  ASSERT_TRUE(rd) << rd.error().message;
  EXPECT_EQ(*rd, "hello-vacps");

  {
    auto ex = vacps::fs::exists(*path);
    ASSERT_TRUE(ex) << ex.error().message;
    EXPECT_TRUE(*ex);
  }

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
  ASSERT_TRUE(write_text_via_file(*path, "via-abs"));
  auto rd = read_text_via_file(*path);
  ASSERT_TRUE(rd);
  EXPECT_EQ(*rd, "via-abs");
}

TEST_F(FsTest, AppendAndRemove) {
  auto path = vacps::fs::resolve_path(root_, "a.txt");
  ASSERT_TRUE(path);
  ASSERT_TRUE(write_text_via_file(*path, "a"));
  ASSERT_TRUE(append_text_via_file(*path, "b"));
  auto rd = read_text_via_file(*path);
  ASSERT_TRUE(rd);
  EXPECT_EQ(*rd, "ab");
  ASSERT_TRUE(vacps::fs::remove_path(*path));
  auto gone = vacps::fs::exists(*path);
  ASSERT_TRUE(gone) << gone.error().message;
  EXPECT_FALSE(*gone);
}

TEST_F(FsTest, RemoveDefaultNonRecursive) {
  const auto dir = root_ / "nonempty";
  ASSERT_TRUE(vacps::fs::mkdir(dir));
  ASSERT_TRUE(write_text_via_file(dir / "child.txt", "x"));

  // Default: non-empty directory fails.
  auto fail = vacps::fs::remove_path(dir);
  EXPECT_FALSE(fail);
  auto still = vacps::fs::exists(dir / "child.txt");
  ASSERT_TRUE(still) << still.error().message;
  EXPECT_TRUE(*still);

  // Empty dir and file succeed without recursive.
  ASSERT_TRUE(vacps::fs::remove_path(dir / "child.txt"));
  ASSERT_TRUE(vacps::fs::remove_path(dir));
  auto gone = vacps::fs::exists(dir);
  ASSERT_TRUE(gone) << gone.error().message;
  EXPECT_FALSE(*gone);
}

TEST_F(FsTest, RemoveRecursive) {
  const auto dir = root_ / "tree";
  const auto nested = dir / "a" / "b";
  ASSERT_TRUE(vacps::fs::mkdir(nested, vacps::fs::MkdirOptions{.recursive = true}));
  ASSERT_TRUE(write_text_via_file(nested / "f.txt", "z"));

  auto fail = vacps::fs::remove_path(dir);
  EXPECT_FALSE(fail);

  ASSERT_TRUE(vacps::fs::remove_path(dir, vacps::fs::RemoveOptions{.recursive = true}));
  auto gone = vacps::fs::exists(dir);
  ASSERT_TRUE(gone) << gone.error().message;
  EXPECT_FALSE(*gone);
}

TEST_F(FsTest, MkdirRecursive) {
  const auto nested = root_ / "m" / "n" / "o";
  auto non_rec = vacps::fs::mkdir(nested);
  EXPECT_FALSE(non_rec);

  ASSERT_TRUE(vacps::fs::mkdir(nested, vacps::fs::MkdirOptions{.recursive = true}));
  auto ok = vacps::fs::exists(nested);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_TRUE(*ok);

  // Last component only when parent exists.
  const auto leaf = root_ / "m" / "n" / "p";
  ASSERT_TRUE(vacps::fs::mkdir(leaf));
  auto leaf_ok = vacps::fs::exists(leaf);
  ASSERT_TRUE(leaf_ok) << leaf_ok.error().message;
  EXPECT_TRUE(*leaf_ok);
}

TEST_F(FsTest, RenameReplace) {
  const auto from = root_ / "from.txt";
  const auto to = root_ / "to.txt";
  ASSERT_TRUE(write_text_via_file(from, "src"));
  ASSERT_TRUE(write_text_via_file(to, "dst"));

  // Default: fail if target exists.
  auto fail = vacps::fs::rename_path(from, to);
  EXPECT_FALSE(fail);
  auto still_from = vacps::fs::exists(from);
  ASSERT_TRUE(still_from);
  EXPECT_TRUE(*still_from);
  auto dst = read_text_via_file(to);
  ASSERT_TRUE(dst);
  EXPECT_EQ(*dst, "dst");

  ASSERT_TRUE(vacps::fs::rename_path(from, to, vacps::fs::RenameOptions{.replace = true}));
  auto gone = vacps::fs::exists(from);
  ASSERT_TRUE(gone);
  EXPECT_FALSE(*gone);
  auto src = read_text_via_file(to);
  ASSERT_TRUE(src);
  EXPECT_EQ(*src, "src");
}

TEST_F(FsTest, ExistsNotFoundAndPermission) {
  auto missing = vacps::fs::exists(root_ / "no-such-file");
  ASSERT_TRUE(missing) << missing.error().message;
  EXPECT_FALSE(*missing);

  const auto present = root_ / "present.txt";
  ASSERT_TRUE(write_text_via_file(present, "y"));
  auto yes = vacps::fs::exists(present);
  ASSERT_TRUE(yes) << yes.error().message;
  EXPECT_TRUE(*yes);

  // Permission: unreadable parent → error (not false). Skip if running as root.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "root bypasses directory search permission";
  }
  const auto locked = root_ / "locked";
  ASSERT_TRUE(vacps::fs::mkdir(locked));
  const auto secret = locked / "secret.txt";
  ASSERT_TRUE(write_text_via_file(secret, "nope"));
  std::error_code ec;
  fs::permissions(locked, fs::perms::none, ec);
  if (ec) {
    GTEST_SKIP() << "cannot chmod test dir: " << ec.message();
  }
  auto denied = vacps::fs::exists(secret);
  // Restore perms before asserts so TearDown can clean up.
  fs::permissions(locked, fs::perms::owner_all, ec);
  ASSERT_FALSE(denied) << "expected permission error, got success="
                       << (denied ? (*denied ? "true" : "false") : "n/a");
  EXPECT_FALSE(denied.error().message.empty());
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
  ASSERT_TRUE(write_text_via_file(secret, "nope"));

  const auto link = root_ / "escape-link";
  std::error_code ec;
  fs::create_symlink(secret, link, ec);
  if (ec) {
    GTEST_SKIP() << "symlink not supported: " << ec.message();
  }

  auto r = vacps::fs::resolve_path(root_, "escape-link");
  ASSERT_TRUE(r) << r.error().message;
  auto text = read_text_via_file(*r);
  ASSERT_TRUE(text) << text.error().message;
  EXPECT_EQ(*text, "nope");

  fs::remove_all(outside, ec);
}

TEST_F(FsTest, NestedRelativeOk) {
  auto p = vacps::fs::resolve_path(root_, "x/y/z.txt");
  ASSERT_TRUE(p) << p.error().message;
  ASSERT_TRUE(vacps::fs::mkdir_p(p->parent_path()));
  ASSERT_TRUE(write_text_via_file(*p, "z"));
  auto rd = read_text_via_file(*p);
  ASSERT_TRUE(rd);
  EXPECT_EQ(*rd, "z");
}

TEST_F(FsTest, ReadProcLoadavgWhenPresent) {
  auto path = vacps::fs::resolve_path(root_, "/proc/loadavg");
  ASSERT_TRUE(path);
  auto text = read_text_via_file(*path);
  if (!text) {
    GTEST_SKIP() << "no /proc/loadavg: " << text.error().message;
  }
  EXPECT_FALSE(text->empty());
}
