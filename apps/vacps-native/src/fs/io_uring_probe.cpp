#include "fs/io_uring_probe.hpp"

#include "app/log.hpp"

#include <cstring>
#include <string>

#if defined(VACPS_HAVE_LIBURING)
#include <liburing.h>
#endif

namespace vacps::fs {

bool probe_io_uring() noexcept {
#if !defined(VACPS_HAVE_LIBURING)
  return false;
#else
  io_uring ring{};
  int rc = ::io_uring_queue_init(8, &ring, 0);
  if (rc < 0) {
    log::info(
        "io_uring probe: setup failed ({}) — vacps:fs File uses thread_pool",
        std::strerror(-rc));
    return false;
  }

  struct RingGuard {
    io_uring* r;
    ~RingGuard() { ::io_uring_queue_exit(r); }
  } guard{&ring};

  io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
  if (sqe == nullptr) {
    log::warn("io_uring probe: get_sqe failed — thread_pool fallback");
    return false;
  }
  ::io_uring_prep_nop(sqe);

  rc = ::io_uring_submit(&ring);
  if (rc < 0) {
    // setup ok but enter denied (incomplete seccomp) — do NOT use Asio file
    log::warn(
        "io_uring probe: submit/enter failed ({}) — thread_pool fallback "
        "(would leave Asio ops pending)",
        std::strerror(-rc));
    return false;
  }

  io_uring_cqe* cqe = nullptr;
  rc = ::io_uring_wait_cqe(&ring, &cqe);
  if (rc < 0 || cqe == nullptr) {
    log::warn(
        "io_uring probe: wait_cqe failed ({}) — thread_pool fallback",
        rc < 0 ? std::strerror(-rc) : "null cqe");
    return false;
  }
  const int op_res = cqe->res;
  ::io_uring_cqe_seen(&ring, cqe);
  if (op_res < 0) {
    log::warn(
        "io_uring probe: NOP failed ({}) — thread_pool fallback",
        std::strerror(-op_res));
    return false;
  }

  log::info(
      "io_uring probe: ok — vacps:fs File may use Asio random_access_file");
  return true;
#endif
}

}  // namespace vacps::fs
