# GoogleTest 1.17.0 (FetchContent). Only included when VACPS_BUILD_TESTS=ON.

set(VACPS_GTEST_VERSION 1.17.0)
set(VACPS_GTEST_URL
  "https://github.com/google/googletest/archive/refs/tags/v${VACPS_GTEST_VERSION}.tar.gz")
set(VACPS_GTEST_SHA256
  "65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c")

message(STATUS "Fetching GoogleTest ${VACPS_GTEST_VERSION} ...")
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Avoid installing / building shared gtest when we static-link the agent.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL ${VACPS_GTEST_URL}
  URL_HASH SHA256=${VACPS_GTEST_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)
message(STATUS "Using GoogleTest ${VACPS_GTEST_VERSION}")
