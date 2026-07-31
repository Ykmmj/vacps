#include "app/log.hpp"
#include "app/version.hpp"
#include "bootstrap/config.hpp"
#include "runtime/application_runtime.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

void print_usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "  --version, -V     Print version and exit\n"
      << "  --help, -h        Show help\n"
      << "  --data-dir DIR    Data directory (default: VACPS_DATA_DIR or \"data\")\n"
      << "  --script PATH     Business ESM bundle (required)\n"
      << "  --log-level LVL   log level\n"
      << "\n"
      << "C++: Asio event loop + vacps:* modules.\n"
      << "Agent policy (listen, auth, CP keys, …) is owned by the business script.\n";
}

/** Filesystem probe when neither CLI nor VACPS_SCRIPT set a path. */
std::string resolve_default_script() {
  namespace fs = std::filesystem;
  for (const char* c : {"script/dist/vacps.mjs", "apps/vacps-native/script/dist/vacps.mjs"}) {
    std::error_code ec;
    if (fs::is_regular_file(c, ec)) return c;
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  // Sole bulk env read (n1 §十 / RUNTIME_LAYERING). CLI overrides apply after.
  auto boot = vacps::bootstrap::BootstrapConfig::fromEnvironment();

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] != nullptr ? argv[i] : "";
    if (arg == "--version" || arg == "-V") {
      std::cout << "vacps-agent-linux-x86_64 " << vacps::version() << '\n';
      return EXIT_SUCCESS;
    }
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    }
    if (arg == "--data-dir" && i + 1 < argc) {
      boot.data_dir = argv[++i];
      continue;
    }
    if (arg == "--script" && i + 1 < argc) {
      boot.script_path = argv[++i];
      continue;
    }
    if (arg == "--log-level" && i + 1 < argc) {
      boot.log_level = argv[++i];
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  vacps::log::init(boot.log_level);
  vacps::log::info("vacps-agent {} starting", vacps::version());

  std::string script_path = boot.script_path;
  if (script_path.empty()) {
    script_path = resolve_default_script();
  }
  if (script_path.empty()) {
    vacps::log::error("business script required (VACPS_SCRIPT or script/dist/vacps.mjs)");
    vacps::log::flush();
    return EXIT_FAILURE;
  }

  try {
    auto opts = vacps::runtime::ApplicationRuntimeOptions::from_bootstrap(boot);
    vacps::runtime::ApplicationRuntime app(std::move(opts));
    if (auto e = app.start(script_path); !e) {
      vacps::log::error("quickjs host failed: {}", e.error().message);
      vacps::log::flush();
      return EXIT_FAILURE;
    }
    return app.run();
  } catch (const std::exception& e) {
    vacps::log::error("fatal: {}", e.what());
    vacps::log::flush();
    return EXIT_FAILURE;
  }
}
