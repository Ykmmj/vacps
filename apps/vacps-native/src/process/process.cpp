#include "process/process.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/process.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <format>
#include <utility>

#include <unistd.h>

namespace vacps::process {
namespace bp = boost::process;
namespace asio = boost::asio;

namespace {

/**
 * Child-side pre-exec setup (async-signal-safe only).
 * - New process group so terminate/timeout can signal the whole tree.
 * - Restore SIGPIPE=SIG_DFL: ignored dispositions survive exec, and children
 *   should observe the normal default (terminate) unless they opt out.
 */
struct child_pre_exec {
  boost::system::error_code on_exec_setup(
      bp::posix::default_launcher& /*launcher*/,
      const bp::filesystem::path& /*executable*/,
      const char* const* /*argv*/) {
    if (::setpgid(0, 0) != 0) {
      return boost::system::error_code(errno, boost::system::generic_category());
    }
    // sigemptyset / sigaction are async-signal-safe on POSIX.
    struct ::sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    if (::sigemptyset(&sa.sa_mask) != 0) {
      return boost::system::error_code(errno, boost::system::generic_category());
    }
    if (::sigaction(SIGPIPE, &sa, nullptr) != 0) {
      return boost::system::error_code(errno, boost::system::generic_category());
    }
    return {};
  }
};

[[nodiscard]] int system_code_of(const boost::system::error_code& ec) noexcept {
  if (!ec) {
    return 0;
  }
  if (ec.category() == boost::system::system_category() ||
      ec.category() == asio::error::get_system_category()) {
    return ec.value();
  }
  if (ec == asio::error::broken_pipe) {
    return EPIPE;
  }
  return 0;
}

}  // namespace

Result<int> decode_terminate_signal(std::string_view signal) {
  if (signal.empty() || signal == "SIGTERM") {
    return SIGTERM;
  }
  if (signal == "SIGINT") {
    return SIGINT;
  }
  if (signal == "SIGKILL") {
    return SIGKILL;
  }
  return std::unexpected(Error{std::format(
      "Process.terminate: unsupported signal '{}' (use SIGTERM, SIGINT, or SIGKILL)",
      signal)});
}

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
  std::size_t max_out{16 * 1024 * 1024};
  std::size_t max_err{16 * 1024 * 1024};
  std::size_t produced_out{0};
  std::size_t produced_err{0};
  bool stdout_truncated{false};
  bool stderr_truncated{false};

  bool finished{false};
  bool process_exited{false};
  bool timed_out{false};
  bool cancelled{false};
  bool stdin_open{false};
  bool out_eof{false};
  bool err_eof{false};
  bool write_busy{false};
  bool start_called{false};
  bool closing_{false};
  ProcessStatus status{ProcessStatus::Created};
  std::int32_t exit_code{0};

  /** Join barrier for wait() and async_close drain. */
  std::vector<std::shared_ptr<asio::steady_timer>> finish_waiters;
  std::vector<std::shared_ptr<asio::steady_timer>> write_waiters;
  std::shared_ptr<asio::steady_timer> timeout_timer;
  std::shared_ptr<asio::steady_timer> grace_timer;

  void notify_finish_waiters() noexcept {
    try {
      auto waiters = std::move(finish_waiters);
      finish_waiters.clear();
      for (auto& t : waiters) {
        if (t) {
          t->cancel();
        }
      }
    } catch (...) {
    }
  }

  void notify_write_waiters() noexcept {
    try {
      auto ww = std::move(write_waiters);
      write_waiters.clear();
      for (auto& t : ww) {
        if (t) {
          t->cancel();
        }
      }
    } catch (...) {
    }
  }

  void kill_group(int sig) noexcept {
    if (pgid > 0) {
      ::kill(-static_cast<pid_t>(pgid), sig);
    }
  }

  /** Cancel pipes/timers without forging exit/eof completion flags. */
  void cancel_io() noexcept {
    try {
      if (timeout_timer) {
        timeout_timer->cancel();
      }
    } catch (...) {
    }
    try {
      if (grace_timer) {
        grace_timer->cancel();
      }
    } catch (...) {
    }
    try {
      if (in_pipe) {
        boost::system::error_code ec;
        in_pipe->cancel(ec);
        in_pipe->close(ec);
      }
    } catch (...) {
    }
    stdin_open = false;
    try {
      if (out_pipe) {
        boost::system::error_code ec;
        out_pipe->cancel(ec);
      }
    } catch (...) {
    }
    try {
      if (err_pipe) {
        boost::system::error_code ec;
        err_pipe->cancel(ec);
      }
    } catch (...) {
    }
    notify_write_waiters();
    notify_finish_waiters();
  }

  void clear_buffers() noexcept {
    try {
      if (budget) {
        budget->sub_buffered(stdout_acc.size() + stderr_acc.size());
      }
      stdout_acc.clear();
      stderr_acc.clear();
      // No shrink_to_fit: may run from noexcept dispose/finalize paths.
    } catch (...) {
    }
  }

  void try_finish() noexcept {
    if (finished) {
      return;
    }
    if (!process_exited || !out_eof || !err_eof) {
      return;
    }
    finished = true;
    stdin_open = false;
    // Preserve Closing until finalize_close; otherwise publish terminal status.
    // No allocation here (noexcept): wait() copies acc while the handle is open.
    if (!closing_) {
      if (timed_out) {
        status = ProcessStatus::TimedOut;
      } else if (cancelled) {
        status = ProcessStatus::Cancelled;
      } else {
        status = ProcessStatus::Exited;
      }
    }
    try {
      if (timeout_timer) {
        timeout_timer->cancel();
        timeout_timer.reset();
      }
    } catch (...) {
    }
    try {
      if (grace_timer) {
        grace_timer->cancel();
        grace_timer.reset();
      }
    } catch (...) {
    }
    notify_finish_waiters();
    // Finalize close only when an explicit close was requested.
    if (closing_) {
      finalize_close();
    }
  }

  void on_process_exit(std::int32_t code) noexcept {
    if (process_exited) {
      return;
    }
    process_exited = true;
    exit_code = code;
    stdin_open = false;
    try {
      if (timeout_timer) {
        timeout_timer->cancel();
        timeout_timer.reset();
      }
    } catch (...) {
    }
    try_finish();
  }

  /**
   * Enter close path without waiting.
   * Created (not started) → Closed immediately.
   * Running/Exited… → Closing + SIGKILL + cancel I/O; finalize if already drained.
   * Closing → re-assert kill/cancel.
   * Closed → no-op.
   */
  void begin_close() noexcept {
    if (status == ProcessStatus::Closed) {
      return;
    }
    if (!start_called) {
      status = ProcessStatus::Closed;
      closing_ = false;
      notify_finish_waiters();
      notify_write_waiters();
      return;
    }
    if (status == ProcessStatus::Closed) {
      return;
    }
    closing_ = true;
    if (status != ProcessStatus::Closing && status != ProcessStatus::Closed) {
      status = ProcessStatus::Closing;
    }
    cancelled = true;
    kill_group(SIGKILL);
    cancel_io();
    if (finished) {
      finalize_close();
    }
  }

  void finalize_close() noexcept {
    if (status == ProcessStatus::Closed) {
      return;
    }
    status = ProcessStatus::Closed;
    closing_ = false;
    clear_buffers();
    try {
      proc.reset();
    } catch (...) {
    }
    try {
      out_pipe.reset();
    } catch (...) {
    }
    try {
      err_pipe.reset();
    } catch (...) {
    }
    try {
      in_pipe.reset();
    } catch (...) {
    }
    try {
      timeout_timer.reset();
    } catch (...) {
    }
    try {
      grace_timer.reset();
    } catch (...) {
    }
    slot.reset();
    notify_finish_waiters();
    notify_write_waiters();
  }

  void append_capture(
      std::string& acc,
      std::size_t max_bytes,
      bool& truncated,
      std::size_t& produced,
      const char* data,
      std::size_t n) {
    produced += n;
    const auto global_room = budget ? budget->global_buffer_room() : n;
    const auto entry_room =
        acc.size() < max_bytes ? max_bytes - acc.size() : std::size_t{0};
    const auto room = std::min(entry_room, global_room);
    if (room > 0) {
      const auto take = std::min(n, room);
      acc.append(data, take);
      if (budget) {
        budget->add_buffered(take);
      }
    }
    if (n > room) {
      truncated = true;
    }
  }

  asio::awaitable<void> wait_until_finished_or_closed() {
    while (!finished && status != ProcessStatus::Closed) {
      auto gate = std::make_shared<asio::steady_timer>(ex);
      gate->expires_at(asio::steady_timer::time_point::max());
      finish_waiters.push_back(gate);
      if (finished || status == ProcessStatus::Closed) {
        gate->cancel();
      }
      co_await gate->async_wait(asio::as_tuple);
    }
    co_return;
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
  state_->max_out = opts_.max_stdout_bytes;
  state_->max_err = opts_.max_stderr_bytes;
}

Process::~Process() {
  dispose();
}

void Process::dispose() noexcept {
  auto st = state_;
  if (!st) {
    return;
  }
  // Any-thread safe: only post onto the owner executor. Do not read/write
  // mutable State fields on the calling thread (finalizer / stop_callback).
  try {
    // Post keeps a State ref until the handler runs; drain/reap ops retain further.
    asio::post(st->ex, [st]() noexcept {
      try {
        st->begin_close();
      } catch (...) {
      }
    });
  } catch (...) {
    // Last resort if the executor cannot accept work.
    try {
      st->kill_group(SIGKILL);
      st->cancel_io();
    } catch (...) {
    }
  }
}

bool Process::closed() const noexcept {
  return !state_ || state_->status == ProcessStatus::Closed;
}

bool Process::started() const noexcept {
  return state_ && state_->start_called &&
         state_->status != ProcessStatus::Closed;
}

ProcessStatus Process::status() const noexcept {
  if (!state_) {
    return ProcessStatus::Closed;
  }
  return state_->status;
}

asio::awaitable<VoidResult> Process::start() {
  if (!state_) {
    co_return std::unexpected(Error{"Process.start: process is closed"});
  }
  if (state_->status == ProcessStatus::Closed || state_->closing_) {
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

  auto fail_spawn = [st = state_](std::string msg) -> VoidResult {
    // Kill any partially launched child; release slot; allow retry.
    if (st->pgid > 0) {
      st->kill_group(SIGKILL);
    }
    if (st->proc) {
      try {
        st->proc.reset();
      } catch (...) {
      }
    }
    try {
      st->out_pipe.reset();
      st->err_pipe.reset();
      st->in_pipe.reset();
    } catch (...) {
    }
    st->pgid = 0;
    st->start_called = false;
    st->stdin_open = false;
    st->slot.reset();
    st->status = ProcessStatus::Created;
    return std::unexpected(Error{std::move(msg)});
  };

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
            bp::process_stdio{
                *state_->in_pipe, *state_->out_pipe, *state_->err_pipe},
            bp::process_start_dir(opts_.cwd),
            child_pre_exec{});
      }
      return bp::process(
          state_->ex,
          exe,
          args,
          bp::process_stdio{
              *state_->in_pipe, *state_->out_pipe, *state_->err_pipe},
          child_pre_exec{});
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
        if (ec || !st || st->finished || st->process_exited ||
            st->status == ProcessStatus::Closed) {
          return;
        }
        st->timed_out = true;
        st->kill_group(SIGKILL);
      });
    }

    // Drain stdout — local pipe hold so cleanup cannot free under this op.
    boost::asio::co_spawn(
        st->ex,
        [st]() -> asio::awaitable<void> {
          auto pipe = st->out_pipe;
          if (!pipe) {
            st->out_eof = true;
            st->try_finish();
            co_return;
          }
          std::array<char, 4096> buf{};
          for (;;) {
            boost::system::error_code ec;
            auto n = co_await pipe->async_read_some(
                asio::buffer(buf),
                asio::redirect_error(ec));
            if (ec || n == 0) {
              st->out_eof = true;
              st->try_finish();
              co_return;
            }
            st->append_capture(
                st->stdout_acc,
                st->max_out,
                st->stdout_truncated,
                st->produced_out,
                buf.data(),
                n);
          }
        },
        boost::asio::detached);

    // Drain stderr
    boost::asio::co_spawn(
        st->ex,
        [st]() -> asio::awaitable<void> {
          auto pipe = st->err_pipe;
          if (!pipe) {
            st->err_eof = true;
            st->try_finish();
            co_return;
          }
          std::array<char, 4096> buf{};
          for (;;) {
            boost::system::error_code ec;
            auto n = co_await pipe->async_read_some(
                asio::buffer(buf),
                asio::redirect_error(ec));
            if (ec || n == 0) {
              st->err_eof = true;
              st->try_finish();
              co_return;
            }
            st->append_capture(
                st->stderr_acc,
                st->max_err,
                st->stderr_truncated,
                st->produced_err,
                buf.data(),
                n);
          }
        },
        boost::asio::detached);

    // Wait for real child reap (async_execute owns the process object).
    boost::asio::co_spawn(
        st->ex,
        [st]() -> asio::awaitable<void> {
          if (!st->proc) {
            st->on_process_exit(-1);
            co_return;
          }
          auto child = std::move(*st->proc);
          auto [ec, code] = co_await bp::async_execute(
              std::move(child), asio::as_tuple);
          (void)ec;
          if (st->timed_out) {
            st->kill_group(SIGKILL);
          }
          st->on_process_exit(code);
          co_return;
        },
        boost::asio::detached);

    co_return success();
  } catch (const boost::system::system_error& e) {
    co_return fail_spawn(std::format("process.start: {}", e.what()));
  } catch (const std::exception& e) {
    co_return fail_spawn(std::format("process.start: {}", e.what()));
  } catch (...) {
    co_return fail_spawn("process.start: unknown exception");
  }
}

asio::awaitable<Result<std::size_t>> Process::write(std::string data) {
  if (!state_ || state_->status == ProcessStatus::Closed || state_->closing_) {
    co_return std::unexpected(Error{"Process.write: process is closed"});
  }
  if (!state_->start_called) {
    co_return std::unexpected(Error{"Process.write: process not started"});
  }

  while (state_->write_busy) {
    if (state_->finished || !state_->stdin_open ||
        state_->status == ProcessStatus::Closed || state_->closing_) {
      co_return std::unexpected(Error{"Process.write: stdin is not available"});
    }
    auto waiter = std::make_shared<asio::steady_timer>(state_->ex);
    waiter->expires_at(asio::steady_timer::time_point::max());
    state_->write_waiters.push_back(waiter);
    auto [wec] =
        co_await waiter->async_wait(asio::as_tuple);
    (void)wec;
    auto& w = state_->write_waiters;
    w.erase(std::remove(w.begin(), w.end(), waiter), w.end());
    if (state_->finished || state_->status == ProcessStatus::Closed ||
        state_->closing_) {
      co_return std::unexpected(Error{"Process.write: process finished"});
    }
  }

  if (state_->finished || !state_->stdin_open || !state_->in_pipe ||
      state_->closing_ || state_->status == ProcessStatus::Closed) {
    co_return std::unexpected(Error{"Process.write: stdin is not available"});
  }

  state_->write_busy = true;
  struct Guard {
    State* s;
    ~Guard() {
      if (s) {
        s->write_busy = false;
        s->notify_write_waiters();
      }
    }
  } guard{state_.get()};

  // Local hold so State cleanup cannot free the pipe under this write.
  auto pipe = state_->in_pipe;
  if (!pipe || !state_->stdin_open) {
    co_return std::unexpected(Error{"Process.write: stdin is not available"});
  }

  try {
    if (data.empty()) {
      co_return std::size_t{0};
    }
    auto [ec, n] = co_await asio::async_write(
        *pipe, asio::buffer(data), asio::as_tuple);
    if (ec) {
      if (ec == asio::error::operation_aborted) {
        co_return std::unexpected(
            Error{"Process.write: cancelled", "write", 0});
      }
      // Closed child stdin → EPIPE / broken_pipe (parent must not die: SIGPIPE
      // is ignored process-wide via bootstrap::initialize_process).
      co_return std::unexpected(Error{
          std::format("Process.write: {}", ec.message()),
          "write",
          system_code_of(ec)});
    }
    co_return n;
  } catch (const std::exception& e) {
    co_return std::unexpected(
        Error{std::format("Process.write: {}", e.what())});
  }
}

asio::awaitable<Result<RunResult>> Process::wait() {
  if (!state_ || state_->status == ProcessStatus::Closed) {
    co_return std::unexpected(Error{"Process.wait: process is closed"});
  }
  if (!state_->start_called) {
    co_return std::unexpected(Error{"Process.wait: process not started"});
  }

  if (!state_->finished) {
    co_await state_->wait_until_finished_or_closed();
  }

  // close() releases captured buffers; an outstanding/late wait fails honestly.
  if (state_->status == ProcessStatus::Closed) {
    co_return std::unexpected(Error{"Process.wait: process is closed"});
  }
  if (!state_->finished) {
    co_return std::unexpected(
        Error{"Process.wait: process closed before completion"});
  }

  // Copy while the handle is still open (async_close clears acc).
  RunResult r;
  r.exit_code = state_->exit_code;
  r.timed_out = state_->timed_out;
  r.stdout_str = state_->stdout_acc;
  r.stderr_str = state_->stderr_acc;
  co_return r;
}

VoidResult Process::terminate(int signal) {
  if (!state_ || state_->status == ProcessStatus::Closed || state_->closing_) {
    return std::unexpected(Error{"Process.terminate: process is closed"});
  }
  if (!state_->start_called) {
    return std::unexpected(Error{"Process.terminate: process not started"});
  }
  if (state_->finished || state_->process_exited) {
    return success();
  }

  state_->cancelled = true;
  state_->kill_group(signal);
  if (signal != SIGKILL) {
    // Fire-and-forget escalate; does not delay this call.
    auto st = state_;
    st->grace_timer = std::make_shared<asio::steady_timer>(st->ex);
    st->grace_timer->expires_after(std::chrono::milliseconds{3000});
    st->grace_timer->async_wait([st](const boost::system::error_code& ec) {
      if (ec || !st || st->finished || st->status == ProcessStatus::Closed) {
        return;
      }
      st->kill_group(SIGKILL);
    });
  }
  state_->notify_finish_waiters();
  return success();
}

asio::awaitable<VoidResult> Process::async_close() {
  auto st = state_;
  if (!st) {
    co_return success();
  }

  // Drain barrier must not inherit caller cancellation (same as Server.close).
  co_await asio::this_coro::reset_cancellation_state(
      asio::disable_cancellation());

  if (st->status == ProcessStatus::Closed) {
    co_return success();
  }

  st->begin_close();

  while (st->status != ProcessStatus::Closed) {
    if (st->finished) {
      st->finalize_close();
      break;
    }
    auto gate = std::make_shared<asio::steady_timer>(st->ex);
    gate->expires_at(asio::steady_timer::time_point::max());
    st->finish_waiters.push_back(gate);
    if (st->status == ProcessStatus::Closed || st->finished) {
      gate->cancel();
    }
    co_await gate->async_wait(asio::as_tuple);
  }
  co_return success();
}

}  // namespace vacps::process
