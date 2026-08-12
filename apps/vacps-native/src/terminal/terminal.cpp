#include "terminal/terminal.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/write.hpp>
#include <boost/process.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace vacps::terminal {

namespace asio = boost::asio;
namespace bp = boost::process;

namespace {

struct OperationCancel {
  asio::cancellation_signal signal;
  bool active{true};
};

[[nodiscard]] auto cancel_on_stop(
    std::stop_token stop,
    const asio::any_io_executor& executor,
    const std::shared_ptr<OperationCancel>& operation) {
  std::weak_ptr<OperationCancel> weak = operation;
  return std::stop_callback{
      stop,
      [weak = std::move(weak), executor]() noexcept {
        asio::post(executor, [weak]() noexcept {
          if (auto operation = weak.lock(); operation && operation->active) {
            operation->signal.emit(asio::cancellation_type::all);
          }
        });
      }};
}

class UniqueFd {
 public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_{-1};
};

[[nodiscard]] boost::system::error_code errno_code() noexcept {
  return {errno, boost::system::generic_category()};
}

struct PtyChildSetup {
  int slave_fd{-1};

  boost::system::error_code on_setup(
      bp::posix::default_launcher& launcher,
      const bp::filesystem::path&,
      const char* const*) {
    launcher.fd_whitelist.push_back(slave_fd);
    return {};
  }

  boost::system::error_code on_exec_setup(
      bp::posix::default_launcher&,
      const bp::filesystem::path&,
      const char* const*) noexcept {
    if (::setsid() < 0) {
      return errno_code();
    }
    if (::ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
      return errno_code();
    }

    const pid_t foreground = ::getpid();
    if (::ioctl(slave_fd, TIOCSPGRP, &foreground) < 0) {
      return errno_code();
    }

    for (int target = STDIN_FILENO; target <= STDERR_FILENO; ++target) {
      if (::dup2(slave_fd, target) < 0) {
        return errno_code();
      }
    }
    if (slave_fd > STDERR_FILENO) {
      ::close(slave_fd);
    }

    sigset_t empty{};
    if (::sigemptyset(&empty) != 0 ||
        ::sigprocmask(SIG_SETMASK, &empty, nullptr) != 0) {
      return errno_code();
    }
    struct ::sigaction action {};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    if (::sigemptyset(&action.sa_mask) != 0) {
      return errno_code();
    }
    constexpr std::array signals{
        SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU};
    for (const int signal : signals) {
      if (::sigaction(signal, &action, nullptr) != 0) {
        return errno_code();
      }
    }
    return {};
  }
};

[[nodiscard]] int system_code_of(const boost::system::error_code& ec) noexcept {
  if (ec.category() == boost::system::system_category() ||
      ec.category() == boost::system::generic_category() ||
      ec.category() == asio::error::get_system_category()) {
    return ec.value();
  }
  return 0;
}

}  // namespace

Result<int> decode_signal(std::string_view signal) {
  if (signal == "SIGINT") return SIGINT;
  if (signal == "SIGTERM") return SIGTERM;
  if (signal == "SIGHUP") return SIGHUP;
  if (signal == "SIGKILL") return SIGKILL;
  if (signal == "SIGTSTP") return SIGTSTP;
  if (signal == "SIGCONT") return SIGCONT;
  return std::unexpected(Error{std::format(
      "Terminal.signal: unsupported signal '{}'", signal)});
}

struct Terminal::State {
  struct Chunk {
    std::uint64_t offset{0};
    std::vector<std::uint8_t> data;
  };

  asio::any_io_executor executor;
  std::shared_ptr<process::ProcessBudget> budget;
  process::ProcessSlot slot;
  std::shared_ptr<bp::process> child;
  std::shared_ptr<asio::posix::stream_descriptor> master;
  pid_t process_group{0};

  TerminalStatus status{TerminalStatus::Created};
  bool start_called{false};
  bool process_exited{false};
  bool pty_eof{false};
  bool finished{false};
  bool timed_out{false};
  bool closing{false};
  bool closed{false};
  bool close_escalated{false};
  bool write_busy{false};
  std::int32_t exit_code{0};
  int exit_signal{0};
  std::optional<int> close_final_signal;
  std::optional<CloseResult> close_result;

  std::uint16_t columns{80};
  std::uint16_t rows{24};
  std::uint8_t erase_character{0x7f};
  std::size_t max_buffer_bytes{4 * 1024 * 1024};
  std::size_t buffered_bytes{0};
  std::uint64_t produced_bytes{0};
  std::deque<Chunk> chunks;

  std::shared_ptr<asio::steady_timer> timeout_timer;
  std::shared_ptr<asio::steady_timer> close_timer;
  std::vector<std::shared_ptr<asio::steady_timer>> read_waiters;
  std::vector<std::shared_ptr<asio::steady_timer>> finish_waiters;
  std::vector<std::shared_ptr<asio::steady_timer>> write_waiters;

  ~State() {
    if (budget && buffered_bytes > 0) {
      budget->sub_buffered(buffered_bytes);
    }
  }

  /** Best-effort terminal teardown signal: foreground job, then session leader group. */
  void signal_terminal(int signal) noexcept {
    pid_t foreground = 0;
    if (master &&
        ::ioctl(master->native_handle(), TIOCGPGRP, &foreground) == 0 &&
        foreground > 0) {
      (void)::kill(-foreground, signal);
    }
    if (process_group > 0 && process_group != foreground) {
      (void)::kill(-process_group, signal);
    }
  }

  void notify(std::vector<std::shared_ptr<asio::steady_timer>>& waiters) {
    auto pending = std::move(waiters);
    waiters.clear();
    for (auto& timer : pending) {
      if (timer) timer->cancel();
    }
  }

  [[nodiscard]] ExitResult exit_result() const {
    ExitResult result{
        .status = status,
        .exit_code = std::nullopt,
        .signal = std::nullopt,
        .timed_out = timed_out,
    };
    if (process_exited) {
      if (exit_signal != 0) {
        result.signal = exit_signal;
      } else {
        result.exit_code = exit_code;
      }
    }
    return result;
  }

  [[nodiscard]] std::uint64_t available_from() const noexcept {
    return chunks.empty() ? produced_bytes : chunks.front().offset;
  }

  [[nodiscard]] CloseResult current_close_result() const {
    return CloseResult{
        .exit = exit_result(),
        .escalated = close_escalated,
        .final_signal = close_final_signal,
    };
  }

  void try_finish() {
    if (finished || !process_exited || !pty_eof) return;
    finished = true;
    if (timeout_timer) timeout_timer->cancel();
    if (close_timer) close_timer->cancel();
    notify(read_waiters);
    notify(write_waiters);
    notify(finish_waiters);
  }

  void on_process_exit(int code, int signal) {
    process_exited = true;
    exit_code = static_cast<std::int32_t>(code);
    exit_signal = signal;
    if (timed_out) {
      status = TerminalStatus::TimedOut;
    } else if (exit_signal != 0) {
      status = TerminalStatus::Signaled;
    } else {
      status = TerminalStatus::Exited;
    }
    // Process completion and PTY EOF are separate facts. Readers/expect must
    // observe the former even when descendants still hold the slave open.
    notify(read_waiters);
    try_finish();
  }

  void evict(std::size_t bytes) {
    while (bytes > 0 && !chunks.empty()) {
      Chunk& front = chunks.front();
      const std::size_t take = std::min(bytes, front.data.size());
      if (take == front.data.size()) {
        chunks.pop_front();
      } else {
        front.data.erase(front.data.begin(), front.data.begin() + take);
        front.offset += take;
      }
      bytes -= take;
      buffered_bytes -= take;
      if (budget) budget->sub_buffered(take);
    }
  }

  void append(const std::uint8_t* data, std::size_t size) {
    const std::uint64_t chunk_end = produced_bytes + size;
    if (size > max_buffer_bytes) {
      data += size - max_buffer_bytes;
      size = max_buffer_bytes;
    }
    const std::uint64_t chunk_offset = chunk_end - size;
    produced_bytes = chunk_end;

    if (buffered_bytes + size > max_buffer_bytes) {
      evict(buffered_bytes + size - max_buffer_bytes);
    }
    const std::size_t global_room = budget ? budget->global_buffer_room() : size;
    const std::size_t take = std::min(size, global_room);
    if (take > 0) {
      const std::size_t skipped = size - take;
      chunks.push_back(Chunk{
          .offset = chunk_offset + skipped,
          .data = std::vector<std::uint8_t>(data + skipped, data + size),
      });
      buffered_bytes += take;
      if (budget) budget->add_buffered(take);
    }
    notify(read_waiters);
  }

  [[nodiscard]] ReadResult collect(ReadOptions options) const {
    ReadResult result{
        .exit = exit_result(),
        .data = {},
        .next_offset = options.offset,
        .available_from = available_from(),
        .dropped_bytes = 0,
        .dropped = false,
        .eof = false,
    };
    std::uint64_t cursor = options.offset;
    if (cursor < result.available_from) {
      result.dropped = true;
      result.dropped_bytes += result.available_from - cursor;
      cursor = result.available_from;
    }

    bool reached_limit = false;
    result.data.reserve(std::min(options.max_bytes, buffered_bytes));
    for (const Chunk& chunk : chunks) {
      const std::uint64_t end = chunk.offset + chunk.data.size();
      if (cursor >= end) continue;
      if (cursor < chunk.offset) {
        if (!result.data.empty()) break;
        result.dropped = true;
        result.dropped_bytes += chunk.offset - cursor;
        cursor = chunk.offset;
      }
      const std::size_t begin = static_cast<std::size_t>(cursor - chunk.offset);
      const std::size_t room = options.max_bytes - result.data.size();
      const std::size_t take = std::min(room, chunk.data.size() - begin);
      result.data.insert(
          result.data.end(), chunk.data.begin() + begin, chunk.data.begin() + begin + take);
      cursor += take;
      if (result.data.size() == options.max_bytes) {
        reached_limit = true;
        break;
      }
    }
    if (!reached_limit && result.data.empty() && cursor < produced_bytes) {
      result.dropped = true;
      result.dropped_bytes += produced_bytes - cursor;
      cursor = produced_bytes;
    }
    result.next_offset = cursor;
    result.eof = finished && cursor >= produced_bytes;
    return result;
  }

  void cancel_master() noexcept {
    if (!master) return;
    boost::system::error_code ignored;
    master->cancel(ignored);
    master->close(ignored);
    pty_eof = true;
    notify(read_waiters);
    notify(write_waiters);
    try_finish();
  }

  void begin_dispose() noexcept {
    if (closed) return;
    closing = true;
    signal_terminal(SIGKILL);
    if (timeout_timer) timeout_timer->cancel();
    if (close_timer) close_timer->cancel();
    cancel_master();
  }

  void finalize_close() {
    cancel_master();
    if (budget && buffered_bytes > 0) {
      budget->sub_buffered(buffered_bytes);
    }
    chunks.clear();
    buffered_bytes = 0;
    child.reset();
    slot.reset();
    closed = true;
    notify(read_waiters);
    notify(write_waiters);
    notify(finish_waiters);
  }
};

Terminal::Terminal(
    asio::any_io_executor executor,
    std::shared_ptr<process::ProcessBudget> budget,
    std::vector<std::string> argv,
    StartOptions options)
    : argv_(std::move(argv)),
      options_(std::move(options)),
      state_(std::make_shared<State>()) {
  state_->executor = std::move(executor);
  state_->budget = std::move(budget);
  state_->columns = options_.columns;
  state_->rows = options_.rows;
  state_->max_buffer_bytes = options_.max_buffer_bytes;
}

Terminal::~Terminal() {
  dispose();
}

asio::awaitable<VoidResult> Terminal::start() {
  assert(state_);
  assert(!state_->start_called);
  assert(!argv_.empty() && !argv_[0].empty());
  assert(state_->budget);

  state_->status = TerminalStatus::Starting;
  auto acquired = state_->budget->try_acquire_process();
  if (!acquired) {
    state_->status = TerminalStatus::Created;
    co_return std::unexpected(std::move(acquired.error()));
  }
  state_->slot = process::ProcessSlot{state_->budget};

  auto fail = [state = state_](std::string message) -> VoidResult {
    state->signal_terminal(SIGKILL);
    state->cancel_master();
    state->child.reset();
    state->process_group = 0;
    state->start_called = false;
    state->slot.reset();
    state->status = TerminalStatus::Created;
    return std::unexpected(Error{std::move(message)});
  };

  try {
    UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
    if (master.get() < 0) {
      co_return fail(std::format("Terminal.start: posix_openpt: {}", strerror(errno)));
    }
    if (::grantpt(master.get()) != 0 || ::unlockpt(master.get()) != 0) {
      co_return fail(std::format("Terminal.start: prepare PTY slave: {}", strerror(errno)));
    }
    std::array<char, 128> slave_name{};
    const int name_error = ::ptsname_r(master.get(), slave_name.data(), slave_name.size());
    if (name_error != 0) {
      co_return fail(std::format("Terminal.start: ptsname_r: {}", strerror(name_error)));
    }
    UniqueFd slave{::open(slave_name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
    if (slave.get() < 0) {
      co_return fail(std::format("Terminal.start: open PTY slave: {}", strerror(errno)));
    }

    const struct winsize size {
      .ws_row = options_.rows,
      .ws_col = options_.columns,
      .ws_xpixel = 0,
      .ws_ypixel = 0,
    };
    if (::ioctl(slave.get(), TIOCSWINSZ, &size) != 0) {
      co_return fail(std::format("Terminal.start: TIOCSWINSZ: {}", strerror(errno)));
    }
    struct termios attributes {};
    if (::tcgetattr(slave.get(), &attributes) != 0) {
      co_return fail(std::format("Terminal.start: tcgetattr: {}", strerror(errno)));
    }
    state_->erase_character = attributes.c_cc[VERASE];

    state_->master = std::make_shared<asio::posix::stream_descriptor>(
        state_->executor, master.release());

    std::vector<std::string> args;
    args.reserve(argv_.size() - 1);
    for (std::size_t index = 1; index < argv_.size(); ++index) {
      args.push_back(argv_[index]);
    }
    const std::string& executable = argv_[0];
    std::unordered_map<bp::environment::key, bp::environment::value>
        child_environment;
    for (const auto& entry : bp::environment::current()) {
      child_environment.emplace(entry.key(), entry.value());
    }
    for (const EnvironmentVariable& variable : options_.environment) {
      child_environment.insert_or_assign(
          bp::environment::key{variable.name},
          bp::environment::value{variable.value});
    }
    bp::process_environment environment{child_environment};
    bp::process child = [&]() {
      if (!options_.cwd.empty()) {
        return bp::process(
            state_->executor,
            executable,
            args,
            bp::process_start_dir(options_.cwd),
            std::move(environment),
            PtyChildSetup{slave.get()});
      }
      return bp::process(
          state_->executor,
          executable,
          args,
          std::move(environment),
          PtyChildSetup{slave.get()});
    }();
    slave.reset();

    state_->process_group = static_cast<pid_t>(child.id());
    state_->child = std::make_shared<bp::process>(std::move(child));
    state_->start_called = true;
    state_->status = TerminalStatus::Running;

    if (options_.timeout.count() > 0) {
      auto state = state_;
      state->timeout_timer = std::make_shared<asio::steady_timer>(state->executor);
      state->timeout_timer->expires_after(options_.timeout);
      state->timeout_timer->async_wait([state](const boost::system::error_code& ec) {
        if (ec || state->finished || state->process_exited) return;
        state->timed_out = true;
        state->signal_terminal(SIGKILL);
        // A background process in another job-control group may still hold the
        // slave. Closing the master makes timeout completion bounded.
        state->cancel_master();
      });
    }

    auto state = state_;
    asio::co_spawn(
        state->executor,
        [state]() -> asio::awaitable<void> {
          auto master = state->master;
          std::array<std::uint8_t, 8192> buffer{};
          for (;;) {
            boost::system::error_code ec;
            const std::size_t read = co_await master->async_read_some(
                asio::buffer(buffer), asio::redirect_error(ec));
            if (ec || read == 0) {
              state->pty_eof = true;
              state->notify(state->read_waiters);
              state->notify(state->write_waiters);
              state->try_finish();
              co_return;
            }
            state->append(buffer.data(), read);
          }
        },
        asio::detached);

    asio::co_spawn(
        state->executor,
        [state]() -> asio::awaitable<void> {
          auto child = state->child;
          auto [ec, code] = co_await child->async_wait(asio::as_tuple);
          (void)ec;
          const int native_status = child->native_exit_code();
          const int signal = WIFSIGNALED(native_status) ? WTERMSIG(native_status) : 0;
          state->on_process_exit(code, signal);
        },
        asio::detached);

    co_return success();
  } catch (const boost::system::system_error& error) {
    co_return fail(std::format("Terminal.start: {}", error.what()));
  } catch (const std::exception& error) {
    co_return fail(std::format("Terminal.start: {}", error.what()));
  }
}

asio::awaitable<Result<std::size_t>> Terminal::write(
    std::vector<std::uint8_t> data,
    std::stop_token stop) {
  assert(state_ && state_->start_called && !state_->closing);

  auto cancel = std::make_shared<OperationCancel>();
  auto on_stop = cancel_on_stop(stop, state_->executor, cancel);
  while (state_->write_busy) {
    auto waiter = std::make_shared<asio::steady_timer>(state_->executor);
    waiter->expires_at(asio::steady_timer::time_point::max());
    state_->write_waiters.push_back(waiter);
    co_await waiter->async_wait(
        asio::bind_cancellation_slot(cancel->signal.slot(), asio::as_tuple));
    auto& waiters = state_->write_waiters;
    waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter), waiters.end());
    if (stop.stop_requested()) {
      cancel->active = false;
      co_return std::unexpected(
          Error{"Terminal.write: cancelled", "cancel", ECANCELED});
    }
  }
  if (state_->finished || state_->pty_eof) {
    co_return std::unexpected(Error{"Terminal.write: terminal is not writable"});
  }

  state_->write_busy = true;
  struct WriteGuard {
    State& state;
    ~WriteGuard() {
      state.write_busy = false;
      state.notify(state.write_waiters);
    }
  } guard{*state_};

  auto master = state_->master;
  auto [ec, written] = co_await asio::async_write(
      *master,
      asio::buffer(data),
      asio::bind_cancellation_slot(cancel->signal.slot(), asio::as_tuple));
  cancel->active = false;
  if (ec) {
    co_return std::unexpected(Error{
        std::format("Terminal.write: {}", ec.message()),
        "write",
        system_code_of(ec)});
  }
  co_return written;
}

asio::awaitable<ReadResult> Terminal::read(
    ReadOptions options,
    std::stop_token stop) {
  assert(state_ && state_->start_called && !state_->closing);
  ReadResult result = state_->collect(options);
  if (!result.data.empty() || result.dropped || result.eof || options.wait.count() == 0) {
    co_return result;
  }

  auto waiter = std::make_shared<asio::steady_timer>(state_->executor);
  waiter->expires_after(options.wait);
  state_->read_waiters.push_back(waiter);
  if (state_->produced_bytes > options.offset || state_->finished) waiter->cancel();
  auto cancel = std::make_shared<OperationCancel>();
  auto on_stop = cancel_on_stop(stop, state_->executor, cancel);
  co_await waiter->async_wait(
      asio::bind_cancellation_slot(cancel->signal.slot(), asio::as_tuple));
  cancel->active = false;
  auto& waiters = state_->read_waiters;
  waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter), waiters.end());
  co_return state_->collect(options);
}

VoidResult Terminal::resize(std::uint16_t columns, std::uint16_t rows) {
  assert(state_ && state_->start_called && !state_->closing);
  const struct winsize size {
    .ws_row = rows,
    .ws_col = columns,
    .ws_xpixel = 0,
    .ws_ypixel = 0,
  };
  if (::ioctl(state_->master->native_handle(), TIOCSWINSZ, &size) != 0) {
    return std::unexpected(Error{
        std::format("Terminal.resize: {}", strerror(errno)), "ioctl", errno});
  }
  state_->columns = columns;
  state_->rows = rows;
  return success();
}

VoidResult Terminal::signal(int signo) {
  assert(state_ && state_->start_called && !state_->closing);
  if (state_->pty_eof) return success();
  pid_t foreground = 0;
  if (::ioctl(state_->master->native_handle(), TIOCGPGRP, &foreground) != 0) {
    return std::unexpected(Error{
        std::format("Terminal.signal: TIOCGPGRP: {}", strerror(errno)),
        "ioctl",
        errno});
  }
  if (foreground <= 0) {
    return std::unexpected(Error{
        "Terminal.signal: PTY has no foreground process group", "ioctl", EIO});
  }
  if (::kill(-foreground, signo) != 0 && errno != ESRCH) {
    return std::unexpected(Error{
        std::format("Terminal.signal: {}", strerror(errno)), "kill", errno});
  }
  return success();
}

TerminalSnapshot Terminal::snapshot() const {
  assert(state_ && state_->start_called && !state_->closing);
  struct termios attributes {};
  if (::tcgetattr(state_->master->native_handle(), &attributes) == 0) {
    state_->erase_character = attributes.c_cc[VERASE];
  }
  return TerminalSnapshot{
      .exit = state_->exit_result(),
      .columns = state_->columns,
      .rows = state_->rows,
      .erase_character = state_->erase_character,
      .next_offset = state_->produced_bytes,
      .available_from = state_->available_from(),
      .buffered_bytes = state_->buffered_bytes,
  };
}

asio::awaitable<ExitWaitResult> Terminal::wait_for_exit(
    std::optional<std::chrono::milliseconds> timeout,
    std::stop_token stop) {
  assert(state_ && state_->start_called && !state_->closing);
  if (!state_->finished) {
    auto waiter = std::make_shared<asio::steady_timer>(state_->executor);
    if (timeout) {
      waiter->expires_after(*timeout);
    } else {
      waiter->expires_at(asio::steady_timer::time_point::max());
    }
    state_->finish_waiters.push_back(waiter);
    if (state_->finished) waiter->cancel();
    auto cancel = std::make_shared<OperationCancel>();
    auto on_stop = cancel_on_stop(stop, state_->executor, cancel);
    co_await waiter->async_wait(
        asio::bind_cancellation_slot(cancel->signal.slot(), asio::as_tuple));
    cancel->active = false;
    auto& waiters = state_->finish_waiters;
    waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter), waiters.end());
  }
  co_return ExitWaitResult{
      .exit = state_->exit_result(),
      .completed = state_->finished,
  };
}

asio::awaitable<CloseResult> Terminal::async_close(std::chrono::milliseconds grace) {
  auto state = state_;
  if (state->closed) co_return *state->close_result;

  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  if (!state->start_called) {
    state->close_result = state->current_close_result();
    state->finalize_close();
    co_return *state->close_result;
  }
  if (!state->closing) {
    state->closing = true;
    if (!state->finished) {
      state->signal_terminal(SIGHUP);
      state->close_timer = std::make_shared<asio::steady_timer>(state->executor);
      state->close_timer->expires_after(grace);
      state->close_timer->async_wait([state](const boost::system::error_code& ec) {
        if (!ec && !state->finished) {
          state->close_escalated = true;
          state->close_final_signal = SIGKILL;
          state->signal_terminal(SIGKILL);
          // Do not let unrelated surviving job-control groups keep close
          // waiting forever by retaining the PTY slave.
          state->cancel_master();
        }
      });
    }
  }

  while (!state->finished) {
    auto waiter = std::make_shared<asio::steady_timer>(state->executor);
    waiter->expires_at(asio::steady_timer::time_point::max());
    state->finish_waiters.push_back(waiter);
    if (state->finished) waiter->cancel();
    co_await waiter->async_wait(asio::as_tuple);
  }
  if (!state->close_result) {
    state->close_result = state->current_close_result();
    state->finalize_close();
  }
  co_return *state->close_result;
}

void Terminal::dispose() noexcept {
  auto state = state_;
  if (!state) return;
  try {
    asio::post(state->executor, [state]() noexcept { state->begin_dispose(); });
  } catch (...) {
    state->signal_terminal(SIGKILL);
  }
}

}  // namespace vacps::terminal
