#pragma once

#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>

namespace mooncake::nof_staging {

// Padding belongs to the host allocation only. Callers submit the individual
// payload ranges as an SGL so the stored object remains tightly packed.
class HostLayout {
   public:
    std::optional<size_t> Append(size_t size, size_t alignment) {
        if (size == 0 || alignment == 0) return std::nullopt;
        const size_t padding = (alignment - span_ % alignment) % alignment;
        const auto maximum = std::numeric_limits<size_t>::max();
        if (padding > maximum - span_) return std::nullopt;
        const size_t offset = span_ + padding;
        if (size > maximum - offset) return std::nullopt;
        span_ = offset + size;
        return offset;
    }

    size_t span() const { return span_; }

   private:
    size_t span_ = 0;
};

// SPDK and ACL can impose different alignment requirements on the same base.
inline std::optional<size_t> CommonAlignment(size_t first, size_t second) {
    if (first == 0 || second == 0) return std::nullopt;
    const size_t factor = first / std::gcd(first, second);
    if (factor > std::numeric_limits<size_t>::max() / second) {
        return std::nullopt;
    }
    return factor * second;
}

}  // namespace mooncake::nof_staging
