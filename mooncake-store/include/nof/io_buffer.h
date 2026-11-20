#pragma once
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mooncake {

// External host buffers are registered with SPDK one page at a time.
inline constexpr uintptr_t kNofRegistrationPageSize = 2ULL << 20;

inline size_t NofBytesToRegistrationPageEnd(const void *address) {
    return kNofRegistrationPageSize -
           reinterpret_cast<uintptr_t>(address) % kNofRegistrationPageSize;
}

struct NofBuffer {
    void *data;
    size_t size;
};
// Addresses belong to the caller. The NoF request owns this cursor until its
// completion; the caller keeps the corresponding buffers alive until then.
struct NofSglCursor {
    std::vector<NofBuffer> buffers;
    size_t start{0}, offset{0}, length{0}, position{0};
    bool split_at_registration_page{false};
    void Reset(uint32_t value) {
        offset = start + value;
        position = value;
    }
    int Next(void **address, uint32_t *size) {
        if (!address || !size || position >= length) return -EINVAL;
        size_t skip = offset;
        for (auto &v : buffers) {
            if (skip >= v.size) {
                skip -= v.size;
                continue;
            }
            size_t take = std::min({v.size - skip, length - position,
                                    static_cast<size_t>(UINT32_MAX)});
            *address = static_cast<char *>(v.data) + skip;
            if (split_at_registration_page) {
                take = std::min(take, NofBytesToRegistrationPageEnd(*address));
            }
            *size = static_cast<uint32_t>(take);
            offset += take;
            position += take;
            return 0;
        }
        return -EINVAL;
    }
};
}  // namespace mooncake
