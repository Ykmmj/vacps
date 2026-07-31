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

# ── Core library (shared by agent + tests) ─────────────────────────
# Layout: src/{main,app,quickjs,process,storage,http,crypto,fs}
add_library(vacps_native_core STATIC
  src/app/version.cpp
  src/app/config.cpp
  src/app/log.cpp
  src/storage/db.cpp
  src/fs/fs.cpp
  src/fs/async.cpp
  src/fs/io_uring_probe.cpp
  src/process/process.cpp
  src/process/registry.cpp
  src/crypto/crypto.cpp
  src/http/script_dispatch.cpp
  src/http/client.cpp
  src/quickjs/runtime.cpp
  src/quickjs/context.cpp
  src/quickjs/host.cpp
  src/quickjs/modules.cpp
  src/quickjs/url_globals.cpp
)

target_include_directories(vacps_native_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  ${VACPS_BOOST_INCLUDE_DIRS}
)

target_compile_definitions(vacps_native_core PUBLIC
  VACPS_NATIVE_VERSION="${PROJECT_VERSION}"
  BOOST_ASIO_NO_DEPRECATED
  BOOST_BEAST_USE_STD_STRING_VIEW
  BOOST_PROCESS_USE_STD_FS=1
  BOOST_PROCESS_STATIC_LINK=1
)

if(VACPS_BOOST_TARGETS)
  target_link_libraries(vacps_native_core PUBLIC ${VACPS_BOOST_TARGETS})
endif()

target_link_libraries(vacps_native_core PUBLIC
  Threads::Threads
  vacps_sqlite
  vacps_quickjs
  vacps_boost_process
  ada
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  OpenSSL::SSL
  OpenSSL::Crypto
)

# liburing: compile-time enable Asio stream_file + runtime probe (setup+NOP).
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
  target_compile_definitions(vacps_native_core PUBLIC
    BOOST_ASIO_HAS_IO_URING=1
    VACPS_HAVE_LIBURING=1
  )
  target_include_directories(vacps_native_core PUBLIC ${VACPS_URING_INCLUDE})
  target_link_libraries(vacps_native_core PUBLIC ${VACPS_URING_LIB})
  message(STATUS "liburing: ${VACPS_URING_LIB} (stream_file if probe_io_uring() ok)")
else()
  message(STATUS "liburing: not found (vacps:fs content always thread_pool)")
endif()

# ── Agent binary ───────────────────────────────────────────────────
add_executable(vacps-agent src/main.cpp)
set_target_properties(vacps-agent PROPERTIES
  OUTPUT_NAME "vacps-agent-linux-x86_64"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(vacps-agent PRIVATE vacps_native_core)

include(GNUInstallDirs)
install(TARGETS vacps-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── Unit tests (GoogleTest 1.17.0) ─────────────────────────────────
if(VACPS_BUILD_TESTS)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GTest.cmake")
  enable_testing()

  add_executable(vacps-native-tests
    tests/test_crypto.cpp
    tests/test_fs.cpp
    tests/test_fs_async.cpp
    tests/test_db.cpp
    tests/test_process.cpp
    tests/test_config.cpp
    tests/test_host.cpp
    tests/test_js_native_api.cpp
    tests/test_promise_bridge.cpp
    tests/test_url_global.cpp
    tests/test_http_client.cpp
    tests/test_js_tasks.cpp
  )
  target_link_libraries(vacps-native-tests PRIVATE
    vacps_native_core
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
