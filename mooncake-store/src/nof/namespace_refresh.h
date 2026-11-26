#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace mooncake {

struct NofNamespaceRefreshResult {
    bool active = false;
    int32_t admin_error = 0;
    uint64_t poll_count = 0;
};

// The callbacks synchronize controller access. Force an admin poll at least
// once for an inactive namespace, even when the configured timeout is zero.
template <typename IsActive, typename PollAdmin>
NofNamespaceRefreshResult RefreshNofNamespace(
    IsActive is_active, PollAdmin poll_admin, std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval) {
    NofNamespaceRefreshResult result;
    if (is_active()) {
        result.active = true;
        return result;
    }
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        ++result.poll_count;
        result.admin_error = poll_admin();
        if (result.admin_error < 0) return result;
        result.admin_error = 0;
        if (is_active()) {
            result.active = true;
            return result;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        if (elapsed >= timeout) return result;
        if (poll_interval.count() > 0) {
            std::this_thread::sleep_for(
                std::min(poll_interval, timeout - elapsed));
        }
    }
}

}  // namespace mooncake
