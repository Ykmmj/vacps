#include "process/process.hpp"

// Boost.Process v2 + Asio (Context7: /websites/boost_doc_libs_libs_process, /boostorg/asio)
// - process_stdio + readable_pipe for capture
// - async_execute with cancel_after(terminal) for timeout → terminate
// - new process group (setpgid) so timeout kills the full tree (design §19.2)

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/process.hpp>
#include <boost/system/error_code.hpp>

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

asio::awaitable<void> async_read_all(asio::readable_pipe& pipe, std::string& out) {
  boost::system::error_code ec;
  co_await asio::async_read(
      pipe,
      asio::dynamic_buffer(out),
      asio::redirect_error(asio::use_awaitable, ec));
  // Process docs: eof is normal when the child closes the pipe.
  (void)ec;
  co_return;
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

    // Concurrent pipe drain + async_execute (Process v2 quickstart style).
    auto wait_child = [proc = std::move(proc), result, timeout_ms, pgid]() mutable
        -> asio::awaitable<void> {
      if (timeout_ms > 0) {
        // Official pattern:
        //   async_execute(proc)
        //     (cancel_after(dur, cancellation_type::terminal))
        //     (token);
        // terminal → terminate leader; then kill process group for descendants.
        auto [ec, code] = co_await (
            bp::async_execute(std::move(proc))(
                asio::cancel_after(
                    std::chrono::milliseconds(timeout_ms),
                    asio::cancellation_type::terminal))(
                asio::as_tuple(asio::use_awaitable)));
        result->exit_code = code;
        // evaluate_exit_code: WIFSIGNALED → WTERMSIG (e.g. SIGKILL == 9), not 128+sig.
        // cancel_after may also complete with operation_aborted.
        if (ec == asio::error::operation_aborted ||
            code == SIGKILL || code == SIGTERM || code == SIGINT) {
          result->timed_out = true;
          kill_process_group(pgid);
        }
      } else {
        auto [ec, code] = co_await bp::async_execute(
            std::move(proc), asio::as_tuple(asio::use_awaitable));
        (void)ec;
        result->exit_code = code;
      }
      co_return;
    };

    co_await (
        async_read_all(*out_pipe, result->stdout_str) &&
        async_read_all(*err_pipe, result->stderr_str) && wait_child());

    co_return *result;
  } catch (const boost::system::system_error& e) {
    co_return std::unexpected(Error{std::format("process.run: {}", e.what())});
  } catch (const std::exception& e) {
    co_return std::unexpected(Error{std::format("process.run: {}", e.what())});
  }
}

}  // namespace vacps::process
