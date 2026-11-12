#pragma once

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "nof/dma_buffer_allocator.h"

namespace mooncake {

// One immutable zero slot for writes and exclusive discard slots for reads.
// Owners keep both the pool and its allocator alive until the I/O is drained.
class NofPaddingPool : public std::enable_shared_from_this<NofPaddingPool> {
   public:
    explicit NofPaddingPool(std::shared_ptr<DmaBufferAllocator> allocator)
        : allocator_(std::move(allocator)) {}

    ~NofPaddingPool() { allocator_->Free(slab_); }

    int Acquire(size_t size, bool for_write, void** ptr,
                std::shared_ptr<void>* owner) {
        if (!ptr || !owner || !size) return -EINVAL;
        std::unique_lock<std::mutex> lock(mutex_);
        if (!slab_) {
            // Covers standard 512-byte and 4-KiB namespace blocks. Allocate
            // only on the first non-block-aligned object.
            free_slots_.reserve(kReadSlots);
            slab_ =
                allocator_->Alloc(kSlotSize * (kReadSlots + 1), kSlotSize, -1);
            if (!slab_) return -ENOMEM;
            std::memset(slab_, 0, kSlotSize);
            for (size_t i = 1; i <= kReadSlots; ++i) free_slots_.push_back(i);
        }
        if (size > kSlotSize) return -E2BIG;
        if (for_write) {
            lock.unlock();
            *owner = std::shared_ptr<void>(shared_from_this(), slab_);
            *ptr = slab_;
            return 0;
        }
        if (free_slots_.empty()) return -EAGAIN;
        const size_t slot = free_slots_.back();
        void* address = static_cast<char*>(slab_) + slot * kSlotSize;
        free_slots_.pop_back();
        lock.unlock();
        // shared_ptr calls the deleter if its control-block allocation fails,
        // returning the borrowed slot without holding the pool lock.
        auto allocation = std::shared_ptr<void>(
            address, [pool = shared_from_this(), slot](void*) {
                std::lock_guard<std::mutex> guard(pool->mutex_);
                pool->free_slots_.push_back(slot);
            });
        *owner = std::move(allocation);
        *ptr = address;
        return 0;
    }

   private:
    static constexpr size_t kSlotSize = 4096;
    static constexpr size_t kReadSlots = 1024;
    std::shared_ptr<DmaBufferAllocator> allocator_;
    void* slab_ = nullptr;
    std::mutex mutex_;
    std::vector<size_t> free_slots_;
};

}  // namespace mooncake
