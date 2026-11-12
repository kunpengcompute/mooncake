#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <ylt/util/tl/expected.hpp>

#include "client_buffer.h"

namespace mooncake {

// The owner refers to the complete pool allocation, including alignment gaps.
// data is the aligned base and logical_size is the object's payload length;
// NofStagedTransfer supplies the actual NoF slices when the layout has gaps.
struct NofStagingBuffer {
    BufferHandle owner;
    char* data;
    size_t logical_size;
};

namespace nof_staging_detail {

inline tl::expected<size_t, ErrorCode> AllocationSize(size_t length,
                                                      size_t alignment) {
    if (length == 0 || alignment == 0 ||
        length > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    return length + (alignment - 1);
}

// Separate allocation from address validation so integer boundaries and owner
// lifetime can be tested without a running SPDK environment.
inline tl::expected<NofStagingBuffer, ErrorCode> AlignAllocation(
    BufferHandle owner, size_t length, size_t alignment) {
    if (length == 0 || alignment == 0 || owner.ptr() == nullptr ||
        owner.size() < length) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    const auto base = reinterpret_cast<uintptr_t>(owner.ptr());
    const size_t padding = (alignment - base % alignment) % alignment;
    const auto max_address = std::numeric_limits<uintptr_t>::max();
    if (padding > owner.size() - length || padding > max_address - base) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    const auto aligned = base + padding;
    if (length > max_address - aligned) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    char* data = reinterpret_cast<char*>(aligned);
    return NofStagingBuffer{std::move(owner), data, length};
}

}  // namespace nof_staging_detail

inline tl::expected<NofStagingBuffer, ErrorCode> AcquireNofStaging(
    const std::shared_ptr<ClientBufferAllocator>& pool, size_t length,
    size_t alignment) {
    if (!pool || !pool->isSpdkHostDma()) {
        return tl::unexpected(ErrorCode::INVALID_PARAMS);
    }
    auto allocation_size =
        nof_staging_detail::AllocationSize(length, alignment);
    if (!allocation_size) {
        return tl::unexpected(allocation_size.error());
    }
    auto owner = pool->allocate(*allocation_size);
    if (!owner) {
        return tl::unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    }
    return nof_staging_detail::AlignAllocation(std::move(*owner), length,
                                               alignment);
}

}  // namespace mooncake
