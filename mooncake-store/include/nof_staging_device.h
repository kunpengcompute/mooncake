#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

namespace mooncake::nof_staging {

inline constexpr size_t kAscendCopyAlignment = 64;

enum class Backend { Unsupported, Cuda, Ascend };

struct Device {
    Backend backend = Backend::Unsupported;
    int id = -1;
    // Ascend's application-owned context. CUDA uses its runtime primary
    // context. The caller must keep the context, like the registered
    // allocation, alive.
    void* context = nullptr;
    uint32_t memory_location_id = 0;

    bool operator==(const Device& other) const {
        return backend == other.backend && id == other.id &&
               context == other.context &&
               memory_location_id == other.memory_location_id;
    }
};

inline std::optional<bool> ParseEnableFlag(const char* value) {
    if (value == nullptr || std::string_view(value) == "0") return false;
    if (std::string_view(value) == "1") return true;
    return std::nullopt;
}

inline bool ValidRange(uintptr_t base, size_t size) {
    return base != 0 && size != 0 &&
           size <= std::numeric_limits<uintptr_t>::max() - base;
}

// Records addresses only: this class neither owns accelerator allocations nor
// registers them with Transfer Engine/SPDK. The owner provides synchronization.
class RegionRegistry {
   public:
    bool Insert(const void* ptr, size_t size, const Device& device) {
        const auto base = reinterpret_cast<uintptr_t>(ptr);
        if (!ValidRange(base, size) || device.id < 0 ||
            device.backend == Backend::Unsupported) {
            return false;
        }
        auto next = regions_.lower_bound(base);
        if (next != regions_.end() && next->first == base) {
            return next->second.size == size && next->second.device == device;
        }
        if (next != regions_.end() && size > next->first - base) return false;
        if (next != regions_.begin()) {
            auto previous = std::prev(next);
            if (base - previous->first < previous->second.size) return false;
        }
        regions_.emplace(base, Region{size, device});
        return true;
    }

    std::optional<Device> Find(const void* ptr, size_t size) const {
        const auto base = reinterpret_cast<uintptr_t>(ptr);
        if (!ValidRange(base, size)) return std::nullopt;
        auto next = regions_.upper_bound(base);
        if (next == regions_.begin()) return std::nullopt;
        const auto region = std::prev(next);
        const auto offset = base - region->first;
        if (offset >= region->second.size ||
            size > region->second.size - offset) {
            return std::nullopt;
        }
        return region->second.device;
    }

    bool HasExact(const void* ptr, size_t size, const Device& device) const {
        auto region = regions_.find(reinterpret_cast<uintptr_t>(ptr));
        return region != regions_.end() && region->second.size == size &&
               region->second.device == device;
    }

    bool Erase(const void* base) {
        return regions_.erase(reinterpret_cast<uintptr_t>(base)) != 0;
    }
    void Clear() { regions_.clear(); }
    size_t size() const { return regions_.size(); }

   private:
    struct Region {
        size_t size;
        Device device;
    };
    std::map<uintptr_t, Region> regions_;
};

// Fail closed on unknown/host/managed memory or an unsupported build backend.
// Endpoint attributes are checked, not every page of an arbitrary VA range.
// Ascend uses the legacy pointer device-ID convention and requires the current
// device to match the range before borrowing the application's context.
// The caller must supply one live, contiguous allocation with no holes, keep it
// alive through unregister, and not write it concurrently with a PUT copy.
std::optional<Device> DetectDeviceRange(const void* ptr, size_t size);

// Synchronous: success includes copy completion and checked context
// restoration. Host memory must come from a verified SPDK DMA pool, either
// the original client pool or the dedicated lazy NoF staging pool.
// Ascend callers must finish producing source data and exclude concurrent
// access to destination buffers before entering these functions. They only
// synchronize their own copy stream, not the application's producer streams.
bool CopyToHost(void* host, const void* device_ptr, size_t size,
                const Device& device);
bool CopyFromHost(void* device_ptr, const void* host, size_t size,
                  const Device& device);

// Borrows storage for one object's copies; a null device denotes a host range
// already validated by the caller. Slice order is preserved.
struct CopySlice {
    void* dst;
    const void* src;
    size_t size;
    std::optional<Device> device;
};

// One caller-owned batch. Ascend streams are created lazily and reused by
// Device/Context; each Copy still waits for its own bytes before returning.
// The session is not shared between threads and must end before its borrowed
// application contexts. Finish reports cleanup errors and closes the session.
class CopySession {
   public:
    CopySession();
    ~CopySession();
    CopySession(const CopySession&) = delete;
    CopySession& operator=(const CopySession&) = delete;

    bool Copy(const CopySlice* slices, size_t count, bool to_host);
    bool Finish();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool finished_ = false;
    bool finish_result_ = true;
};

// Synchronous, with the same completion/context guarantees as CopyToHost and
// CopyFromHost. Uses a standalone session; batch callers reuse CopySession.
// Ascend slices in each Device/Context group share a stream and synchronize
// before returning. A host slice also waits for preceding device copies to
// preserve slice ordering.
// Ascend copy endpoints must be 64-byte aligned. The caller lays out Host
// slices before invoking Copy; logical lengths need not be a multiple of 64.
// CUDA retains one device readiness fence and one completion fence per group.
// Host-only copies need no device synchronization.
bool Copy(const CopySlice* slices, size_t count, bool to_host);

}  // namespace mooncake::nof_staging
