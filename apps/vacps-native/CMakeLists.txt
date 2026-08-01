cmake_minimum_required(VERSION 3.24)
project(vacps-native
  VERSION 0.1.7
  LANGUAGES C CXX
  DESCRIPTION "VACPS native agent (musl static ELF)"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VACPS_STATIC "Link the agent as a fully static ELF" ON)
option(VACPS_ENABLE_ASAN "Enable AddressSanitizer (Debug only, dynamic)" OFF)
option(VACPS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(VACPS_BUILD_TESTS "Build GoogleTest unit tests" ON)

# ── Compiler / linker ──────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wformat
    -Wformat-security
  )
endif()

if(VACPS_STATIC)
  add_link_options(-fuse-ld=lld)
  add_link_options(-static)
  add_link_options(-Wl,--gc-sections)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

if(VACPS_ENABLE_ASAN)
  if(VACPS_STATIC)
    message(FATAL_ERROR "ASan is incompatible with VACPS_STATIC=ON")
  endif()
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

if(VACPS_ENABLE_UBSAN)
  if(VACPS_STATIC)
    message(FATAL_ERROR "UBSan is incompatible with VACPS_STATIC=ON")
  endif()
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake")

find_package(Threads REQUIRED)

# OpenSSL (Alpine: openssl-dev + openssl-libs-static for musl full-static)
if(VACPS_STATIC)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL ${OPENSSL_VERSION} (static=${OPENSSL_USE_STATIC_LIBS})")

# ── Boost.Process v2 (compiled; not header-only) ───────────────────
# Sources live under FetchContent Boost tarball: libs/process/src
if(VACPS_BOOST_INCLUDE_DIRS)
  set(_vacps_boost_root "${VACPS_BOOST_INCLUDE_DIRS}")
else()
  # system boost — try to locate process sources is not required if headers only
  set(_vacps_boost_root "")
endif()

if(_vacps_boost_root AND EXISTS "${_vacps_boost_root}/libs/process/src/error.cpp")
  set(_bp_src "${_vacps_boost_root}/libs/process/src")
  add_library(vacps_boost_process STATIC
    "${_bp_src}/error.cpp"
    "${_bp_src}/environment.cpp"
    "${_bp_src}/pid.cpp"
    "${_bp_src}/shell.cpp"
    "${_bp_src}/detail/environment_posix.cpp"
    "${_bp_src}/detail/last_error.cpp"
    "${_bp_src}/detail/throw_error.cpp"
    "${_bp_src}/detail/utf8.cpp"
    "${_bp_src}/posix/close_handles.cpp"
    "${_bp_src}/ext/cmd.cpp"
    "${_bp_src}/ext/cwd.cpp"
    "${_bp_src}/ext/env.cpp"
    "${_bp_src}/ext/exe.cpp"
    "${_bp_src}/ext/proc_info.cpp"
  )
  target_include_directories(vacps_boost_process PUBLIC ${_vacps_boost_root})
  target_compile_definitions(vacps_boost_process
    PUBLIC
      BOOST_PROCESS_USE_STD_FS=1
      BOOST_PROCESS_STATIC_LINK=1
    PRIVATE
      BOOST_PROCESS_SOURCE=1
  )
  target_compile_features(vacps_boost_process PUBLIC cxx_std_17)
  set_target_properties(vacps_boost_process PROPERTIES
    POSITION_INDEPENDENT_CODE OFF
  )
  message(STATUS "Boost.Process v2 from ${_bp_src}")
else()
  message(FATAL_ERROR "Boost.Process sources not found (need FetchContent Boost with libs/process)")
endif()

# ── Layered libraries (n1.md §十九) ────────────────────────────────
# Domain layers do not link QuickJS. quickjs_core does not link domain modules
# (fs/storage/http/process) or bindings. quickjs_bindings composes domain +
# engine. vacps_native_core is an INTERFACE umbrella; vacps_runtime is the
# composition STATIC (ScriptServices + ApplicationRuntime).
#
# install_default_modules lives in bindings and is called from
# ApplicationRuntime after ScriptRuntime::create — no core→bindings cycle,
# so LINK_GROUP RESCAN is not required.

# Platform triple for vacps:host platform() — arch + libc from the compiler.
execute_process(
  COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
  OUTPUT_VARIABLE VACPS_COMPILER_TRIPLE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(VACPS_COMPILER_TRIPLE MATCHES "aarch64")
  set(_vacps_plat_arch "aarch64")
elseif(VACPS_COMPILER_TRIPLE MATCHES "x86_64")
  set(_vacps_plat_arch "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(_vacps_plat_arch "aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(_vacps_plat_arch "x86_64")
else()
  set(_vacps_plat_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(VACPS_COMPILER_TRIPLE MATCHES "musl")
  set(_vacps_plat_libc "musl")
elseif(VACPS_COMPILER_TRIPLE MATCHES "gnu")
  set(_vacps_plat_libc "gnu")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Static Alpine product default; preprocessor in platform.hpp can still refine.
  if(VACPS_STATIC)
    set(_vacps_plat_libc "musl")
  else()
    set(_vacps_plat_libc "gnu")
  endif()
else()
  set(_vacps_plat_libc "unknown")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_vacps_plat_os "linux")
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _vacps_plat_os)
endif()
set(VACPS_PLATFORM_STRING "${_vacps_plat_os}-${_vacps_plat_arch}-${_vacps_plat_libc}")
message(STATUS "VACPS_PLATFORM_STRING=${VACPS_PLATFORM_STRING} (triple=${VACPS_COMPILER_TRIPLE})")

function(vacps_layer name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${VACPS_BOOST_INCLUDE_DIRS}
  )
  target_compile_definitions(${name} PUBLIC
    VACPS_NATIVE_VERSION="${PROJECT_VERSION}"
    VACPS_PLATFORM_STRING="${VACPS_PLATFORM_STRING}"
    BOOST_ASIO_NO_DEPRECATED
    BOOST_BEAST_USE_STD_STRING_VIEW
    BOOST_PROCESS_USE_STD_FS=1
    BOOST_PROCESS_STATIC_LINK=1
  )
  target_compile_features(${name} PUBLIC cxx_std_23)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  if(VACPS_BOOST_TARGETS)
    target_link_libraries(${name} PUBLIC ${VACPS_BOOST_TARGETS})
  endif()
endfunction()

# ── vacps_base: app + bootstrap ────────────────────────────────────
vacps_layer(vacps_base
  src/app/version.cpp
  src/app/log.cpp
  src/bootstrap/environment.cpp
  src/bootstrap/config.cpp
)
target_link_libraries(vacps_base PUBLIC
  Threads::Threads
  spdlog::spdlog
)

# ── vacps_crypto ───────────────────────────────────────────────────
vacps_layer(vacps_crypto
  src/crypto/crypto.cpp
)
target_link_libraries(vacps_crypto PUBLIC
  vacps_base
  OpenSSL::SSL
  OpenSSL::Crypto
)

# ── vacps_fs (uses crypto for content hash; Asio headers for async) ─
vacps_layer(vacps_fs
  src/fs/fs.cpp
  src/fs/open_options.cpp
  src/fs/file.cpp
  src/fs/async.cpp
  src/fs/io_uring_probe.cpp
)
target_link_libraries(vacps_fs PUBLIC
  vacps_base
  vacps_crypto
)

# liburing: compile-time enable Asio random_access_file + runtime probe (setup+NOP).
# Docker default seccomp blocks io_uring_setup → probe fails → thread_pool only.
# Incomplete profiles (setup OK, enter denied) also fail probe — avoids pending ops.
# Asio has no automatic file→thread_pool fallback; we branch ourselves.
find_path(VACPS_URING_INCLUDE liburing.h)
unset(VACPS_URING_LIB CACHE)
if(VACPS_STATIC)
  set(_vacps_lib_suffixes_save ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  find_library(VACPS_URING_LIB uring)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_vacps_lib_suffixes_save})
else()
  find_library(VACPS_URING_LIB uring)
endif()
if(VACPS_URING_LIB AND VACPS_URING_INCLUDE)
  # Global so open_options Flags type (Asio file_base vs fallback) is ODR-consistent.
  add_compile_definitions(
    BOOST_ASIO_HAS_IO_URING=1
    VACPS_HAVE_LIBURING=1
  )
  include_directories(${VACPS_URING_INCLUDE})
  target_link_libraries(vacps_fs PUBLIC ${VACPS_URING_LIB})
  message(STATUS "liburing: ${VACPS_URING_LIB} (random_access_file if probe_io_uring() ok)")
else()
  message(STATUS "liburing: not found (vacps:fs File always thread_pool)")
endif()

# ── vacps_storage ──────────────────────────────────────────────────
vacps_layer(vacps_storage
  src/storage/database.cpp
  src/storage/store.cpp
)
target_link_libraries(vacps_storage PUBLIC
  vacps_base
  vacps_sqlite
)

# ── vacps_process (Process + ProcessRuntime + budget; no Registry) ─
vacps_layer(vacps_process
  src/process/process.cpp
  src/process/runtime.cpp
  src/process/budget.cpp
)
target_link_libraries(vacps_process PUBLIC
  vacps_base
  vacps_boost_process
)

# ── vacps_url (domain; Ada) ────────────────────────────────────────
vacps_layer(vacps_url
  src/url/url.cpp
  src/url/search_params.cpp
)
target_link_libraries(vacps_url PUBLIC
  vacps_base
  ada
)

# ── vacps_text (Encoder header-only + Decoder; simdutf) ────────────
vacps_layer(vacps_text
  src/text/decoder.cpp
)
target_link_libraries(vacps_text PUBLIC
  vacps_base
  simdutf::simdutf
)

# ── vacps_http (transport only — IRequestHandler; no QuickJS) ──────
# script_dispatch lives in vacps_quickjs_bindings (needs ScriptRuntime).
# HTTP transport only — JS owns Server instances (no ServerRegistry).
vacps_layer(vacps_http
  src/http/client.cpp
  src/http/server.cpp
  src/http/session.cpp
)
target_link_libraries(vacps_http PUBLIC
  vacps_base
  ada
  nlohmann_json::nlohmann_json
  OpenSSL::SSL
  OpenSSL::Crypto
)

# ── vacps_quickjs_core: engine RAII + ScriptRuntime ────────────────
# Headers: value, convert, cstring, atom, class_binding, object_holder,
# promise_bridge, js_bridge (engine helpers only — no storage types).
# ScriptServices lives in vacps_runtime (composition): core must not link
# fs/storage/http/process per n1 §十九. Headers may still name those types
# via ScriptServices / ModuleBindings (include path only).
vacps_layer(vacps_quickjs_core
  src/quickjs/runtime.cpp
  src/quickjs/context.cpp
  src/quickjs/script_runtime.cpp
)
target_link_libraries(vacps_quickjs_core PUBLIC
  vacps_base
  vacps_quickjs
)

# ── vacps_quickjs_bindings: modules, globals, HTTP→JS adapter ──────
# module_catalog.cpp: make_default_module_catalog + install_default_modules.
vacps_layer(vacps_quickjs_bindings
  src/quickjs/module_catalog.cpp
  src/quickjs/globals/install.cpp
  src/quickjs/globals/url_binding.cpp
  src/quickjs/globals/url_search_params_binding.cpp
  src/quickjs/globals/text_encoder_binding.cpp
  src/quickjs/globals/text_decoder_binding.cpp
  src/quickjs/bindings/log_binding.cpp
  src/quickjs/bindings/host_binding.cpp
  src/quickjs/bindings/crypto_binding.cpp
  src/quickjs/bindings/fs_binding.cpp
  src/quickjs/bindings/store_binding.cpp
  src/quickjs/bindings/http_binding.cpp
  src/quickjs/bindings/process_binding.cpp
  src/http/script_dispatch.cpp
  src/quickjs/script_request_handler.cpp
)
target_link_libraries(vacps_quickjs_bindings PUBLIC
  vacps_quickjs_core
  vacps_fs
  vacps_storage
  vacps_process
  vacps_crypto
  vacps_http
  vacps_url
  vacps_text
  ada
  simdutf::simdutf
  nlohmann_json::nlohmann_json
)

# ── Umbrella: engine + bindings + domains (no composition / process entry) ─
# One-way deps: bindings → core; domains do not link QuickJS. Plain interface
# link (no RESCAN) after install_default_modules left core.
add_library(vacps_native_core INTERFACE)
target_link_libraries(vacps_native_core INTERFACE
  vacps_quickjs_bindings
  vacps_quickjs_core
  vacps_http
  vacps_process
  vacps_fs
  vacps_storage
  vacps_crypto
  vacps_url
  vacps_text
  vacps_base
)

# Composition / process entry (ShutdownCoordinator; ownership doc).
# ScriptServices (process backend Registry, fs/db pools, probe) lives here.
vacps_layer(vacps_runtime
  src/quickjs/script_services.cpp
  src/runtime/shutdown_coordinator.cpp
  src/runtime/tick_loop.cpp
  src/runtime/application_runtime.cpp
)
target_link_libraries(vacps_runtime PUBLIC
  vacps_native_core
  vacps_process
  vacps_http
  vacps_fs
)

# ── Agent binary ───────────────────────────────────────────────────
add_executable(vacps-agent src/main.cpp)
set_target_properties(vacps-agent PROPERTIES
  OUTPUT_NAME "vacps-agent-linux-x86_64"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(vacps-agent PRIVATE vacps_runtime)

include(GNUInstallDirs)
install(TARGETS vacps-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── Unit tests (GoogleTest 1.17.0) ─────────────────────────────────
if(VACPS_BUILD_TESTS)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GTest.cmake")
  enable_testing()

  add_executable(vacps-native-tests
    tests/test_crypto.cpp
    tests/test_fs.cpp
    tests/test_fs_file.cpp
    tests/test_fs_async.cpp
    tests/test_db.cpp
    tests/test_process.cpp
    tests/test_script_runtime.cpp
    tests/test_js_native_api.cpp
    tests/test_promise_bridge.cpp
    tests/test_url_global.cpp
    tests/test_text_decoder.cpp
    tests/test_platform.cpp
    tests/test_http_client.cpp
    tests/test_js_tasks.cpp
    tests/test_shutdown.cpp
  )
  target_link_libraries(vacps-native-tests PRIVATE
    vacps_runtime
    GTest::gtest_main
  )
  target_compile_definitions(vacps-native-tests PRIVATE
    VACPS_NATIVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  set_target_properties(vacps-native-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )

  include(GoogleTest)
  gtest_discover_tests(vacps-native-tests
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DISCOVERY_TIMEOUT 30
  )
endif()

message(STATUS "vacps-native ${PROJECT_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  VACPS_STATIC: ${VACPS_STATIC}")
message(STATUS "  VACPS_BUILD_TESTS: ${VACPS_BUILD_TESTS}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Layers: base crypto fs storage process url text http quickjs_core quickjs_bindings")
message(STATUS "  Umbrella: vacps_native_core (INTERFACE); vacps_runtime (STATIC composition)")
