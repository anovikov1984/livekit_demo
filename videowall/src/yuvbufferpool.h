#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

// Size-keyed free-list of std::vector<uint8_t> buffers, used to recycle the
// per-plane allocations on the video hot path. acquire(n) returns an
// implicitly-shared `shared_ptr<const vector<uint8_t>>`; when the last
// reference is dropped, the custom deleter pushes the vector back into the
// pool instead of freeing it.
class YuvBufferPool : public std::enable_shared_from_this<YuvBufferPool> {
public:
  using PlaneBuffer = std::shared_ptr<std::vector<std::uint8_t>>;

  // Returns a writable buffer of exactly `n` bytes. Caller fills it, then
  // typically transfers it into a `shared_ptr<const ...>` via
  // `std::const_pointer_cast` for the cross-thread queued-connection payload.
  PlaneBuffer acquire(std::size_t n) {
    std::unique_ptr<std::vector<std::uint8_t>> buf;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      auto &bucket = free_[n];
      if (!bucket.empty()) {
        buf = std::move(bucket.back());
        bucket.pop_back();
      }
    }
    if (!buf) {
      buf = std::make_unique<std::vector<std::uint8_t>>(n);
    }

    auto *raw = buf.release();
    std::weak_ptr<YuvBufferPool> weak = weak_from_this();
    auto deleter = [weak](std::vector<std::uint8_t> *p) {
      std::unique_ptr<std::vector<std::uint8_t>> owned(p);
      if (auto pool = weak.lock()) {
        pool->release(std::move(owned));
      }
    };
    return PlaneBuffer(raw, std::move(deleter));
  }

private:
  static constexpr std::size_t kMaxFreePerSize = 4;

  void release(std::unique_ptr<std::vector<std::uint8_t>> buf) {
    const std::size_t n = buf->size();
    std::lock_guard<std::mutex> lk(mutex_);
    auto &bucket = free_[n];
    if (bucket.size() < kMaxFreePerSize) {
      bucket.push_back(std::move(buf));
    }
  }

  std::mutex mutex_;
  std::unordered_map<std::size_t,
                     std::vector<std::unique_ptr<std::vector<std::uint8_t>>>>
      free_;
};
