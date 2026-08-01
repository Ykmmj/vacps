#include "process/runtime.hpp"

#include <format>

namespace vacps::process {

ProcessRuntime::ProcessRuntime(asio::any_io_executor executor, ProcessLimits limits)
    : executor_(std::move(executor)),
      budget_(std::make_shared<ProcessBudget>(limits)) {}

Result<std::shared_ptr<Process>> ProcessRuntime::create(
    std::vector<std::string> argv,
    StartOptions options) {
  try {
    return std::make_shared<Process>(
        executor_, budget_, std::move(argv), std::move(options));
  } catch (const std::exception& e) {
    return std::unexpected(Error{std::format("process.create: {}", e.what())});
  }
}

}  // namespace vacps::process
