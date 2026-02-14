#include "ssd_register_client.h"

namespace mooncake {

SSDRegisterClient::SSDRegisterClient(): master_client_(generate_uuid(), nullptr) {}

SSDRegisterClient::~SSDRegisterClient() = default;

int SSDRegisterClient::set_register(
    const std::string &nqn,
    size_t nsid,
    const std::string &traddr, 
    size_t trsvcid,
    const std::string &master_server_addr) {
    
    LOG(INFO) << "Registering SSD: nqn=" << nqn
              << ",nsid=" << nsid
              << ",traddr=" << traddr
              << ",trsvcid=" << trsvcid
              << ",master=" << master_server_addr;

    auto err = master_client_.Connect(master_server_addr);
    if (err != ErrorCode::OK) {
        LOG(ERROR) << "Failed to connect to master";
        return OPERATION_FAILED;
    }

    NoFSegment segment;
    segment.base = 16777216;
    segment.size = 16777216;
    segment.id = generate_uuid();
    segment.name = nqn;
    segment.te_endpoint = traddr + ":" + std::to_string(trsvcid) + ":" + std::to_string(nsid);
    auto mount_result = master_client_.MountNoFSegment(segment);
    if (!mount_result) {
        LOG(ERROR) << "mount_segment_to_master_failed ";
        return OPERATION_FAILED;
    }

    return OPERATION_OK;
  }

}
