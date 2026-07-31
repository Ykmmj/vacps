#include "process/registry.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/process.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <format>
#include <utility>

#include <unistd.h>

namespace vacps::process {
namespace bp = boost::process;
namespace asio = boost::asio;

namespace {

struct new_process_group {
  boost::system::error_code on_exec_setup(
      bp::posix::default_launcher& /*launcher*/,
      const bp::filesystem::path& /*executable*/,
      const char* const* /*argv*/) {
    if (::setpgid(0, 0) != 0) {
      return boost::system::error_code(errno, boost::system::generic_category());
    }
    return {};
  }
};

std::string make_id(std::uint64_t seq) {
  return std::format("proc_{:016x}", seq);
}

int parse_signal(std::string_view signal) {
  std::string s;
  s.reserve(signal.size());
  for (char c : signal) s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  if (s == "SIGKILL" || s == "KILL") return SIGKILL;
  if (s == "SIGINT" || s == "INT") return SIGINT;
  return SIGTERM;
}

std::string status_string(bool finished, bool timed_out, bool cancelled) {
  if (!finished) return "running";
  if (timed_out) return "timed_out";
  if (cancelled) return "cancelled";
  return "exited";
}

}  // namespace

struct Registry::Entry {
  std::string id;
  bp::pid_type pgid{0};
  std::shared_ptr<bp::process> proc;
  std::shared_ptr<asio::readable_pipe> out_pipe;
  std::shared_ptr<asio::readable_pipe> err_pipe;
  std::shared_ptr<asio::writable_pipe> in_pipe;

  std::string stdout_acc;
  std::string stderr_acc;
  std::size_t hard_max_out{16 * 1024 * 1024};
  std::size_t hard_max_err{16 * 1024 * 1024};
  std::size_t produced_out{0};
  std::size_t produced_err{0};
  bool stdout_truncated{false};
  bool stderr_truncated{false};

  /** Fully done: process_exited && out_eof && err_eof. */
  bool finished{false};
  /** Wait coroutine observed process exit (pipes may still drain). */
  bool process_exited{false};
  bool timed_out{false};
  bool cancelled{false};
  bool stdin_open{false};
  bool out_eof{false};
  bool err_eof{false};
  /** One in-flight stdin write at a time (serializes concurrent write() calls). */
  bool write_busy{false};
  std::int32_t exit_code{0};

  std::vector<std::shared_ptr<asio::steady_timer>> read_waiters;
  std::vector<std::shared_ptr<asio::steady_timer>> write_waiters;
  std::shared_ptr<asio::steady_timer> timeout_timer;
  std::shared_ptr<asio::steady_timer> grace_timer;
  std::shared_ptr<asio::steady_timer> retention_timer;
  /** Monotonic finish order for reclaim (0 = not finished). */
  std::uint64_t finish_seq{0};
};

Registry::Registry(asio::any_io_executor ex, RegistryLimits limits)
    : ex_(std::move(ex)), limits_(limits) {
  if (limits_.max_entries == 0) limits_.max_entries = 128;
  if (limits_.max_total_buffer_bytes == 0) {
    limits_.max_total_buffer_bytes = 64 * 1024 * 1024;
  }
}

Registry::~Registry() { shutdown(); }

void Registry::shutdown() noexcept {
  for (auto& [_, e] : entries_) {
    if (!e) continue;
    if (!e->finished) {
      kill_group(*e, SIGKILL);
      e->process_exited = true;
      e->out_eof = true;
      e->err_eof = true;
      e->finished = true;
      e->cancelled = true;
      e->stdin_open = false;
    }
    if (e->retention_timer) {
      e->retention_timer->cancel();
      e->retention_timer.reset();
    }
    if (e->timeout_timer) {
      e->timeout_timer->cancel();
      e->timeout_timer.reset();
    }
    if (e->grace_timer) {
      e->grace_timer->cancel();
      e->grace_timer.reset();
    }
    notify_waiters(*e);
  }
  entries_.clear();
}

std::size_t Registry::total_buffered_bytes() const noexcept {
  std::size_t n = 0;
  for (const auto& [_, e] : entries_) {
    if (!e) continue;
    n += e->stdout_acc.size();
    n += e->stderr_acc.size();
  }
  return n;
}

bool Registry::erase_entry(const std::string& id) noexcept {
  auto it = entries_.find(id);
  if (it == entries_.end()) return false;
  auto e = it->second;
  if (e) {
    if (!e->finished && !e->process_exited) {
      kill_group(*e, SIGKILL);
    }
    e->process_exited = true;
    e->out_eof = true;
    e->err_eof = true;
    e->finished = true;
    e->stdin_open = false;
    e->stdout_acc.clear();
    e->stdout_acc.shrink_to_fit();
    e->stderr_acc.clear();
    e->stderr_acc.shrink_to_fit();
    if (e->retention_timer) {
      e->retention_timer->cancel();
      e->retention_timer.reset();
    }
    if (e->timeout_timer) {
      e->timeout_timer->cancel();
      e->timeout_timer.reset();
    }
    if (e->grace_timer) {
      e->grace_timer->cancel();
      e->grace_timer.reset();
    }
    notify_waiters(*e);
  }
  entries_.erase(it);
  return true;
}

bool Registry::reclaim_one_finished_oldest() noexcept {
  std::string best_id;
  std::uint64_t best_seq = 0;
  bool found = false;
  for (const auto& [id, e] : entries_) {
    if (!e || !e->finished) continue;
    if (!found || e->finish_seq < best_seq) {
      found = true;
      best_seq = e->finish_seq;
      best_id = id;
    }
  }
  if (!found) return false;
  return erase_entry(best_id);
}

void Registry::reclaim_finished_for_limits() noexcept {
  // Only drop finished entries when strictly over the caps.
  // At capacity (size == max_entries) we keep them until start() needs a free slot.
  while (entries_.size() > limits_.max_entries) {
    if (!reclaim_one_finished_oldest()) break;
  }
  while (total_buffered_bytes() > limits_.max_total_buffer_bytes) {
    if (!reclaim_one_finished_oldest()) break;
  }
}

void Registry::schedule_retention(std::shared_ptr<Entry> e) {
  if (!e || limits_.retention_ms <= 0 || e->retention_timer) return;
  e->retention_timer = std::make_shared<asio::steady_timer>(ex_);
  e->retention_timer->expires_after(std::chrono::milliseconds(limits_.retention_ms));
  const std::string id = e->id;
  e->retention_timer->async_wait([this, id, e](const boost::system::error_code& ec) {
    if (ec || !e || !e->finished) return;
    // Only erase if still the same finished entry.
    erase_entry(id);
  });
}

Result<bool> Registry::close(const std::string& id) {
  if (!find(id)) return false;
  return erase_entry(id);
}

std::shared_ptr<Registry::Entry> Registry::find(const std::string& id) const {
  auto it = entries_.find(id);
  if (it == entries_.end()) return nullptr;
  return it->second;
}

void Registry::notify_waiters(Entry& e) noexcept {
  auto waiters = std::move(e.read_waiters);
  e.read_waiters.clear();
  for (auto& t : waiters) {
    if (t) t->cancel();
  }
  notify_write_waiters(e);
}

void Registry::notify_write_waiters(Entry& e) noexcept {
  auto waiters = std::move(e.write_waiters);
  e.write_waiters.clear();
  for (auto& t : waiters) {
    if (t) t->cancel();
  }
}

void Registry::kill_group(Entry& e, int sig) noexcept {
  if (e.pgid > 0) {
    ::kill(-static_cast<pid_t>(e.pgid), sig);
  }
}

void Registry::on_process_exit(
    Entry& e,
    std::int32_t code,
    bool timed_out,
    bool cancelled) {
  if (e.process_exited) return;
  e.process_exited = true;
  e.exit_code = code;
  if (timed_out) e.timed_out = true;
  if (cancelled) e.cancelled = true;
  e.stdin_open = false;
  if (e.timeout_timer) {
    e.timeout_timer->cancel();
    e.timeout_timer.reset();
  }
  // Grace timer may still escalate SIGKILL if already cancelled; leave until finish.
  try_finish(e);
  notify_waiters(e);
}

void Registry::try_finish(Entry& e) noexcept {
  if (e.finished) return;
  if (!e.process_exited || !e.out_eof || !e.err_eof) return;
  e.finished = true;
  e.stdin_open = false;
  e.finish_seq = ++seq_;  // reuse seq_ for finish order (monotonic enough)
  if (e.timeout_timer) {
    e.timeout_timer->cancel();
    e.timeout_timer.reset();
  }
  if (e.grace_timer) {
    e.grace_timer->cancel();
    e.grace_timer.reset();
  }
  notify_waiters(e);
  // Auto-reclaim after retention window unless client calls close().
  auto sp = find(e.id);
  if (sp) schedule_retention(sp);
  reclaim_finished_for_limits();
}

void Registry::schedule_timeout(std::shared_ptr<Entry> e, std::int32_t timeout_ms) {
  if (!e || timeout_ms <= 0) return;
  e->timeout_timer = std::make_shared<asio::steady_timer>(ex_);
  e->timeout_timer->expires_after(std::chrono::milliseconds(timeout_ms));
  e->timeout_timer->async_wait([this, e](const boost::system::error_code& ec) {
    if (ec || !e || e->finished || e->process_exited) return;
    e->timed_out = true;
    kill_group(*e, SIGKILL);
    // wait coroutine will on_process_exit; pipes still drain to EOF
  });
}

void Registry::schedule_grace_kill(std::shared_ptr<Entry> e, std::int32_t grace_ms) {
  if (!e || grace_ms <= 0) {
    if (e && !e->finished) kill_group(*e, SIGKILL);
    return;
  }
  e->grace_timer = std::make_shared<asio::steady_timer>(ex_);
  e->grace_timer->expires_after(std::chrono::milliseconds(grace_ms));
  e->grace_timer->async_wait([this, e](const boost::system::error_code& ec) {
    if (ec || !e || e->finished) return;
    kill_group(*e, SIGKILL);
  });
}

asio::awaitable<Result<StartInfo>> Registry::start(
    std::vector<std::string> argv,
    StartOptions opts) {
  if (argv.empty() || argv[0].empty()) {
    co_return std::unexpected(Error{"process.start: argv is empty"});
  }

  // Need a free slot: reclaim oldest finished first, then reject if still full of live ones.
  while (entries_.size() >= limits_.max_entries) {
    if (!reclaim_one_finished_oldest()) break;
  }
  reclaim_finished_for_limits();  // also enforce buffer budget
  if (entries_.size() >= limits_.max_entries) {
    co_return std::unexpected(Error{std::format(
        "process.start: too many processes (max_entries={})", limits_.max_entries)});
  }

  try {
    auto entry = std::make_shared<Entry>();
    entry->id = make_id(++seq_);
    entry->hard_max_out = opts.hard_max_stdout;
    entry->hard_max_err = opts.hard_max_stderr;
    entry->out_pipe = std::make_shared<asio::readable_pipe>(ex_);
    entry->err_pipe = std::make_shared<asio::readable_pipe>(ex_);
    entry->in_pipe = std::make_shared<asio::writable_pipe>(ex_);

    std::vector<std::string> args;
    args.reserve(argv.size() > 1 ? argv.size() - 1 : 0);
    for (std::size_t i = 1; i < argv.size(); ++i) args.push_back(std::move(argv[i]));
    const std::string exe = std::move(argv[0]);

    bp::process proc = [&]() {
      if (!opts.cwd.empty()) {
        return bp::process(
            ex_,
            exe,
            args,
            bp::process_stdio{*entry->in_pipe, *entry->out_pipe, *entry->err_pipe},
            bp::process_start_dir(opts.cwd),
            new_process_group{});
      }
      return bp::process(
          ex_,
          exe,
          args,
          bp::process_stdio{*entry->in_pipe, *entry->out_pipe, *entry->err_pipe},
          new_process_group{});
    }();

    entry->pgid = proc.id();
    entry->proc = std::make_shared<bp::process>(std::move(proc));
    entry->stdin_open = true;

    if (opts.close_stdin) {
      boost::system::error_code ec;
      entry->in_pipe->close(ec);
      entry->stdin_open = false;
    }

    entries_[entry->id] = entry;
    schedule_timeout(entry, opts.timeout_ms);

    // Drain stdout
    boost::asio::co_spawn(
        ex_,
        [this, entry]() -> asio::awaitable<void> {
          std::array<char, 4096> buf{};
          while (entry && entry->out_pipe) {
            boost::system::error_code ec;
            auto n = co_await entry->out_pipe->async_read_some(
                asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
            if (ec || n == 0) {
              entry->out_eof = true;
              try_finish(*entry);
              notify_waiters(*entry);
              co_return;
            }
            entry->produced_out += n;
            // Hard caps: per-entry hard_max AND global max_total_buffer_bytes.
            const auto used = total_buffered_bytes();
            const auto global_room =
                used < limits_.max_total_buffer_bytes
                    ? limits_.max_total_buffer_bytes - used
                    : std::size_t{0};
            const auto entry_room =
                entry->stdout_acc.size() < entry->hard_max_out
                    ? entry->hard_max_out - entry->stdout_acc.size()
                    : std::size_t{0};
            const auto room = std::min(entry_room, global_room);
            if (room > 0) {
              entry->stdout_acc.append(buf.data(), std::min(n, room));
            }
            if (n > room) entry->stdout_truncated = true;
            reclaim_finished_for_limits();
            notify_waiters(*entry);
          }
        },
        boost::asio::detached);

    // Drain stderr
    boost::asio::co_spawn(
        ex_,
        [this, entry]() -> asio::awaitable<void> {
          std::array<char, 4096> buf{};
          while (entry && entry->err_pipe) {
            boost::system::error_code ec;
            auto n = co_await entry->err_pipe->async_read_some(
                asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
            if (ec || n == 0) {
              entry->err_eof = true;
              try_finish(*entry);
              notify_waiters(*entry);
              co_return;
            }
            entry->produced_err += n;
            const auto used = total_buffered_bytes();
            const auto global_room =
                used < limits_.max_total_buffer_bytes
                    ? limits_.max_total_buffer_bytes - used
                    : std::size_t{0};
            const auto entry_room =
                entry->stderr_acc.size() < entry->hard_max_err
                    ? entry->hard_max_err - entry->stderr_acc.size()
                    : std::size_t{0};
            const auto room = std::min(entry_room, global_room);
            if (room > 0) {
              entry->stderr_acc.append(buf.data(), std::min(n, room));
            }
            if (n > room) entry->stderr_truncated = true;
            reclaim_finished_for_limits();
            notify_waiters(*entry);
          }
        },
        boost::asio::detached);

    // Wait for exit — do not mark fully finished until both pipes EOF.
    boost::asio::co_spawn(
        ex_,
        [this, entry]() -> asio::awaitable<void> {
          if (!entry->proc) co_return;
          auto [ec, code] = co_await bp::async_execute(
              std::move(*entry->proc), asio::as_tuple(asio::use_awaitable));
          (void)ec;
          const bool timed = entry->timed_out;
          const bool cancelled = entry->cancelled;
          if (timed) {
            kill_group(*entry, SIGKILL);
          }
          on_process_exit(*entry, code, timed, cancelled);
          co_return;
        },
        boost::asio::detached);

    StartInfo info;
    info.id = entry->id;
    info.pid = static_cast<std::int32_t>(entry->pgid);
    co_return info;
  } catch (const boost::system::system_error& e) {
    co_return std::unexpected(Error{std::format("process.start: {}", e.what())});
  } catch (const std::exception& e) {
    co_return std::unexpected(Error{std::format("process.start: {}", e.what())});
  }
}

asio::awaitable<Result<ReadInfo>> Registry::read(std::string id, ReadOptions opts) {
  auto entry = find(id);
  if (!entry) {
    co_return std::unexpected(Error{"process.read: unknown process id"});
  }

  auto has_progress = [&]() {
    return entry->stdout_acc.size() > opts.stdout_offset ||
           entry->stderr_acc.size() > opts.stderr_offset || entry->finished ||
           (entry->process_exited && entry->out_eof && entry->err_eof);
  };

  if (opts.wait_ms > 0 && !has_progress()) {
    auto timer = std::make_shared<asio::steady_timer>(ex_);
    timer->expires_after(std::chrono::milliseconds(opts.wait_ms));
    entry->read_waiters.push_back(timer);
    auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
    // Remove if still present
    auto& w = entry->read_waiters;
    w.erase(std::remove(w.begin(), w.end(), timer), w.end());
  }

  ReadInfo info;
  info.status = status_string(entry->finished, entry->timed_out, entry->cancelled);
  info.exit_code = entry->exit_code;
  info.timed_out = entry->timed_out;
  info.stdin_open = entry->stdin_open && !entry->finished && !entry->process_exited;
  info.stdout_total = entry->stdout_acc.size();
  info.stderr_total = entry->stderr_acc.size();
  info.stdout_produced = entry->produced_out;
  info.stderr_produced = entry->produced_err;
  info.stdout_truncated = entry->stdout_truncated;
  info.stderr_truncated = entry->stderr_truncated;

  const auto maxb = opts.max_bytes > 0 ? opts.max_bytes : 65'536;
  std::size_t budget = maxb;

  if (opts.stdout_offset < entry->stdout_acc.size() && budget > 0) {
    const auto n = std::min(budget, entry->stdout_acc.size() - opts.stdout_offset);
    info.stdout_slice = entry->stdout_acc.substr(opts.stdout_offset, n);
    info.next_stdout_offset = opts.stdout_offset + n;
    budget -= n;
  } else {
    info.next_stdout_offset = std::min(opts.stdout_offset, entry->stdout_acc.size());
  }

  if (opts.stderr_offset < entry->stderr_acc.size() && budget > 0) {
    const auto n = std::min(budget, entry->stderr_acc.size() - opts.stderr_offset);
    info.stderr_slice = entry->stderr_acc.substr(opts.stderr_offset, n);
    info.next_stderr_offset = opts.stderr_offset + n;
  } else {
    info.next_stderr_offset = std::min(opts.stderr_offset, entry->stderr_acc.size());
  }

  const bool unread =
      info.next_stdout_offset < entry->stdout_acc.size() ||
      info.next_stderr_offset < entry->stderr_acc.size();
  // eof only when process exited, both pipes drained, and client has all buffered data.
  info.eof = entry->finished && !unread;
  co_return info;
}

asio::awaitable<Result<std::size_t>> Registry::write(
    std::string id,
    std::string data,
    WriteOptions opts) {
  auto entry = find(id);
  if (!entry) {
    co_return std::unexpected(Error{"process.write: unknown process id"});
  }

  // Serialize writes on this process (single-threaded queue via timer wake).
  while (entry->write_busy) {
    if (entry->finished || !entry->stdin_open) {
      co_return std::unexpected(Error{"process.write: stdin is not available"});
    }
    auto waiter = std::make_shared<asio::steady_timer>(ex_);
    waiter->expires_at(asio::steady_timer::time_point::max());
    entry->write_waiters.push_back(waiter);
    auto [wec] = co_await waiter->async_wait(asio::as_tuple(asio::use_awaitable));
    (void)wec;
    auto& w = entry->write_waiters;
    w.erase(std::remove(w.begin(), w.end(), waiter), w.end());
    // Re-find: entry may have been cleared on shutdown (map still holds shared_ptr).
    if (entry->finished) {
      co_return std::unexpected(Error{"process.write: process finished"});
    }
  }

  if (entry->finished || !entry->stdin_open || !entry->in_pipe) {
    co_return std::unexpected(Error{"process.write: stdin is not available"});
  }

  const std::size_t maxb = opts.max_bytes > 0 ? opts.max_bytes : (1u * 1024u * 1024u);
  if (data.size() > maxb) {
    co_return std::unexpected(Error{std::format(
        "process.write: payload {} bytes exceeds max_bytes {}", data.size(), maxb)});
  }

  entry->write_busy = true;
  struct WriteBusyGuard {
    Entry* e;
    Registry* reg;
    ~WriteBusyGuard() {
      if (e) {
        e->write_busy = false;
        if (reg) reg->notify_write_waiters(*e);
      }
    }
  } busy_guard{entry.get(), this};

  std::shared_ptr<asio::steady_timer> write_timer;
  if (opts.timeout_ms > 0) {
    write_timer = std::make_shared<asio::steady_timer>(ex_);
    write_timer->expires_after(std::chrono::milliseconds(opts.timeout_ms));
    write_timer->async_wait([entry](const boost::system::error_code& ec) {
      if (ec || !entry || !entry->in_pipe || !entry->stdin_open) return;
      boost::system::error_code cancel_ec;
      entry->in_pipe->cancel(cancel_ec);
    });
  }

  std::size_t written = 0;
  try {
    if (!data.empty()) {
      auto [ec, n] = co_await asio::async_write(
          *entry->in_pipe,
          asio::buffer(data),
          asio::as_tuple(asio::use_awaitable));
      if (write_timer) {
        write_timer->cancel();
        write_timer.reset();
      }
      if (ec) {
        if (ec == asio::error::operation_aborted) {
          co_return std::unexpected(Error{"process.write: timed out or cancelled"});
        }
        co_return std::unexpected(Error{std::format("process.write: {}", ec.message())});
      }
      written = n;
    } else if (write_timer) {
      write_timer->cancel();
      write_timer.reset();
    }

    if (opts.close_stdin && entry->in_pipe && entry->stdin_open) {
      boost::system::error_code close_ec;
      entry->in_pipe->close(close_ec);
      entry->stdin_open = false;
    }
    co_return written;
  } catch (const std::exception& e) {
    if (write_timer) write_timer->cancel();
    co_return std::unexpected(Error{std::format("process.write: {}", e.what())});
  }
}

Result<bool> Registry::terminate(
    const std::string& id,
    std::string_view signal,
    std::int32_t grace_ms) {
  auto entry = find(id);
  if (!entry) return std::unexpected(Error{"process.terminate: unknown process id"});
  if (entry->finished || entry->process_exited) return false;

  entry->cancelled = true;
  const int sig = parse_signal(signal);
  kill_group(*entry, sig);
  if (sig != SIGKILL) {
    schedule_grace_kill(entry, grace_ms);
  }
  notify_waiters(*entry);
  return true;
}

Result<ReadInfo> Registry::snapshot(const std::string& id) const {
  auto entry = find(id);
  if (!entry) return std::unexpected(Error{"process.snapshot: unknown process id"});
  ReadInfo info;
  info.status = status_string(entry->finished, entry->timed_out, entry->cancelled);
  info.exit_code = entry->exit_code;
  info.timed_out = entry->timed_out;
  info.stdin_open = entry->stdin_open && !entry->finished && !entry->process_exited;
  info.stdout_total = entry->stdout_acc.size();
  info.stderr_total = entry->stderr_acc.size();
  info.stdout_produced = entry->produced_out;
  info.stderr_produced = entry->produced_err;
  info.stdout_truncated = entry->stdout_truncated;
  info.stderr_truncated = entry->stderr_truncated;
  info.stdout_slice = entry->stdout_acc;
  info.stderr_slice = entry->stderr_acc;
  info.next_stdout_offset = entry->stdout_acc.size();
  info.next_stderr_offset = entry->stderr_acc.size();
  info.eof = entry->finished;
  return info;
}

}  // namespace vacps::process
