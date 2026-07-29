// Minimal scaffold: version + self-check. HTTP/Asio land in later stages.
#include "version.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void print_usage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " [--version] [--help]\n"
            << "  VACPS native agent scaffold (x86_64 musl static).\n"
            << "  Full HTTP / task runtime is not wired yet.\n";
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] ? argv[i] : "";
    if (arg == "--version" || arg == "-V") {
      std::cout << "vacps-agent-linux-x86_64 " << vacps::version() << '\n';
      return EXIT_SUCCESS;
    }
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  // Default: print banner (useful for smoke tests).
  std::cout << "vacps-agent-linux-x86_64 " << vacps::version()
            << " (scaffold; no HTTP listener yet)\n";
  return EXIT_SUCCESS;
}
