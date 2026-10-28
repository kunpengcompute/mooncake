#pragma once

#include <algorithm>
#include <cstdint>

namespace mooncake {

// Zero selects the legacy/default thread count, including when a configuration
// file omits rpc_thread_num. A zero-thread handler pool cannot execute
// requests.
inline uint32_t ResolveMasterRpcThreadCount(uint32_t configured_threads,
                                            int32_t command_line_threads,
                                            int32_t legacy_max_threads,
                                            uint32_t hardware_threads) {
    if (command_line_threads > 0) {
        return static_cast<uint32_t>(command_line_threads);
    }
    if (configured_threads > 0) {
        return configured_threads;
    }
    return std::min(static_cast<uint32_t>(std::max(1, legacy_max_threads)),
                    std::max(1u, hardware_threads));
}

}  // namespace mooncake
