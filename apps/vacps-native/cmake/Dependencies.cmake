# Third-party dependencies for vacps-native.
#
# Boost 1.91.0 is fetched by default (Alpine system boost is older).
# Set -DVACPS_USE_SYSTEM_BOOST=ON to use find_package instead.
#
# Asio / Beast / System are header-only since modern Boost; the official
# archives.boost.io tarball has no CMake package targets, so we only need
# the include root (boost/...).

include(FetchContent)

option(VACPS_USE_SYSTEM_BOOST "Use find_package(Boost) instead of FetchContent 1.91.0" OFF)

set(VACPS_BOOST_TARGETS "")
set(VACPS_BOOST_INCLUDE_DIRS "")

if(VACPS_USE_SYSTEM_BOOST)
  find_package(Boost 1.83 REQUIRED)
  if(TARGET Boost::headers)
    set(VACPS_BOOST_TARGETS Boost::headers)
  elseif(TARGET Boost::boost)
    set(VACPS_BOOST_TARGETS Boost::boost)
  else()
    set(VACPS_BOOST_INCLUDE_DIRS ${Boost_INCLUDE_DIRS})
  endif()
  message(STATUS "Using system Boost ${Boost_VERSION}")
else()
  # Official release tarball (classic layout; 1.91.0 has no -cmake release).
  # SHA256 verified 2026-07-29 from archives.boost.io.
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
  # Populate only — do not add_subdirectory (tarball has no root CMake package).
  # CMP0169: direct Populate(name) with declared details is deprecated; keep OLD for now.
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_GetProperties(Boost)
  if(NOT boost_POPULATED)
    FetchContent_Populate(Boost)
  endif()

  set(VACPS_BOOST_INCLUDE_DIRS "${boost_SOURCE_DIR}")
  message(STATUS "Using FetchContent Boost ${VACPS_BOOST_VERSION} (header-only @ ${boost_SOURCE_DIR})")
endif()

# ── SQLite amalgamation (compiled into the agent; no system libsqlite3) ──
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
  PUBLIC
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

# ── spdlog 1.17.0 (compiled static; bundles fmt) ──
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
# Avoid position-independent default when we static-link the agent.
set(SPDLOG_ENABLE_PCH OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  spdlog
  URL ${VACPS_SPDLOG_URL}
  URL_HASH SHA256=${VACPS_SPDLOG_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(spdlog)
message(STATUS "Using spdlog ${VACPS_SPDLOG_VERSION}")

# ── nlohmann/json 3.12.0 (header-only) ──
set(VACPS_NLOHMANN_JSON_VERSION 3.12.0)
set(VACPS_NLOHMANN_JSON_URL
  "https://github.com/nlohmann/json/archive/refs/tags/v${VACPS_NLOHMANN_JSON_VERSION}.tar.gz")
set(VACPS_NLOHMANN_JSON_SHA256
  "4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187")

message(STATUS "Fetching nlohmann/json ${VACPS_NLOHMANN_JSON_VERSION} ...")
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  nlohmann_json
  URL ${VACPS_NLOHMANN_JSON_URL}
  URL_HASH SHA256=${VACPS_NLOHMANN_JSON_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
message(STATUS "Using nlohmann/json ${VACPS_NLOHMANN_JSON_VERSION}")

# ── QuickJS (bellard official release; static, no quickjs-libc host yet) ──
# Design §25.6 / §23: RAII host owns Runtime+Context; pure engine only for now.
# Official release: https://bellard.org/quickjs/ (2026-06-04)
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

# Core engine only (no std/os from quickjs-libc until we own the host surface).
add_library(vacps_quickjs STATIC
  "${_vacps_qjs_dir}/quickjs.c"
  "${_vacps_qjs_dir}/dtoa.c"
  "${_vacps_qjs_dir}/libregexp.c"
  "${_vacps_qjs_dir}/libunicode.c"
  "${_vacps_qjs_dir}/cutils.c"
)
# SYSTEM: quiet C99 compound-literal noise from quickjs.h under -Wpedantic.
target_include_directories(vacps_quickjs SYSTEM PUBLIC "${_vacps_qjs_dir}")
target_compile_definitions(vacps_quickjs
  PUBLIC
    CONFIG_VERSION=\"${VACPS_QUICKJS_VERSION}\"
    _GNU_SOURCE
)
target_compile_options(vacps_quickjs PRIVATE
  $<$<COMPILE_LANG_AND_ID:C,Clang,GNU>:-w>
)
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
