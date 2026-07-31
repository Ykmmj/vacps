#include "process/process.hpp"

#include <utility>

namespace vacps::process {

Process::Process(Registry& registry, std::vector<std::string> argv, StartOptions opts)
    : registry_(&registry),
      argv_(std::move(argv)),
      opts_(std::move(opts)) {}

Process::~Process() {
  if (closed_ || id_.empty() || registry_ == nullptr) {
    return;
  }
  closed_ = true;
  // Best-effort; Registry must still be alive (ScriptServices owns it).
  (void)registry_->close(id_);
}

VoidResult Process::ensure_live(std::string_view op) const {
  if (closed_) {
    return std::unexpected(Error{std::string(op) + ": process is closed"});
  }
  if (id_.empty()) {
    return std::unexpected(Error{std::string(op) + ": process not started"});
  }
  if (registry_ == nullptr) {
    return std::unexpected(Error{std::string(op) + ": registry unavailable"});
  }
  return {};
}

asio::awaitable<VoidResult> Process::start() {
  if (closed_) {
    co_return std::unexpected(Error{"Process.start: process is closed"});
  }
  if (start_called_) {
    co_return std::unexpected(Error{"Process.start: already started"});
  }
  if (registry_ == nullptr) {
    co_return std::unexpected(Error{"Process.start: registry unavailable"});
  }
  if (argv_.empty() || argv_[0].empty()) {
    co_return std::unexpected(Error{"Process.start: argv is empty"});
  }

  start_called_ = true;
  auto result = co_await registry_->start(argv_, opts_);
  if (!result) {
    // Allow retry after failed spawn.
    start_called_ = false;
    co_return std::unexpected(std::move(result.error()));
  }
  id_ = std::move(result->id);
  pid_ = result->pid;
  co_return success();
}

asio::awaitable<Result<std::string>> Process::read(
    std::string_view stream,
    std::int32_t wait_ms,
    std::size_t max_bytes) {
  if (auto live = ensure_live("Process.read"); !live) {
    co_return std::unexpected(std::move(live.error()));
  }
  const bool want_stdout = stream == "stdout";
  if (!want_stdout && stream != "stderr") {
    co_return std::unexpected(
        Error{"Process.read: stream must be \"stdout\" or \"stderr\""});
  }

  ReadOptions opts;
  opts.wait_ms = wait_ms;
  opts.max_bytes = max_bytes > 0 ? max_bytes : 65'536;
  if (want_stdout) {
    opts.stdout_offset = stdout_offset_;
    // Skip stderr budget: park offset at "current end" via large value.
    opts.stderr_offset = static_cast<std::size_t>(-1);
  } else {
    opts.stderr_offset = stderr_offset_;
    opts.stdout_offset = static_cast<std::size_t>(-1);
  }

  auto result = co_await registry_->read(id_, opts);
  if (!result) {
    co_return std::unexpected(std::move(result.error()));
  }

  if (want_stdout) {
    stdout_offset_ = result->next_stdout_offset;
    co_return std::move(result->stdout_slice);
  }
  stderr_offset_ = result->next_stderr_offset;
  co_return std::move(result->stderr_slice);
}

asio::awaitable<Result<std::size_t>> Process::write(
    std::string data,
    WriteOptions opts) {
  if (auto live = ensure_live("Process.write"); !live) {
    co_return std::unexpected(std::move(live.error()));
  }
  co_return co_await registry_->write(id_, std::move(data), opts);
}

asio::awaitable<Result<RunResult>> Process::wait() {
  if (auto live = ensure_live("Process.wait"); !live) {
    co_return std::unexpected(std::move(live.error()));
  }

  for (;;) {
    if (closed_) {
      co_return std::unexpected(Error{"Process.wait: process closed"});
    }
    auto snap = registry_->snapshot(id_);
    if (!snap) {
      co_return std::unexpected(std::move(snap.error()));
    }
    if (snap->status != "running") {
      RunResult r;
      r.exit_code = snap->exit_code;
      r.timed_out = snap->timed_out;
      r.stdout_str = std::move(snap->stdout_slice);
      r.stderr_str = std::move(snap->stderr_slice);
      r.stdout_produced = snap->stdout_produced;
      r.stderr_produced = snap->stderr_produced;
      r.stdout_truncated = snap->stdout_truncated;
      r.stderr_truncated = snap->stderr_truncated;
      co_return r;
    }

    // Wait until status changes: park offsets so wake is status-only.
    ReadOptions opts;
    opts.wait_ms = 60'000;
    opts.max_bytes = 1;
    opts.stdout_offset = static_cast<std::size_t>(-1);
    opts.stderr_offset = static_cast<std::size_t>(-1);
    auto r = co_await registry_->read(id_, opts);
    if (!r) {
      co_return std::unexpected(std::move(r.error()));
    }
  }
}

Result<bool> Process::terminate(std::string_view signal, std::int32_t grace_ms) {
  if (auto live = ensure_live("Process.terminate"); !live) {
    return std::unexpected(std::move(live.error()));
  }
  return registry_->terminate(id_, signal, grace_ms);
}

VoidResult Process::close() {
  if (closed_) {
    return {};
  }
  closed_ = true;
  if (!id_.empty() && registry_ != nullptr) {
    auto result = registry_->close(id_);
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
  }
  return {};
}

std::optional<std::int32_t> Process::pid() const noexcept {
  if (!start_called_ || closed_ || id_.empty() || pid_ <= 0) {
    return std::nullopt;
  }
  return pid_;
}

bool Process::running() const {
  if (!start_called_ || closed_ || id_.empty() || registry_ == nullptr) {
    return false;
  }
  auto snap = registry_->snapshot(id_);
  if (!snap) {
    return false;
  }
  return snap->status == "running";
}

}  // namespace vacps::process
