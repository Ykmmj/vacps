#pragma once

/**
 * Module-private FIFO queue that serializes complete logical FileHandle
 * operations across co_await suspension / run_blocking points.
 *
 * Used only by the vacps:fs JavaScript FileHandle binding. Not a domain
 * File backend abstraction — vacps::fs::File requires externally serialized
 * access and does not own a queue.
 *
 * Rules:
 * - All queue mutation / completion decisions run on the queue executor.
 * - std::stop_callback only asio::post's cancel work (never dispatch), and
 *   captures weak_ptr + executor value (not shared_ptr to the queue).
 * - Entry does not store shared_ptr<FileOperationQueue> (no queue→Entry→queue
 *   cycle). Handoff posts grant with a live shared_ptr supplied at release.
 * - stop_cb is destroyed only on the queue executor, never on the callback
 *   stack of stop_callback itself.
 * - Exactly-once completion per waiter (grant XOR abort).
 * - No std::mutex across co_await; exclusive Lease is held across suspension.
 *
 * Technical utility (Boost.Asio only) — no QuickJS / Runtime dependency.
 */

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace vacps::js::detail {

namespace asio = boost::asio;

class FileOperationQueue
    : public std::enable_shared_from_this<FileOperationQueue> {
 public:
  explicit FileOperationQueue(asio::any_io_executor ex)
      : ex_(std::move(ex)) {}

  FileOperationQueue(const FileOperationQueue&) = delete;
  FileOperationQueue& operator=(const FileOperationQueue&) = delete;

  /** RAII exclusive ownership of the next logical File operation. */
  class Lease {
   public:
    Lease() = default;
    explicit Lease(std::shared_ptr<FileOperationQueue> q) noexcept
        : queue_(std::move(q)) {}
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& o) noexcept : queue_(std::move(o.queue_)) {}
    Lease& operator=(Lease&& o) noexcept {
      if (this != &o) {
        reset();
        queue_ = std::move(o.queue_);
      }
      return *this;
    }
    ~Lease() { reset(); }

    void reset() noexcept {
      if (queue_) {
        queue_->release();
        queue_.reset();
      }
    }

    explicit operator bool() const noexcept {
      return static_cast<bool>(queue_);
    }

   private:
    std::shared_ptr<FileOperationQueue> queue_;
  };

  /**
   * Acquire an exclusive operation lease (FIFO when contended).
   * Signature: void(boost::system::error_code, Lease)
   */
  template <class CompletionToken>
  auto async_acquire(std::stop_token stop, CompletionToken&& token) {
    auto self = shared_from_this();
    auto initiation =
        [self, stop = std::move(stop)](auto handler) mutable {
          asio::dispatch(
              self->ex_,
              [self,
               stop = std::move(stop),
               handler = std::move(handler)]() mutable {
                Completion complete{
                    [handler = std::move(handler)](
                        boost::system::error_code ec,
                        Lease lease) mutable {
                      handler(ec, std::move(lease));
                    }};

                if (!self->leased_) {
                  if (stop.stop_requested()) {
                    asio::post(
                        self->ex_,
                        [complete = std::move(complete)]() mutable {
                          std::move(complete)(
                              asio::error::operation_aborted, Lease{});
                        });
                    return;
                  }
                  self->leased_ = true;
                  Lease lease{self};
                  asio::post(
                      self->ex_,
                      [complete = std::move(complete),
                       lease = std::move(lease)]() mutable {
                        std::move(complete)(
                            boost::system::error_code{}, std::move(lease));
                      });
                  return;
                }

                // Queued waiter: no strong queue ref inside Entry.
                auto entry = std::make_shared<Entry>(
                    ++self->next_id_, std::move(complete));

                if (stop.stop_possible()) {
                  // stop_callback: post only; weak + executor; no shared_ptr.
                  entry->stop_cb.emplace(
                      stop,
                      StopNotify{
                          .queue = self,
                          .executor = self->ex_,
                          .waiter_id = entry->id,
                      });
                }

                if (stop.stop_requested()) {
                  // On executor (dispatch), not on stop_callback stack.
                  entry->stop_cb.reset();
                  Completion aborted = std::move(entry->complete);
                  asio::post(
                      self->ex_, [aborted = std::move(aborted)]() mutable {
                        std::move(aborted)(
                            asio::error::operation_aborted, Lease{});
                      });
                  return;
                }

                self->waiters_.push_back(std::move(entry));
              });
        };

    return asio::async_initiate<CompletionToken,
                                void(boost::system::error_code, Lease)>(
        std::move(initiation), token);
  }

  [[nodiscard]] asio::any_io_executor executor() const { return ex_; }

 private:
  using Completion = std::move_only_function<
      void(boost::system::error_code, Lease)>;

  struct StopNotify {
    std::weak_ptr<FileOperationQueue> queue;
    asio::any_io_executor executor;
    std::uint64_t waiter_id;

    void operator()() const noexcept {
      asio::post(executor, [weak = queue, id = waiter_id]() noexcept {
        if (auto q = weak.lock()) {
          q->cancel_waiter(id);
        }
      });
    }
  };

  struct Entry {
    Entry(std::uint64_t waiter_id, Completion completion)
        : id(waiter_id), complete(std::move(completion)) {}

    std::uint64_t id;
    // No shared_ptr<FileOperationQueue> here (avoids
    // queue → waiters_ → Entry → queue).
    Completion complete;
    // Destroyed only on the queue executor.
    std::optional<std::stop_callback<StopNotify>> stop_cb;
  };

  void release() {
    auto self = shared_from_this();
    asio::dispatch(ex_, [self]() {
      if (!self->waiters_.empty()) {
        std::shared_ptr<Entry> next = std::move(self->waiters_.front());
        self->waiters_.pop_front();
        // Clear stop_cb on executor (not inside stop_callback).
        next->stop_cb.reset();
        Completion complete = std::move(next->complete);
        Lease lease{self};
        // Remain leased for the new holder.
        asio::post(
            self->ex_,
            [complete = std::move(complete),
             lease = std::move(lease)]() mutable {
              std::move(complete)(
                  boost::system::error_code{}, std::move(lease));
            });
        return;
      }
      self->leased_ = false;
    });
  }

  void cancel_waiter(std::uint64_t id) {
    // Invoked only via posted work on the queue executor.
    for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
      if ((*it)->id != id) {
        continue;
      }
      (*it)->stop_cb.reset();
      Completion complete = std::move((*it)->complete);
      waiters_.erase(it);
      asio::post(ex_, [complete = std::move(complete)]() mutable {
        std::move(complete)(asio::error::operation_aborted, Lease{});
      });
      return;
    }
  }

  asio::any_io_executor ex_;
  bool leased_{false};
  std::uint64_t next_id_{0};
  std::deque<std::shared_ptr<Entry>> waiters_;
};

}  // namespace vacps::js::detail
