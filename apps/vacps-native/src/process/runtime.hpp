#pragma once

/**
 * Shared process infrastructure: executor + budget.
 * Does not own or enumerate Process instances.
 */

#include "process/budget.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <memory>
#include <utility>
#include <vector>

#include "app/error.hpp"
#include "process/process.hpp"

namespace vacps::process {

namespace asio = boost::asio;

class ProcessRuntime {
 public:
  explicit ProcessRuntime(
      asio::any_io_executor executor,
      ProcessLimits limits = {});

  ProcessRuntime(const ProcessRuntime&) = delete;
  ProcessRuntime& operator=(const ProcessRuntime&) = delete;

  [[nodiscard]] asio::any_io_executor executor() const noexcept { return executor_; }
  [[nodiscard]] const std::shared_ptr<ProcessBudget>& budget() const noexcept {
    return budget_;
  }

  /**
   * Create an unstarted Process bound to this runtime's executor and budget.
   * Does not spawn; caller uses Process::start().
   */
  [[nodiscard]] Result<std::shared_ptr<Process>> create(
      std::vector<std::string> argv,
      StartOptions options = {});

 private:
  asio::any_io_executor executor_;
  std::shared_ptr<ProcessBudget> budget_;
};

}  // namespace vacps::process
