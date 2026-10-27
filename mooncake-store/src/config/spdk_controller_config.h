#pragma once

#include <cstdint>
#include <optional>

namespace mooncake {

struct SpdkControllerConfig {
    std::optional<uint32_t> num_io_queues;
    std::optional<uint32_t> io_queue_size;
    std::optional<uint32_t> io_queue_requests;
    std::optional<uint8_t> transport_ack_timeout;
    std::optional<uint16_t> admin_queue_size;
    std::optional<uint64_t> fabrics_connect_timeout_us;
    std::optional<bool> header_digest;
    std::optional<bool> data_digest;
    uint64_t nof_admin_poll_interval_ms = 100;
    uint64_t nof_namespace_refresh_timeout_ms = 1000;
    uint64_t nof_namespace_refresh_poll_interval_ms = 10;

    static SpdkControllerConfig FromEnvironment();
};

}  // namespace mooncake
