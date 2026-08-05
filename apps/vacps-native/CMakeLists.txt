cmake_minimum_required(VERSION 3.24)
project(vacps-native VERSION 0.1.8 LANGUAGES C CXX
  DESCRIPTION "Asio + QuickJS native agent runtime")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VACPS_STATIC "Static link" ON)
option(VACPS_ENABLE_ASAN "AddressSanitizer (incompatible with VACPS_STATIC)" OFF)
option(VACPS_ENABLE_UBSAN "UndefinedBehaviorSanitizer (incompatible with VACPS_STATIC)" OFF)
option(VACPS_ENABLE_TSAN "ThreadSanitizer (incompatible with VACPS_STATIC / ASan)" OFF)
# Optional debug counters in Runtime diagnostics. Zero cost when OFF.

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Wformat -Wformat-security)
endif()

if(VACPS_STATIC)
  add_link_options(-fuse-ld=lld)
  add_link_options(-static)
  add_link_options(-Wl,--gc-sections)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

if(VACPS_ENABLE_TSAN AND (VACPS_ENABLE_ASAN OR VACPS_ENABLE_UBSAN))
  message(FATAL_ERROR "TSan cannot be combined with ASan/UBSan")
endif()

if(VACPS_ENABLE_ASAN)
  if(VACPS_STATIC)
    message(FATAL_ERROR "ASan is incompatible with VACPS_STATIC=ON")
  endif()
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()

if(VACPS_ENABLE_UBSAN)
  if(VACPS_STATIC)
    message(FATAL_ERROR "UBSan is incompatible with VACPS_STATIC=ON")
  endif()
  # Non-recovering UBSan: CI/tests must fail hard on the first UB report.
  add_compile_options(
    -fsanitize=undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
    -g
  )
  add_link_options(-fsanitize=undefined)
endif()

if(VACPS_ENABLE_TSAN)
  if(VACPS_STATIC)
    message(FATAL_ERROR "TSan is incompatible with VACPS_STATIC=ON")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(VacpsDeps)

# ── base (log) ─────────────────────────────────────────────────────
vacps_layer(vacps_base
  src/app/log.cpp
  src/app/version.cpp
)
target_link_libraries(vacps_base PUBLIC
  Threads::Threads
  spdlog::spdlog
)

# ── URL domain (Ada) ───────────────────────────────────────────────
vacps_layer(vacps_url
  src/url/url.cpp
  src/url/search_params.cpp
)
target_link_libraries(vacps_url PUBLIC ada::ada)

# ── Text domain (simdutf) ──────────────────────────────────────────
vacps_layer(vacps_text
  src/text/decoder.cpp
)
target_link_libraries(vacps_text PUBLIC simdutf::simdutf)

# ── Crypto domain (OpenSSL) ────────────────────────────────────────
vacps_layer(vacps_crypto
  src/crypto/crypto.cpp
)
target_link_libraries(vacps_crypto PUBLIC
  vacps_base
  OpenSSL::Crypto
)

# ── HTTP domain (Beast + Ada URL + OpenSSL TLS) ─────────────────────
# Pure C++ outbound client + inbound HTTP/1 server transport.
# JS surface via vacps:http module (request + class Server).
vacps_layer(vacps_http
  src/http/client.cpp
  src/http/server.cpp
)
target_link_libraries(vacps_http PUBLIC
  vacps_base
  ada::ada
  OpenSSL::SSL
)

# ── Storage domain (SQLite; JS surface via vacps:store module) ──
vacps_layer(vacps_storage
  src/storage/database.cpp
  src/storage/store.cpp
)
target_link_libraries(vacps_storage PUBLIC
  vacps_base
  vacps_sqlite
)

# ── FS domain (dual backend: Asio random_access_file/io_uring + POSIX) ──
# BOOST_ASIO_HAS_IO_URING is defined PUBLIC on every vacps_layer (see VacpsDeps
# vacps_layer helper) so all Asio TUs share one config and avoid ODR/ABI skew.
# Epoll remains the default reactor: we never define BOOST_ASIO_DISABLE_EPOLL
# or BOOST_ASIO_HAS_IO_URING_AS_DEFAULT. io_uring backs file ops only when the
# runtime probe succeeds; networking/timers stay on epoll.
find_library(VACPS_URING_LIB uring)
if(NOT VACPS_URING_LIB)
  message(FATAL_ERROR "liburing not found (required for vacps:fs Asio/io_uring backend)")
endif()
vacps_layer(vacps_fs
  src/fs/fs.cpp
  src/fs/file.cpp
  src/fs/open_options.cpp
  src/fs/io_uring_probe.cpp
)
# Probe implementation only.
target_compile_definitions(vacps_fs PRIVATE VACPS_HAVE_LIBURING=1)
target_link_libraries(vacps_fs PUBLIC
  vacps_base
  ${VACPS_URING_LIB}
)

# ── Process domain (Boost.Process v2; owner main_executor only) ──────────
# Process v2 compiled symbols come from VACPS_BOOST_PROCESS_TARGET
# (vacps_boost_process for FetchContent, or Boost::process for system Boost).
vacps_layer(vacps_process
  src/process/budget.cpp
  src/process/process.cpp
  src/process/runtime.cpp
)
if(NOT VACPS_BOOST_PROCESS_TARGET)
  message(FATAL_ERROR
    "vacps_process requires VACPS_BOOST_PROCESS_TARGET "
    "(set in cmake/VacpsDeps.cmake)")
endif()
target_link_libraries(vacps_process PUBLIC
  vacps_base
  ${VACPS_BOOST_PROCESS_TARGET}
)
# Custom vacps_boost_process keeps Process macros PRIVATE; apply consumer ABI
# config only on the Process domain TUs that include Boost.Process headers.
# System Boost::process brings its own interface definitions — do not override.
if(VACPS_BOOST_PROCESS_TARGET STREQUAL "vacps_boost_process")
  target_compile_definitions(vacps_process PRIVATE
    BOOST_PROCESS_USE_STD_FS=1
    BOOST_PROCESS_STATIC_LINK=1
  )
endif()

# ── Global APIs: URL / TextEncoder / TextDecoder ───────────────────
# Headers use Runtime / binding types; link is resolved by vacps_runtime
# (avoids static-lib cycle: runtime → globals → runtime).
vacps_layer(vacps_globals
  src/globals/install.cpp
  src/globals/url_binding.cpp
  src/globals/url_search_params_binding.cpp
  src/globals/text_encoder_binding.cpp
  src/globals/text_decoder_binding.cpp
)
target_link_libraries(vacps_globals PUBLIC
  vacps_base
  vacps_quickjs
  vacps_url
  vacps_text
)

# ── binding DSL (header-only QuickJS C++23 helpers) ────────────────
# No implementation sources — consumers include binding/*.hpp only.
add_library(vacps_binding INTERFACE)
target_include_directories(vacps_binding INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(vacps_binding INTERFACE cxx_std_23)
target_link_libraries(vacps_binding INTERFACE
  vacps_base
  vacps_quickjs
)

# ── vacps:* ES modules (binding DSL + domain libs) ─────────────────
# Same cycle pattern as vacps_globals: templates need Runtime headers;
# symbols resolve when linked into vacps_runtime.
vacps_layer(vacps_modules
  src/modules/catalog.cpp
  src/modules/crypto.cpp
  src/modules/host.cpp
  src/modules/log.cpp
  src/modules/timer.cpp
  src/modules/store.cpp
  src/modules/fs.cpp
  src/modules/http.cpp
  src/modules/process.cpp
)
target_link_libraries(vacps_modules PUBLIC
  vacps_base
  vacps_quickjs
  vacps_crypto
  vacps_storage
  vacps_fs
  vacps_http
  vacps_process
  vacps_binding
)

# ── Asio + QuickJS runtime core (docs/RUNTIME_LAYERING.md) ──────────
vacps_layer(vacps_runtime
  src/runtime/js_engine.cpp
  src/runtime/js_error.cpp
  src/runtime/promise_capability.cpp
  src/runtime/js_promise_await.cpp
  src/runtime/detail/runtime_impl.cpp
  src/runtime/runtime.cpp
  src/runtime/runtime_async.cpp
  src/runtime/runtime_script.cpp
  src/runtime/runtime_callbacks.cpp
)
target_link_libraries(vacps_runtime PUBLIC
  vacps_base
  vacps_quickjs
  vacps_globals
  vacps_modules
)
target_link_libraries(vacps_runtime PRIVATE
  mimalloc-static
)

# ── Bootstrap (process initialization only; no domain policy) ──────
vacps_layer(vacps_bootstrap
  src/bootstrap/process_init.cpp
)
target_link_libraries(vacps_bootstrap PUBLIC
  vacps_base
)

# ── Host composition (CLI + process entry support; no domain policy) ─
vacps_layer(vacps_host
  src/host/command_line.cpp
  src/host/entry_module.cpp
  src/host/application.cpp
)
target_link_libraries(vacps_host PUBLIC
  vacps_runtime
  vacps_process
)

# ── Agent binary ───────────────────────────────────────────────────
add_executable(vacps-agent src/main.cpp)
# Keep the global new/delete override in the final executable's object list;
# placing it only in a static archive allows the linker to discard the TU.
# TSan supplies strong new/delete interceptors that must own these symbols.
if(NOT VACPS_ENABLE_TSAN)
  target_sources(vacps-agent PRIVATE src/bootstrap/mimalloc_new_delete.cpp)
endif()
set_target_properties(vacps-agent PROPERTIES
  OUTPUT_NAME "vacps-agent-linux-x86_64"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(vacps-agent PRIVATE
  vacps_host
  vacps_bootstrap
)
if(NOT VACPS_ENABLE_TSAN)
  target_link_libraries(vacps-agent PRIVATE mimalloc-static)
endif()

include(GNUInstallDirs)
install(TARGETS vacps-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "vacps-native ${PROJECT_VERSION} — Asio + QuickJS runtime")
message(STATUS "  layers: vacps_base vacps_bootstrap vacps_url vacps_text vacps_crypto vacps_http vacps_storage vacps_fs vacps_process vacps_globals vacps_modules vacps_runtime vacps_host")
message(STATUS "  binary: vacps-agent → vacps-agent-linux-x86_64")
