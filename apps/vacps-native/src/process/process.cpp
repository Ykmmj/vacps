#include "process/process.hpp"

// Boost.Process v2 + Asio (Context7: /websites/boost_doc_libs_libs_process, /boostorg/asio)
// - process_stdio + readable_pipe for capture
// - async_execute with cancel_after(terminal) for timeout → terminate
// - new process group (setpgid) so timeout kills the full tree (design §19.2)
// - hard caps on stdout/stderr accumulation (not unbounded dynamic_buffer)

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/process.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <format>
#include <memory>
#include <utility>

#include <unistd.h>

namespace vacps::process {
namespace bp = boost::process;
namespace asio = boost::asio;
using namespace asio::experimental::awaitable_operators;

namespace {

struct CapReadState {
  std::string retained;
  std::size_t produced{0};
  bool truncated{false};
  std::size_t hard_max{kDefaultRunMaxStdoutBytes};
};

/**
 * Drain pipe until EOF. Retain at most hard_max bytes; keep counting produced.
 */
asio::awaitable<void> async_read_capped(asio::readable_pipe& pipe, CapReadState& st) {
  std::array<char, 8192> buf{};
  for (;;) {
    auto [ec, n] = co_await pipe.async_read_some(
        asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
    if (ec == asio::error::eof || n == 0) {
      co_return;
    }
    if (ec) {
      // Other errors: treat as end of stream for capture purposes.
      co_return;
    }
    st.produced += n;
    if (st.retained.size() < st.hard_max) {
      const auto room = st.hard_max - st.retained.size();
      const auto take = n < room ? n : room;
      st.retained.append(buf.data(), take);
      if (n > room) st.truncated = true;
    } else {
      st.truncated = true;
    }
  }
}

/** Child becomes its own process-group leader (Linux). */
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

void kill_process_group(bp::pid_type pid) {
  if (pid <= 0) return;
  // Best-effort: SIGTERM then SIGKILL so shell grandchildren are reaped.
  ::kill(-static_cast<pid_t>(pid), SIGTERM);
  ::kill(-static_cast<pid_t>(pid), SIGKILL);
}

}  // namespace

asio::awaitable<Result<RunResult>> async_run(
    std::vector<std::string> argv,
    RunOptions opts) {
  if (argv.empty() || argv[0].empty()) {
    co_return std::unexpected(Error{"process.run: argv is empty"});
  }

  try {
    auto executor = co_await asio::this_coro::executor;

    auto out_pipe = std::make_shared<asio::readable_pipe>(executor);
    auto err_pipe = std::make_shared<asio::readable_pipe>(executor);

    std::vector<std::string> args;
    args.reserve(argv.size() > 1 ? argv.size() - 1 : 0);
    for (std::size_t i = 1; i < argv.size(); ++i) {
      args.push_back(std::move(argv[i]));
    }
    const std::string exe = std::move(argv[0]);

    // Docs: process(ctx, path, args, process_stdio{in, out, err}, inits...)
    bp::process proc = [&]() {
      if (!opts.cwd.empty()) {
        return bp::process(
            executor,
            exe,
            args,
            bp::process_stdio{nullptr, *out_pipe, *err_pipe},
            bp::process_start_dir(opts.cwd),
            new_process_group{});
      }
      return bp::process(
          executor,
          exe,
          args,
          bp::process_stdio{nullptr, *out_pipe, *err_pipe},
          new_process_group{});
    }();

    const auto pgid = proc.id();
    auto result = std::make_shared<RunResult>();
    const auto timeout_ms = opts.timeout_ms;

    CapReadState out_st;
    CapReadState err_st;
    out_st.hard_max =
        opts.max_stdout_bytes == 0 ? kDefaultRunMaxStdoutBytes : opts.max_stdout_bytes;
    err_st.hard_max =
        opts.max_stderr_bytes == 0 ? kDefaultRunMaxStderrBytes : opts.max_stderr_bytes;

    // Concurrent pipe drain + async_execute. Timeout is an explicit timer flag
    // (never inferred from exit codes 9/15/2 — those are legitimate process exits).
    auto wait_child = [proc = std::move(proc), result, timeout_ms, pgid, executor]() mutable
        -> asio::awaitable<void> {
      std::shared_ptr<asio::steady_timer> timer;
      if (timeout_ms > 0) {
        timer = std::make_shared<asio::steady_timer>(executor);
        timer->expires_after(std::chrono::milliseconds(timeout_ms));
        timer->async_wait([pgid, result, timer](const boost::system::error_code& ec) {
          if (ec) return;  // cancelled: process exited first
          result->timed_out = true;
          kill_process_group(pgid);
        });
      }

      auto [ec, code] = co_await bp::async_execute(
          std::move(proc), asio::as_tuple(asio::use_awaitable));
      (void)ec;
      if (timer) {
        timer->cancel();
      }
      result->exit_code = code;
      // If the timer already fired, timed_out stays true even when exit is SIGKILL.
      co_return;
    };

    co_await (
        async_read_capped(*out_pipe, out_st) &&
        async_read_capped(*err_pipe, err_st) && wait_child());

    result->stdout_str = std::move(out_st.retained);
    result->stderr_str = std::move(err_st.retained);
    result->stdout_produced = out_st.produced;
    result->stderr_produced = err_st.produced;
    result->stdout_truncated = out_st.truncated;
    result->stderr_truncated = err_st.truncated;

    co_return *result;
  } catch (const boost::system::system_error& e) {
    co_return std::unexpected(Error{std::format("process.run: {}", e.what())});
  } catch (const std::exception& e) {
    co_return std::unexpected(Error{std::format("process.run: {}", e.what())});
  }
}

}  // namespace vacps::process
