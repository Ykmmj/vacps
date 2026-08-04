#include "bootstrap/process_init.hpp"

#include <cerrno>
#include <csignal>

namespace vacps::bootstrap {

VoidResult initialize_process() noexcept {
  struct ::sigaction sa {};
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  if (::sigemptyset(&sa.sa_mask) != 0) {
    return std::unexpected(Error{
        "initialize_process: sigemptyset failed",
        "sigemptyset",
        errno});
  }
  if (::sigaction(SIGPIPE, &sa, nullptr) != 0) {
    return std::unexpected(Error{
        "initialize_process: sigaction(SIGPIPE, SIG_IGN) failed",
        "sigaction",
        errno});
  }
  return success();
}

}  // namespace vacps::bootstrap
