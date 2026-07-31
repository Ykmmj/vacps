#pragma once

/**
 * Open flags for vacps::fs::File — Boost.Asio file_base bitmask (Node-like
 * bitwise OR of integer constants), not string open modes.
 *
 * On Linux with BOOST_ASIO_HAS_FILE, Flags is asio::file_base::flags (numeric
 * values match POSIX open flags). Without Asio file support, a compatible
 * bitmask enum keeps the same public API shape.
 *
 * JS passes vacps:fs numeric constants (O_* exports); bindings use
 * flags_from_int. No path policy / allowlist here. System open/fcntl is
 * implementation detail of File, not this header.
 */

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_FILE)
#include <boost/asio/file_base.hpp>
#endif

namespace vacps::fs {

namespace asio = boost::asio;

#if defined(BOOST_ASIO_HAS_FILE)

using Flags = asio::file_base::flags;

#else

/**
 * Compatible bitmask when Asio file support is not compiled in.
 * Numeric values match Linux open flags so flags_from_int / JS O_* stay stable.
 * No system headers in this public header.
 */
enum class Flags : int {
  read_only = 0,
  write_only = 1,
  read_write = 2,
  append = 1024,
  create = 64,
  exclusive = 128,
  truncate = 512,
  sync_all_on_write = 1052672,
};

inline Flags operator|(Flags a, Flags b) noexcept {
  return static_cast<Flags>(static_cast<int>(a) | static_cast<int>(b));
}
inline Flags operator&(Flags a, Flags b) noexcept {
  return static_cast<Flags>(static_cast<int>(a) & static_cast<int>(b));
}
inline Flags& operator|=(Flags& a, Flags b) noexcept {
  a = a | b;
  return a;
}

#endif

struct OpenOptions {
  /** e.g. Flags::write_only | Flags::create | Flags::truncate. */
  Flags flags{
#if defined(BOOST_ASIO_HAS_FILE)
      asio::file_base::read_only
#else
      Flags::read_only
#endif
  };
  /**
   * Permission bits when create is set.
   *
   * Applied on the **pool** backend (`open(2)` third argument). The **Asio**
   * backend (`random_access_file::open`) has no mode parameter in current
   * Boost.Asio — it uses the library default for O_CREAT. That asymmetry is
   * an Asio API limit under the intentional dual-backend design (see File),
   * not a reason to drop either backend.
   */
  unsigned mode{0644};
};

/** True when flags include create. */
[[nodiscard]] bool flags_create(Flags flags) noexcept;

/** Cast a raw bitmask (e.g. from JS number) to Flags. */
[[nodiscard]] inline Flags flags_from_int(int bits) noexcept {
  return static_cast<Flags>(bits);
}

[[nodiscard]] inline int flags_to_int(Flags flags) noexcept {
  return static_cast<int>(flags);
}

}  // namespace vacps::fs
