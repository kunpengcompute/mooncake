#include "ssd_register_client.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mooncake {

NoFRegisterClient::NoFRegisterClient(): master_client_(generate_uuid(), nullptr) {}

NoFRegisterClient::~NoFRegisterClient() = default;

int NoFRegisterClient::set_register(
    const std::string &nqn,
    size_t nsid,
    const std::string &traddr, 
    size_t trsvcid,
    uintptr_t base,
    size_t size,
    const std::string &master_server_addr) {
    
    LOG(INFO) << "Registering SSD: nqn=" << nqn
              << ",nsid=" << nsid
              << ",traddr=" << traddr
              << ",trsvcid=" << trsvcid
              << ",master=" << master_server_addr
              << ",base=" << base
              << ",size=" << size;

    auto err = master_client_.Connect(master_server_addr);
    if (err != ErrorCode::OK) {
        LOG(ERROR) << "Failed to connect to master";
        return OPERATION_FAILED;
    }

    const char *trtype_env = std::getenv("MC_NOF_TRTYPE");
    std::string trtype = trtype_env ? trtype_env : "RDMA";
    std::transform(trtype.begin(), trtype.end(), trtype.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (trtype != "RDMA" && trtype != "TCP") {
        LOG(WARNING) << "Invalid MC_NOF_TRTYPE=" << trtype
                     << ", fallback to RDMA";
        trtype = "RDMA";
    }

    NoFSegment segment;
    segment.base = base;
    segment.size = size;
    segment.id = generate_uuid();
    segment.name = nqn;
    segment.te_endpoint = "traddr:" + traddr + " trsvcid:" + std::to_string(trsvcid) +
                          " subnqn:" + nqn + " trtype:" + trtype +
                          " adrfam:IPv4 ns:" + std::to_string(nsid);
    auto mount_result = master_client_.MountNoFSegment(segment);
    if (!mount_result) {
        LOG(ERROR) << "mount_segment_to_master_failed ";
        return OPERATION_FAILED;
    }

    return OPERATION_OK;
  }


int NoFRegisterClient::set_unregister_by_endpoint(const std::string &nqn, size_t nsid, const std::string &traddr, size_t trsvcid, const std::string &master_server_addr) {
    LOG(INFO) << "Unregistering SSD by endpoint: nqn=" << nqn
              << ",nsid=" << nsid
              << ",traddr=" << traddr
              << ",trsvcid=" << trsvcid
              << ",master=" << master_server_addr;

    // Connect to master server
    auto err = master_client_.Connect(master_server_addr);
    if (err != ErrorCode::OK) {
        LOG(ERROR) << "Failed to connect to master: " << static_cast<int>(err);
        return OPERATION_FAILED;
    }

    // Build the te_endpoint string to match registered segments
    std::string trtype = "RDMA"; // Default transport type
    std::string te_endpoint = "traddr:" + traddr + " trsvcid:" + std::to_string(trsvcid) +
                              " subnqn:" + nqn + " trtype:" + trtype +
                              " adrfam:IPv4 ns:" + std::to_string(nsid);

    LOG(INFO) << "Built te_endpoint: " << te_endpoint;

    // Get all mounted NoF segments from master
    auto all_segments_result = master_client_.GetAllNoFSegments();
    if (!all_segments_result) {
        LOG(ERROR) << "Failed to get all NoF segments: " << static_cast<int>(all_segments_result.error());
        return OPERATION_FAILED;
    }

    std::vector<NoFSegment> all_segments = all_segments_result.value();
    LOG(INFO) << "Retrieved " << all_segments.size() << " mounted NoF segments";

    // Find segments that match our te_endpoint
    std::vector<UUID> matching_segments;
    for (const auto& segment : all_segments) {
        if (segment.te_endpoint == te_endpoint) {
            matching_segments.push_back(segment.id);
            LOG(INFO) << "Found matching segment: id=" << segment.id << ", te_endpoint=" << segment.te_endpoint;
        }
    }

    if (matching_segments.empty()) {
        LOG(ERROR) << "No segment found for te_endpoint: " << te_endpoint;
        return OPERATION_FAILED;
    }

    // Unmount all matching segments
    bool all_unmounted = true;
    for (const auto& segment_id : matching_segments) {
        auto unmount_result = master_client_.UnmountNoFSegment(segment_id);
        if (!unmount_result) {
            LOG(ERROR) << "Failed to unmount segment " << segment_id << ": " << static_cast<int>(unmount_result.error());
            all_unmounted = false;
        } else {
            LOG(INFO) << "Successfully unmounted segment " << segment_id;
        }
    }

    return all_unmounted ? OPERATION_OK : OPERATION_FAILED;
}

}
