#pragma once

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace mooncake {

// One notification per submitted NoF object. Allocate before submitting I/O so
// the SPDK completion thread only publishes an index and read completion time,
// never allocates or runs device copies. The BatchGet caller restores objects.
class NofReadCompletionQueue {
   public:
    struct Completion {
        size_t index;
        std::chrono::steady_clock::time_point completed_at;
    };

    explicit NofReadCompletionQueue(size_t capacity) : completions_(capacity) {}

    void Push(size_t index,
              std::chrono::steady_clock::time_point completed_at) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            assert(written_ < completions_.size());
            completions_[written_++] = {index, completed_at};
        }
        cv_.notify_one();
    }

    Completion Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return read_ != written_; });
        return completions_[read_++];
    }

   private:
    std::vector<Completion> completions_;
    size_t written_ = 0;
    size_t read_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace mooncake
