#pragma once

#include "app/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vacps::process {

namespace asio = boost::asio;

struct StartOptions {
  std::string cwd;
  /** 0 = no timeout. */
  std::int32_t timeout_ms{0};
  bool close_stdin{true};
  std::size_t hard_max_stdout{16 * 1024 * 1024};
  std::size_t hard_max_stderr{16 * 1024 * 1024};
};

struct StartInfo {
  std::string id;
  std::int32_t pid{0};
};

struct ReadOptions {
  std::int32_t wait_ms{0};
  std::size_t max_bytes{65'536};
  std::size_t stdout_offset{0};
  std::size_t stderr_offset{0};
};

struct ReadInfo {
  std::string status;  // running | exited | timed_out | cancelled
  std::int32_t exit_code{0};
  bool timed_out{false};
  bool eof{false};
  bool stdin_open{false};
  std::string stdout_slice;
  std::string stderr_slice;
  std::size_t stdout_total{0};
  std::size_t stderr_total{0};
  std::size_t next_stdout_offset{0};
  std::size_t next_stderr_offset{0};
};

/**
 * Long-lived subprocess registry (Boost.Process v2 + Asio).
 * Single-threaded: all methods co_awaited / called on the host io_context.
 * Process groups: setpgid + kill(-pgid) on timeout/terminate (design §19.2).
 */
class Registry {
 public:
  explicit Registry(asio::any_io_executor ex);
  ~Registry();

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  [[nodiscard]] asio::awaitable<Result<StartInfo>> start(
      std::vector<std::string> argv,
      StartOptions opts = {});

  [[nodiscard]] asio::awaitable<Result<ReadInfo>> read(
      std::string id,
      ReadOptions opts = {});

  [[nodiscard]] Result<std::size_t> write(
      const std::string& id,
      std::string_view data,
      bool close_stdin);

  /** signal: SIGTERM | SIGINT | SIGKILL (case-insensitive). */
  [[nodiscard]] Result<bool> terminate(
      const std::string& id,
      std::string_view signal = "SIGTERM",
      std::int32_t grace_ms = 3000);

  [[nodiscard]] Result<ReadInfo> snapshot(const std::string& id) const;

  /** Best-effort kill all children (host shutdown). */
  void shutdown() noexcept;

 private:
  struct Entry;

  [[nodiscard]] std::shared_ptr<Entry> find(const std::string& id) const;
  void notify_waiters(Entry& e) noexcept;
  void schedule_timeout(std::shared_ptr<Entry> e, std::int32_t timeout_ms);
  void schedule_grace_kill(std::shared_ptr<Entry> e, std::int32_t grace_ms);
  void kill_group(Entry& e, int sig) noexcept;
  void mark_finished(Entry& e, std::string_view status, std::int32_t code, bool timed_out);

  asio::any_io_executor ex_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
  std::uint64_t seq_{0};
};

}  // namespace vacps::process
