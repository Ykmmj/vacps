#include "app/log.hpp"
#include "app/version.hpp"
#include "bootstrap/process_init.hpp"
#include "host/application.hpp"
#include "host/command_line.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/** Filesystem probe when options.script_path is still empty. */
[[nodiscard]] std::string resolve_default_script() {
  namespace fs = std::filesystem;
  for (const char* c :
       {"script/dist/vacps.mjs", "apps/vacps-native/script/dist/vacps.mjs"}) {
    std::error_code ec;
    if (fs::is_regular_file(c, ec)) {
      return c;
    }
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  // Adapt argv without owning the underlying strings (views valid for main).
  std::vector<std::string_view> args;
  args.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 1);
  if (argc <= 0 || argv == nullptr || argv[0] == nullptr) {
    args.emplace_back("vacps-agent");
  } else {
    for (int i = 0; i < argc; ++i) {
      args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
  }

  auto parsed = vacps::host::parse_command_line(args);
  if (!parsed) {
    std::cerr << "command line error: " << parsed.error().message << '\n';
    if (!args.empty()) {
      std::cerr << vacps::host::format_usage(args.front());
    }
    return EXIT_FAILURE;
  }

  // Help/version: no process init, no logging backend, no runtime.
  if (parsed->action == vacps::host::CommandLineAction::help) {
    std::cout << vacps::host::format_usage(args.front());
    return EXIT_SUCCESS;
  }
  if (parsed->action == vacps::host::CommandLineAction::version) {
    std::cout << "vacps-agent-linux-x86_64 " << vacps::version() << '\n';
    return EXIT_SUCCESS;
  }

  auto options = std::move(parsed->options);
  if (options.script_path.empty()) {
    options.script_path = resolve_default_script();
  }
  if (options.script_path.empty()) {
    std::cerr
        << "business script required: pass --script PATH "
           "(or provide script/dist/vacps.mjs)\n";
    std::cerr << vacps::host::format_usage(args.front());
    return EXIT_FAILURE;
  }

  if (auto init = vacps::bootstrap::initialize_process(); !init) {
    std::cerr << "process initialization failed: " << init.error().message
              << '\n';
    return EXIT_FAILURE;
  }

  vacps::log::init(options.log_level);
  vacps::log::info("vacps-agent {} starting", vacps::version());

  try {
    vacps::host::Application application(std::move(options));
    if (auto init = application.initialize(); !init) {
      vacps::log::error(
          "application initialize failed: {}", init.error().message);
      vacps::log::flush();
      return EXIT_FAILURE;
    }
    const int rc = application.run();
    if (rc != 0) {
      vacps::log::error("application run failed (rc={})", rc);
      vacps::log::flush();
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    vacps::log::error("fatal: {}", e.what());
    vacps::log::flush();
    return EXIT_FAILURE;
  }
}
