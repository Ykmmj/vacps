#include "version.hpp"

#ifndef VACPS_NATIVE_VERSION
#define VACPS_NATIVE_VERSION "0.0.0-dev"
#endif

namespace vacps {

const char* version() noexcept {
  return VACPS_NATIVE_VERSION;
}

}  // namespace vacps
