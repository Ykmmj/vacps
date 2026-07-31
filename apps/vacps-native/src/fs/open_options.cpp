#include "fs/open_options.hpp"

namespace vacps::fs {

bool flags_create(Flags flags) noexcept {
#if defined(BOOST_ASIO_HAS_FILE)
  return (static_cast<unsigned>(flags) &
          static_cast<unsigned>(asio::file_base::create)) != 0;
#else
  return (static_cast<unsigned>(flags) & static_cast<unsigned>(Flags::create)) != 0;
#endif
}

}  // namespace vacps::fs
