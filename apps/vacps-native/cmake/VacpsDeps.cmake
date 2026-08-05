# Runtime-core + platform globals dependencies (see docs/RUNTIME_LAYERING.md).
# Core: Boost (Asio), QuickJS, spdlog, Threads.
# Platform globals (URL / Text*): Ada, simdutf.

include(FetchContent)

option(VACPS_USE_SYSTEM_BOOST "Use find_package(Boost) instead of FetchContent 1.91.0" OFF)

set(VACPS_BOOST_TARGETS "")
set(VACPS_BOOST_INCLUDE_DIRS "")
# Imported or locally built Boost.Process v2 target (required for vacps_process).
set(VACPS_BOOST_PROCESS_TARGET "")

if(VACPS_USE_SYSTEM_BOOST)
  find_package(Boost 1.83 REQUIRED)
  if(TARGET Boost::headers)
    set(VACPS_BOOST_TARGETS Boost::headers)
  elseif(TARGET Boost::boost)
    set(VACPS_BOOST_TARGETS Boost::boost)
  else()
    set(VACPS_BOOST_INCLUDE_DIRS ${Boost_INCLUDE_DIRS})
  endif()
  # Process v2 is a compiled library — headers alone leave unresolved symbols
  # (do_throw_error, close_all, error categories, …). Require the real target.
  if(NOT TARGET Boost::process)
    find_package(Boost 1.83 COMPONENTS process QUIET)
  endif()
  if(NOT TARGET Boost::process)
    message(FATAL_ERROR
      "VACPS_USE_SYSTEM_BOOST=ON requires an imported Boost::process target "
      "(Boost.Process v2 compiled library). Install a Boost package that "
      "provides Boost::process, or leave VACPS_USE_SYSTEM_BOOST=OFF to build "
      "Process from the FetchContent 1.91 sources.")
  endif()
  set(VACPS_BOOST_PROCESS_TARGET Boost::process)
  message(STATUS "Using system Boost ${Boost_VERSION} (headers + Boost::process)")
else()
  set(VACPS_BOOST_VERSION 1.91.0)
  set(VACPS_BOOST_URL
    "https://archives.boost.io/release/${VACPS_BOOST_VERSION}/source/boost_1_91_0.tar.gz")
  set(VACPS_BOOST_SHA256
    "5734305f40a76c30f951c9abd409a45a2a19fb546efe4162119250bbe4d3a463")

  message(STATUS "Fetching Boost ${VACPS_BOOST_VERSION} ...")
  FetchContent_Declare(
    Boost
    URL ${VACPS_BOOST_URL}
    URL_HASH SHA256=${VACPS_BOOST_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_GetProperties(Boost)
  if(NOT boost_POPULATED)
    FetchContent_Populate(Boost)
  endif()
  set(VACPS_BOOST_INCLUDE_DIRS "${boost_SOURCE_DIR}")
  message(STATUS "Using FetchContent Boost ${VACPS_BOOST_VERSION} @ ${boost_SOURCE_DIR}")

  # Boost.Process v2 has compiled sources. The release tarball is header-layout
  # only (no Boost::* package targets), so libs/process/CMakeLists.txt cannot be
  # added_subdirectory'd. Build the official source list as a dedicated static
  # third-party target (not mixed into vacps_process).
  set(_vacps_bp_src "${boost_SOURCE_DIR}/libs/process/src")
  if(NOT EXISTS "${_vacps_bp_src}/error.cpp")
    message(FATAL_ERROR
      "Boost.Process sources missing under ${_vacps_bp_src} "
      "(expected FetchContent Boost ${VACPS_BOOST_VERSION} layout)")
  endif()
  # Official source list from libs/process/CMakeLists.txt (Boost 1.91).
  # Platform-specific TUs are empty stubs via BOOST_PROCESS_V2_* ifdefs.
  add_library(vacps_boost_process STATIC
    "${_vacps_bp_src}/detail/environment_posix.cpp"
    "${_vacps_bp_src}/detail/environment_win.cpp"
    "${_vacps_bp_src}/detail/last_error.cpp"
    "${_vacps_bp_src}/detail/process_handle_windows.cpp"
    "${_vacps_bp_src}/detail/throw_error.cpp"
    "${_vacps_bp_src}/detail/utf8.cpp"
    "${_vacps_bp_src}/ext/cmd.cpp"
    "${_vacps_bp_src}/ext/cwd.cpp"
    "${_vacps_bp_src}/ext/env.cpp"
    "${_vacps_bp_src}/ext/exe.cpp"
    "${_vacps_bp_src}/ext/proc_info.cpp"
    "${_vacps_bp_src}/posix/close_handles.cpp"
    "${_vacps_bp_src}/windows/default_launcher.cpp"
    "${_vacps_bp_src}/environment.cpp"
    "${_vacps_bp_src}/error.cpp"
    "${_vacps_bp_src}/pid.cpp"
    "${_vacps_bp_src}/shell.cpp"
  )
  # Flattened release headers live at ${boost_SOURCE_DIR}/boost/...
  target_include_directories(vacps_boost_process PUBLIC
    "${boost_SOURCE_DIR}"
  )
  # Keep Process ABI/config macros PRIVATE on this third-party target so they
  # do not leak into unrelated TUs via PUBLIC link propagation. Consumers that
  # include Boost.Process headers (vacps_process only) set the ABI macros
  # privately themselves when linking this custom target.
  target_compile_definitions(vacps_boost_process PRIVATE
    BOOST_PROCESS_SOURCE=1
    BOOST_PROCESS_USE_STD_FS=1
    BOOST_PROCESS_STATIC_LINK=1
  )
  target_compile_features(vacps_boost_process PUBLIC cxx_std_17)
  set_target_properties(vacps_boost_process PROPERTIES
    POSITION_INDEPENDENT_CODE OFF
  )
  # Quiet third-party noise; keep sanitizers from the parent toolchain.
  target_compile_options(vacps_boost_process PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-w>
  )
  set(VACPS_BOOST_PROCESS_TARGET vacps_boost_process)
  message(STATUS "Building vacps_boost_process from FetchContent Boost.Process v2 sources")
endif()

# ── spdlog 1.17.0 ──────────────────────────────────────────────────
set(VACPS_SPDLOG_VERSION 1.17.0)
set(VACPS_SPDLOG_URL
  "https://github.com/gabime/spdlog/archive/refs/tags/v${VACPS_SPDLOG_VERSION}.tar.gz")
set(VACPS_SPDLOG_SHA256
  "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744")

message(STATUS "Fetching spdlog ${VACPS_SPDLOG_VERSION} ...")
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_ENABLE_PCH OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  spdlog
  URL ${VACPS_SPDLOG_URL}
  URL_HASH SHA256=${VACPS_SPDLOG_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(spdlog)
message(STATUS "Using spdlog ${VACPS_SPDLOG_VERSION}")

# ── mimalloc 3.4.4 (QuickJS backing heap only) ─────────────────────
set(VACPS_MIMALLOC_VERSION 3.4.4)
set(VACPS_MIMALLOC_URL
  "https://github.com/microsoft/mimalloc/archive/refs/tags/v${VACPS_MIMALLOC_VERSION}.tar.gz")
set(VACPS_MIMALLOC_SHA256
  "8ba991a7266983bd5eefc36e140c24734f720fd9b1fd79ddaeff44ea85d16760")

message(STATUS "Fetching mimalloc ${VACPS_MIMALLOC_VERSION} ...")
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_LIBC_MUSL ON CACHE BOOL "" FORCE)
set(MI_TRACK_ASAN ${VACPS_ENABLE_ASAN} CACHE BOOL "" FORCE)

FetchContent_Declare(
  mimalloc
  URL ${VACPS_MIMALLOC_URL}
  URL_HASH SHA256=${VACPS_MIMALLOC_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(mimalloc)
if(NOT mimalloc_POPULATED)
  FetchContent_Populate(mimalloc)
endif()
if(NOT TARGET mimalloc-static)
  add_subdirectory(
    "${mimalloc_SOURCE_DIR}"
    "${mimalloc_BINARY_DIR}"
    EXCLUDE_FROM_ALL
  )
endif()
set_target_properties(mimalloc-static PROPERTIES
  POSITION_INDEPENDENT_CODE OFF
)
target_compile_options(mimalloc-static PRIVATE
  $<$<COMPILE_LANG_AND_ID:C,Clang,GNU>:-w>
)
message(STATUS "Using mimalloc ${VACPS_MIMALLOC_VERSION}")

# ── QuickJS (bellard 2026-06-04) ───────────────────────────────────
set(VACPS_QUICKJS_VERSION 2026-06-04)
set(VACPS_QUICKJS_URL
  "https://bellard.org/quickjs/quickjs-${VACPS_QUICKJS_VERSION}.tar.xz")
set(VACPS_QUICKJS_SHA256
  "b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a")

message(STATUS "Fetching QuickJS ${VACPS_QUICKJS_VERSION} ...")
FetchContent_Declare(
  quickjs
  URL ${VACPS_QUICKJS_URL}
  URL_HASH SHA256=${VACPS_QUICKJS_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(quickjs)
if(NOT quickjs_POPULATED)
  FetchContent_Populate(quickjs)
endif()

set(_vacps_qjs_dir "${quickjs_SOURCE_DIR}")
if(NOT EXISTS "${_vacps_qjs_dir}/quickjs.c")
  set(_vacps_qjs_dir "${quickjs_SOURCE_DIR}/quickjs-${VACPS_QUICKJS_VERSION}")
endif()
if(NOT EXISTS "${_vacps_qjs_dir}/quickjs.c")
  message(FATAL_ERROR "quickjs.c not found under ${quickjs_SOURCE_DIR}")
endif()

add_library(vacps_quickjs STATIC
  "${_vacps_qjs_dir}/quickjs.c"
  "${_vacps_qjs_dir}/dtoa.c"
  "${_vacps_qjs_dir}/libregexp.c"
  "${_vacps_qjs_dir}/libunicode.c"
  "${_vacps_qjs_dir}/cutils.c"
)
target_include_directories(vacps_quickjs SYSTEM PUBLIC "${_vacps_qjs_dir}")
target_compile_definitions(vacps_quickjs
  PUBLIC
    CONFIG_VERSION=\"${VACPS_QUICKJS_VERSION}\"
    _GNU_SOURCE
)
target_compile_options(vacps_quickjs PRIVATE
  $<$<COMPILE_LANG_AND_ID:C,Clang,GNU>:-w>
)
# QuickJS intentionally uses patterns UBSan flags in third-party C (function
# pointer types, signed left shifts in bytecode packing, etc.). Keep ASan/TSan
# on this target; only strip undefined-behavior instrumentation.
if(VACPS_ENABLE_UBSAN)
  target_compile_options(vacps_quickjs PRIVATE -fno-sanitize=undefined)
endif()
set_target_properties(vacps_quickjs PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE OFF
)
find_library(VACPS_LIBM m)
if(VACPS_LIBM)
  target_link_libraries(vacps_quickjs PUBLIC ${VACPS_LIBM})
endif()
message(STATUS "Using QuickJS ${VACPS_QUICKJS_VERSION} @ ${_vacps_qjs_dir}")

find_package(Threads REQUIRED)

# ── Ada URL 4.0.0 (WHATWG; globalThis.URL) ─────────────────────────
set(VACPS_ADA_VERSION 4.0.0)
set(VACPS_ADA_URL
  "https://github.com/ada-url/ada/archive/refs/tags/v${VACPS_ADA_VERSION}.tar.gz")
set(VACPS_ADA_SHA256
  "6d6c7ef7dd2e329320d34eb2ab29ccdc879ee3935af9dfb894a6640e58dc381d")
message(STATUS "Fetching Ada ${VACPS_ADA_VERSION} ...")
set(ADA_TESTING OFF CACHE BOOL "" FORCE)
set(ADA_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ADA_TOOLS OFF CACHE BOOL "" FORCE)
set(ADA_USE_SIMDUTF OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ada
  URL ${VACPS_ADA_URL}
  URL_HASH SHA256=${VACPS_ADA_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(ada)
message(STATUS "Using Ada ${VACPS_ADA_VERSION}")

# ── simdutf 9.0.0 (TextEncoder / TextDecoder) ─────────────────────
set(VACPS_SIMDUTF_VERSION 9.0.0)
set(VACPS_SIMDUTF_URL
  "https://github.com/simdutf/simdutf/archive/refs/tags/v${VACPS_SIMDUTF_VERSION}.tar.gz")
set(VACPS_SIMDUTF_SHA256
  "fd2ce975f29809a975a8da8843cfb3a7265af3f71be548f199d23cf65e101764")
message(STATUS "Fetching simdutf ${VACPS_SIMDUTF_VERSION} ...")
set(SIMDUTF_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_ICONV OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  simdutf
  URL ${VACPS_SIMDUTF_URL}
  URL_HASH SHA256=${VACPS_SIMDUTF_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(simdutf)
message(STATUS "Using simdutf ${VACPS_SIMDUTF_VERSION}")

# ── OpenSSL (vacps:crypto — RAND / SHA-256 / Base64 / Ed25519) ──────
find_package(OpenSSL REQUIRED)
message(STATUS "Using OpenSSL ${OPENSSL_VERSION}")


# ── SQLite amalgamation (compiled in; no system libsqlite3) ────────
# https://sqlite.org/download.html — 3.53.4 → 3530400
set(VACPS_SQLITE_VERSION 3530400) # 3.53.4
set(VACPS_SQLITE_URL
  "https://www.sqlite.org/2026/sqlite-amalgamation-${VACPS_SQLITE_VERSION}.zip")
set(VACPS_SQLITE_SHA256
  "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d")

message(STATUS "Fetching SQLite amalgamation ${VACPS_SQLITE_VERSION} ...")
FetchContent_Declare(
  sqlite_amalgamation
  URL ${VACPS_SQLITE_URL}
  URL_HASH SHA256=${VACPS_SQLITE_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(sqlite_amalgamation)
if(NOT sqlite_amalgamation_POPULATED)
  FetchContent_Populate(sqlite_amalgamation)
endif()

set(_vacps_sqlite_c "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
set(_vacps_sqlite_h_dir "${sqlite_amalgamation_SOURCE_DIR}")
if(NOT EXISTS "${_vacps_sqlite_c}")
  set(_vacps_sqlite_c
    "${sqlite_amalgamation_SOURCE_DIR}/sqlite-amalgamation-${VACPS_SQLITE_VERSION}/sqlite3.c")
  set(_vacps_sqlite_h_dir
    "${sqlite_amalgamation_SOURCE_DIR}/sqlite-amalgamation-${VACPS_SQLITE_VERSION}")
endif()
if(NOT EXISTS "${_vacps_sqlite_c}")
  message(FATAL_ERROR "sqlite3.c not found under ${sqlite_amalgamation_SOURCE_DIR}")
endif()

add_library(vacps_sqlite STATIC "${_vacps_sqlite_c}")
target_include_directories(vacps_sqlite PUBLIC "${_vacps_sqlite_h_dir}")
target_compile_definitions(vacps_sqlite
  PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_USE_URI=1
)
target_compile_options(vacps_sqlite PRIVATE
  $<$<COMPILE_LANG_AND_ID:C,Clang,GNU>:-w>
)
set_target_properties(vacps_sqlite PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE OFF
)
message(STATUS "Using SQLite amalgamation ${VACPS_SQLITE_VERSION} @ ${_vacps_sqlite_h_dir}")

# Layer helper: static lib with src/ + Boost includes.
function(vacps_layer name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${VACPS_BOOST_INCLUDE_DIRS}
  )
  target_compile_definitions(${name} PUBLIC
    VACPS_NATIVE_VERSION="${PROJECT_VERSION}"
    BOOST_ASIO_NO_DEPRECATED
    # Consistent across all Asio TUs (file support + ODR). Does NOT select
    # io_uring as the default reactor — epoll remains default on Linux.
    BOOST_ASIO_HAS_IO_URING=1
  )
  target_compile_features(${name} PUBLIC cxx_std_23)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  if(VACPS_BOOST_TARGETS)
    target_link_libraries(${name} PUBLIC ${VACPS_BOOST_TARGETS})
  endif()
endfunction()
