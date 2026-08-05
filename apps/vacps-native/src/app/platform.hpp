#pragma once

/**
 * Host platform triple for vacps:host platform().
 * CMake sets VACPS_PLATFORM_STRING (e.g. linux-x86_64-musl); preprocessor
 * fallback covers aarch64/x86_64 + musl/gnu when the define is absent.
 */

#ifndef VACPS_PLATFORM_STRING

#if defined(__linux__)
#  define VACPS_PLATFORM_OS "linux"
#elif defined(__APPLE__)
#  define VACPS_PLATFORM_OS "darwin"
#else
#  define VACPS_PLATFORM_OS "unknown"
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#  define VACPS_PLATFORM_ARCH "aarch64"
#elif defined(__x86_64__) || defined(_M_X64)
#  define VACPS_PLATFORM_ARCH "x86_64"
#else
#  define VACPS_PLATFORM_ARCH "unknown"
#endif

#if defined(__GLIBC__)
#  define VACPS_PLATFORM_LIBC "gnu"
#elif defined(__linux__)
/* Non-glibc Linux (Alpine, custom musl toolchains) */
#  define VACPS_PLATFORM_LIBC "musl"
#else
#  define VACPS_PLATFORM_LIBC "unknown"
#endif

#define VACPS_PLATFORM_STRING \
  VACPS_PLATFORM_OS "-" VACPS_PLATFORM_ARCH "-" VACPS_PLATFORM_LIBC

#endif  // VACPS_PLATFORM_STRING

namespace vacps {

/** Compile-time platform triple (e.g. "linux-x86_64-musl"). */
[[nodiscard]] inline constexpr const char* platform_string() noexcept {
  return VACPS_PLATFORM_STRING;
}

}  // namespace vacps
