#pragma once

#include <memory>

namespace vacps::js {

/**
 * JS class opaque payload: shared ownership so async ops and the GC finalizer
 * stay safe independently of object lifetime.
 *
 * Finalizer deletes the ObjectHolder (drops one shared_ptr ref); pending work
 * may still hold the same shared_ptr<T>.
 */
template <typename T>
struct ObjectHolder {
  std::shared_ptr<T> value;
};

}  // namespace vacps::js
