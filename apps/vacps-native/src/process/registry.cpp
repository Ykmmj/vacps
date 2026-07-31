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

  bool finished{false};
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
};

Registry::Registry(asio::any_io_executor ex) : ex_(std::move(ex)) {}

Registry::~Registry() { shutdown(); }

void Registry::shutdown() noexcept {
  for (auto& [_, e] : entries_) {
    if (!e || e->finished) continue;
    kill_group(*e, SIGKILL);
    e->finished = true;
    e->cancelled = true;
    notify_waiters(*e);
  }
  entries_.clear();
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

void Registry::mark_finished(
    Entry& e,
    std::string_view /*status*/,
    std::int32_t code,
    bool timed_out) {
  if (e.finished) return;
  e.finished = true;
  e.exit_code = code;
  e.timed_out = timed_out;
  e.stdin_open = false;
  if (e.timeout_timer) {
    e.timeout_timer->cancel();
    e.timeout_timer.reset();
  }
  if (e.grace_timer) {
    e.grace_timer->cancel();
    e.grace_timer.reset();
  }
  notify_waiters(e);
}

void Registry::schedule_timeout(std::shared_ptr<Entry> e, std::int32_t timeout_ms) {
  if (!e || timeout_ms <= 0) return;
  e->timeout_timer = std::make_shared<asio::steady_timer>(ex_);
  e->timeout_timer->expires_after(std::chrono::milliseconds(timeout_ms));
  e->timeout_timer->async_wait([this, e](const boost::system::error_code& ec) {
    if (ec || !e || e->finished) return;
    e->timed_out = true;
    kill_group(*e, SIGKILL);
    // wait coroutine will mark finished when process exits
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
              notify_waiters(*entry);
              co_return;
            }
            if (entry->stdout_acc.size() < entry->hard_max_out) {
              const auto room = entry->hard_max_out - entry->stdout_acc.size();
              entry->stdout_acc.append(buf.data(), std::min(n, room));
            }
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
              notify_waiters(*entry);
              co_return;
            }
            if (entry->stderr_acc.size() < entry->hard_max_err) {
              const auto room = entry->hard_max_err - entry->stderr_acc.size();
              entry->stderr_acc.append(buf.data(), std::min(n, room));
            }
            notify_waiters(*entry);
          }
        },
        boost::asio::detached);

    // Wait for exit
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
            mark_finished(*entry, "timed_out", code, true);
          } else if (cancelled) {
            mark_finished(*entry, "cancelled", code, false);
          } else {
            mark_finished(*entry, "exited", code, false);
          }
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

  auto has_data = [&]() {
    return entry->stdout_acc.size() > opts.stdout_offset ||
           entry->stderr_acc.size() > opts.stderr_offset || entry->finished;
  };

  if (opts.wait_ms > 0 && !has_data()) {
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
  info.stdin_open = entry->stdin_open && !entry->finished;
  info.stdout_total = entry->stdout_acc.size();
  info.stderr_total = entry->stderr_acc.size();

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
  if (entry->finished) return false;

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
  info.stdin_open = entry->stdin_open && !entry->finished;
  info.stdout_total = entry->stdout_acc.size();
  info.stderr_total = entry->stderr_acc.size();
  info.stdout_slice = entry->stdout_acc;
  info.stderr_slice = entry->stderr_acc;
  info.next_stdout_offset = entry->stdout_acc.size();
  info.next_stderr_offset = entry->stderr_acc.size();
  info.eof = entry->finished;
  return info;
}

}  // namespace vacps::process
