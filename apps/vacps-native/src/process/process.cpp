#include "process/process.hpp"

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

int parse_signal(std::string_view signal) {
  std::string s;
  s.reserve(signal.size());
  for (char c : signal) {
    s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (s == "SIGKILL" || s == "KILL") return SIGKILL;
  if (s == "SIGINT" || s == "INT") return SIGINT;
  return SIGTERM;
}

}  // namespace

struct Process::State {
  asio::any_io_executor ex;
  std::shared_ptr<ProcessBudget> budget;
  ProcessSlot slot;

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
  std::size_t stdout_offset{0};
  std::size_t stderr_offset{0};

  bool finished{false};
  bool process_exited{false};
  bool timed_out{false};
  bool cancelled{false};
  bool stdin_open{false};
  bool out_eof{false};
  bool err_eof{false};
  bool write_busy{false};
  bool closed{false};
  bool start_called{false};
  ProcessStatus status{ProcessStatus::Created};
  std::int32_t exit_code{0};

  std::vector<std::shared_ptr<asio::steady_timer>> read_waiters;
  std::vector<std::shared_ptr<asio::steady_timer>> write_waiters;
  std::shared_ptr<asio::steady_timer> timeout_timer;
  std::shared_ptr<asio::steady_timer> grace_timer;

  void notify_waiters() noexcept {
    auto waiters = std::move(read_waiters);
    read_waiters.clear();
    for (auto& t : waiters) {
      if (t) t->cancel();
    }
    auto ww = std::move(write_waiters);
    write_waiters.clear();
    for (auto& t : ww) {
      if (t) t->cancel();
    }
  }

  void kill_group(int sig) noexcept {
    if (pgid > 0) {
      ::kill(-static_cast<pid_t>(pgid), sig);
    }
  }

  void try_finish() noexcept {
    if (finished) return;
    if (!process_exited || !out_eof || !err_eof) return;
    finished = true;
    stdin_open = false;
    if (timed_out) {
      status = ProcessStatus::TimedOut;
    } else if (cancelled) {
      status = ProcessStatus::Cancelled;
    } else {
      status = ProcessStatus::Exited;
    }
    if (timeout_timer) {
      timeout_timer->cancel();
      timeout_timer.reset();
    }
    if (grace_timer) {
      grace_timer->cancel();
      grace_timer.reset();
    }
    notify_waiters();
  }

  void on_process_exit(std::int32_t code, bool timed, bool cancel) {
    if (process_exited) return;
    process_exited = true;
    exit_code = code;
    if (timed) timed_out = true;
    if (cancel) cancelled = true;
    stdin_open = false;
    if (timeout_timer) {
      timeout_timer->cancel();
      timeout_timer.reset();
    }
    try_finish();
    notify_waiters();
  }

  void clear_buffers() noexcept {
    if (budget) {
      budget->sub_buffered(stdout_acc.size() + stderr_acc.size());
    }
    stdout_acc.clear();
    stdout_acc.shrink_to_fit();
    stderr_acc.clear();
    stderr_acc.shrink_to_fit();
  }

  void force_kill_and_mark_done() noexcept {
    if (!process_exited && pgid > 0) {
      kill_group(SIGKILL);
    }
    process_exited = true;
    out_eof = true;
    err_eof = true;
    finished = true;
    cancelled = true;
    stdin_open = false;
    status = ProcessStatus::Closed;
    if (timeout_timer) {
      timeout_timer->cancel();
      timeout_timer.reset();
    }
    if (grace_timer) {
      grace_timer->cancel();
      grace_timer.reset();
    }
    notify_waiters();
    clear_buffers();
    proc.reset();
    out_pipe.reset();
    err_pipe.reset();
    in_pipe.reset();
    slot.reset();
  }
};

Process::Process(
    asio::any_io_executor executor,
    std::shared_ptr<ProcessBudget> budget,
    std::vector<std::string> argv,
    StartOptions opts)
    : argv_(std::move(argv)),
      opts_(std::move(opts)),
      state_(std::make_shared<State>()) {
  state_->ex = std::move(executor);
  state_->budget = std::move(budget);
  state_->hard_max_out = opts_.hard_max_stdout;
  state_->hard_max_err = opts_.hard_max_stderr;
}

Process::~Process() {
  dispose();
}

void Process::dispose() noexcept {
  if (!state_ || state_->closed) {
    return;
  }
  state_->closed = true;
  state_->status = ProcessStatus::Closing;
  state_->force_kill_and_mark_done();
  state_->status = ProcessStatus::Closed;
}

VoidResult Process::ensure_live(std::string_view op) const {
  if (!state_ || state_->closed) {
    return std::unexpected(Error{std::string(op) + ": process is closed"});
  }
  if (!state_->start_called || !state_->proc) {
    if (state_->start_called && state_->process_exited) {
      return {};  // started and exited is live for read/wait of buffers
    }
    if (!state_->start_called) {
      return std::unexpected(Error{std::string(op) + ": process not started"});
    }
  }
  return {};
}

bool Process::closed() const noexcept {
  return !state_ || state_->closed;
}

bool Process::started() const noexcept {
  return state_ && state_->start_called && !state_->closed;
}

bool Process::running() const noexcept {
  if (!state_ || state_->closed || !state_->start_called) {
    return false;
  }
  return !state_->process_exited && !state_->finished;
}

ProcessStatus Process::status() const noexcept {
  if (!state_) {
    return ProcessStatus::Closed;
  }
  return state_->status;
}

std::optional<std::int32_t> Process::pid() const noexcept {
  if (!state_ || state_->closed || !state_->start_called || state_->pgid <= 0) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>(state_->pgid);
}

asio::awaitable<VoidResult> Process::start() {
  if (!state_ || state_->closed) {
    co_return std::unexpected(Error{"Process.start: process is closed"});
  }
  if (state_->start_called) {
    co_return std::unexpected(Error{"Process.start: already started"});
  }
  if (argv_.empty() || argv_[0].empty()) {
    co_return std::unexpected(Error{"Process.start: argv is empty"});
  }
  if (!state_->budget) {
    co_return std::unexpected(Error{"Process.start: no process budget"});
  }

  state_->status = ProcessStatus::Starting;
  auto acq = state_->budget->try_acquire_process();
  if (!acq) {
    state_->status = ProcessStatus::Created;
    co_return std::unexpected(std::move(acq.error()));
  }
  state_->slot = ProcessSlot{state_->budget};

  try {
    state_->out_pipe = std::make_shared<asio::readable_pipe>(state_->ex);
    state_->err_pipe = std::make_shared<asio::readable_pipe>(state_->ex);
    state_->in_pipe = std::make_shared<asio::writable_pipe>(state_->ex);

    std::vector<std::string> args;
    args.reserve(argv_.size() > 1 ? argv_.size() - 1 : 0);
    for (std::size_t i = 1; i < argv_.size(); ++i) {
      args.push_back(argv_[i]);
    }
    const std::string& exe = argv_[0];

    bp::process proc = [&]() {
      if (!opts_.cwd.empty()) {
        return bp::process(
            state_->ex,
            exe,
            args,
            bp::process_stdio{*state_->in_pipe, *state_->out_pipe, *state_->err_pipe},
            bp::process_start_dir(opts_.cwd),
            new_process_group{});
      }
      return bp::process(
          state_->ex,
          exe,
          args,
          bp::process_stdio{*state_->in_pipe, *state_->out_pipe, *state_->err_pipe},
          new_process_group{});
    }();

    state_->pgid = proc.id();
    state_->proc = std::make_shared<bp::process>(std::move(proc));
    state_->stdin_open = true;
    state_->start_called = true;
    state_->status = ProcessStatus::Running;

    if (opts_.close_stdin) {
      boost::system::error_code ec;
      state_->in_pipe->close(ec);
      state_->stdin_open = false;
    }

    auto st = state_;
    const auto timeout_ms = opts_.timeout.count();
    if (timeout_ms > 0) {
      st->timeout_timer = std::make_shared<asio::steady_timer>(st->ex);
      st->timeout_timer->expires_after(opts_.timeout);
      st->timeout_timer->async_wait([st](const boost::system::error_code& ec) {
        if (ec || !st || st->finished || st->process_exited || st->closed) return;
        st->timed_out = true;
        st->kill_group(SIGKILL);
      });
    }

    // Drain stdout
    boost::asio::co_spawn(
        st->ex,
        [st]() -> asio::awaitable<void> {
          std::array<char, 4096> buf{};
          while (st && st->out_pipe && !st->closed) {
            boost::system::error_code ec;
            auto n = co_await st->out_pipe->async_read_some(
                asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
            if (ec || n == 0) {
              st->out_eof = true;
              st->try_finish();
              st->notify_waiters();
              co_return;
            }
            st->produced_out += n;
            const auto global_room =
                st->budget ? st->budget->global_buffer_room() : n;
            const auto entry_room =
                st->stdout_acc.size() < st->hard_max_out
                    ? st->hard_max_out - st->stdout_acc.size()
                    : std::size_t{0};
            const auto room = std::min(entry_room, global_room);
            if (room > 0) {
              const auto take = std::min(n, room);
              st->stdout_acc.append(buf.data(), take);
              if (st->budget) {
                st->budget->add_buffered(take);
              }
            }
            if (n > room) {
              st->stdout_truncated = true;
            }
            st->notify_waiters();
          }
        },
        boost::asio::detached);

    // Drain stderr
    boost::asio::co_spawn(
        st->ex,
        [st]() -> asio::awaitable<void> {
          std::array<char, 4096> buf{};
          while (st && st->err_pipe && !st->closed) {
            boost::system::error_code ec;
            auto n = co_await st->err_pipe->async_read_some(
                asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
            if (ec || n == 0) {
              st->err_eof = true;
              st->try_finish();
              st->notify_waiters();
              co_return;
            }
            st->produced_err += n;
            const auto global_room =
                st->budget ? st->budget->global_buffer_room() : n;
            const auto entry_room =
                st->stderr_acc.size() < st->hard_max_err
                    ? st->hard_max_err - st->stderr_acc.size()
                    : std::size_t{0};
            const auto room = std::min(entry_room, global_room);
            if (room > 0) {
              const auto take = std::min(n, room);
              st->stderr_acc.append(buf.data(), take);
              if (st->budget) {
                st->budget->add_buffered(take);
              }
            }
            if (n > room) {
              st->stderr_truncated = true;
            }
            st->notify_waiters();
          }
        },
        boost::asio::detached);

    // Wait for exit
    boost::asio::co_spawn(
        st->ex,
        [st]() -> asio::awaitable<void> {
          if (!st->proc) co_return;
          auto [ec, code] = co_await bp::async_execute(
              std::move(*st->proc), asio::as_tuple(asio::use_awaitable));
          (void)ec;
          const bool timed = st->timed_out;
          const bool cancelled = st->cancelled;
          if (timed) {
            st->kill_group(SIGKILL);
          }
          st->on_process_exit(code, timed, cancelled);
          co_return;
        },
        boost::asio::detached);

    co_return success();
  } catch (const boost::system::system_error& e) {
    state_->slot.reset();
    state_->start_called = false;
    state_->status = ProcessStatus::Created;
    co_return std::unexpected(Error{std::format("process.start: {}", e.what())});
  } catch (const std::exception& e) {
    state_->slot.reset();
    state_->start_called = false;
    state_->status = ProcessStatus::Created;
    co_return std::unexpected(Error{std::format("process.start: {}", e.what())});
  }
}

asio::awaitable<Result<ReadResult>> Process::read(
    std::string_view stream,
    std::chrono::milliseconds wait,
    std::size_t max_bytes) {
  if (!state_ || state_->closed) {
    co_return std::unexpected(Error{"Process.read: process is closed"});
  }
  if (!state_->start_called) {
    co_return std::unexpected(Error{"Process.read: process not started"});
  }
  const bool want_stdout = stream == "stdout";
  if (!want_stdout && stream != "stderr") {
    co_return std::unexpected(Error{"Process.read: stream must be stdout or stderr"});
  }

  auto& acc = want_stdout ? state_->stdout_acc : state_->stderr_acc;
  auto& offset = want_stdout ? state_->stdout_offset : state_->stderr_offset;
  auto stream_eof = [&]() {
    return want_stdout ? state_->out_eof : state_->err_eof;
  };

  auto has_progress = [&]() {
    return acc.size() > offset || state_->finished ||
           (stream_eof() && offset >= acc.size());
  };

  if (wait.count() > 0 && !has_progress()) {
    auto timer = std::make_shared<asio::steady_timer>(state_->ex);
    timer->expires_after(wait);
    state_->read_waiters.push_back(timer);
    auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
    auto& w = state_->read_waiters;
    w.erase(std::remove(w.begin(), w.end(), timer), w.end());
  }

  ReadResult out;
  const auto maxb = max_bytes > 0 ? max_bytes : 65'536;
  if (offset < acc.size()) {
    const auto n = std::min(maxb, acc.size() - offset);
    out.data = acc.substr(offset, n);
    offset += n;
  }
  out.eof = state_->finished || (stream_eof() && offset >= acc.size());
  co_return out;
}

asio::awaitable<Result<std::size_t>> Process::write(
    std::string data,
    WriteOptions opts) {
  if (!state_ || state_->closed) {
    co_return std::unexpected(Error{"Process.write: process is closed"});
  }
  if (!state_->start_called) {
    co_return std::unexpected(Error{"Process.write: process not started"});
  }

  while (state_->write_busy) {
    if (state_->finished || !state_->stdin_open || state_->closed) {
      co_return std::unexpected(Error{"Process.write: stdin is not available"});
    }
    auto waiter = std::make_shared<asio::steady_timer>(state_->ex);
    waiter->expires_at(asio::steady_timer::time_point::max());
    state_->write_waiters.push_back(waiter);
    auto [wec] = co_await waiter->async_wait(asio::as_tuple(asio::use_awaitable));
    (void)wec;
    auto& w = state_->write_waiters;
    w.erase(std::remove(w.begin(), w.end(), waiter), w.end());
    if (state_->finished || state_->closed) {
      co_return std::unexpected(Error{"Process.write: process finished"});
    }
  }

  if (state_->finished || !state_->stdin_open || !state_->in_pipe) {
    co_return std::unexpected(Error{"Process.write: stdin is not available"});
  }

  if (data.size() > opts.max_bytes) {
    co_return std::unexpected(Error{std::format(
        "Process.write: payload {} bytes exceeds max_bytes {}",
        data.size(),
        opts.max_bytes)});
  }

  state_->write_busy = true;
  struct Guard {
    State* s;
    ~Guard() {
      if (s) {
        s->write_busy = false;
        s->notify_waiters();
      }
    }
  } guard{state_.get()};

  std::shared_ptr<asio::steady_timer> write_timer;
  if (opts.timeout.count() > 0) {
    write_timer = std::make_shared<asio::steady_timer>(state_->ex);
    write_timer->expires_after(opts.timeout);
    auto st = state_;
    write_timer->async_wait([st](const boost::system::error_code& ec) {
      if (ec || !st || !st->in_pipe || !st->stdin_open) return;
      boost::system::error_code cancel_ec;
      st->in_pipe->cancel(cancel_ec);
    });
  }

  std::size_t written = 0;
  try {
    if (!data.empty()) {
      auto [ec, n] = co_await asio::async_write(
          *state_->in_pipe,
          asio::buffer(data),
          asio::as_tuple(asio::use_awaitable));
      if (write_timer) {
        write_timer->cancel();
        write_timer.reset();
      }
      if (ec) {
        if (ec == asio::error::operation_aborted) {
          co_return std::unexpected(Error{"Process.write: timed out or cancelled"});
        }
        co_return std::unexpected(
            Error{std::format("Process.write: {}", ec.message())});
      }
      written = n;
    } else if (write_timer) {
      write_timer->cancel();
      write_timer.reset();
    }

    if (opts.close_stdin && state_->in_pipe && state_->stdin_open) {
      boost::system::error_code close_ec;
      state_->in_pipe->close(close_ec);
      state_->stdin_open = false;
    }
    co_return written;
  } catch (const std::exception& e) {
    if (write_timer) write_timer->cancel();
    co_return std::unexpected(Error{std::format("Process.write: {}", e.what())});
  }
}

asio::awaitable<Result<RunResult>> Process::wait() {
  if (!state_ || state_->closed) {
    co_return std::unexpected(Error{"Process.wait: process is closed"});
  }
  if (!state_->start_called) {
    co_return std::unexpected(Error{"Process.wait: process not started"});
  }

  while (!state_->finished && !state_->closed) {
    auto timer = std::make_shared<asio::steady_timer>(state_->ex);
    timer->expires_after(std::chrono::milliseconds{60'000});
    state_->read_waiters.push_back(timer);
    auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
    auto& w = state_->read_waiters;
    w.erase(std::remove(w.begin(), w.end(), timer), w.end());
    // Also wake on process_exited + drain — keep waiting until finished for
    // full buffers, but allow building result once finished.
  }

  RunResult r;
  r.exit_code = state_->exit_code;
  r.timed_out = state_->timed_out;
  if (state_->timed_out) {
    r.reason = ExitReason::TimedOut;
  } else if (state_->cancelled) {
    r.reason = ExitReason::Cancelled;
  } else {
    r.reason = ExitReason::Exited;
  }
  r.stdout_str = state_->stdout_acc;
  r.stderr_str = state_->stderr_acc;
  r.stdout_produced = state_->produced_out;
  r.stderr_produced = state_->produced_err;
  r.stdout_truncated = state_->stdout_truncated;
  r.stderr_truncated = state_->stderr_truncated;
  co_return r;
}

Result<bool> Process::terminate(
    std::string_view signal,
    std::chrono::milliseconds grace) {
  if (!state_ || state_->closed) {
    return std::unexpected(Error{"Process.terminate: process is closed"});
  }
  if (!state_->start_called) {
    return std::unexpected(Error{"Process.terminate: process not started"});
  }
  if (state_->finished || state_->process_exited) {
    return false;
  }

  state_->cancelled = true;
  const int sig = parse_signal(signal);
  state_->kill_group(sig);
  if (sig != SIGKILL && grace.count() > 0) {
    auto st = state_;
    st->grace_timer = std::make_shared<asio::steady_timer>(st->ex);
    st->grace_timer->expires_after(grace);
    st->grace_timer->async_wait([st](const boost::system::error_code& ec) {
      if (ec || !st || st->finished || st->closed) return;
      st->kill_group(SIGKILL);
    });
  } else if (sig != SIGKILL && grace.count() <= 0) {
    state_->kill_group(SIGKILL);
  }
  state_->notify_waiters();
  return true;
}

VoidResult Process::close() {
  if (!state_ || state_->closed) {
    return {};
  }
  dispose();
  return {};
}

}  // namespace vacps::process
