#include "ssd_register_client.h"

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

    NoFSegment segment;
    segment.base = base;
    segment.size = size;
    segment.id = generate_uuid();
    segment.name = nqn;
    segment.te_endpoint = "traddr:" + traddr + " trsvcid:" + std::to_string(trsvcid) + " subnqn:" + nqn + " trtype:RDMA adrfam:IPv4" + " ns:" + std::to_string(nsid);
    auto mount_result = master_client_.MountNoFSegment(segment);
    if (!mount_result) {
        LOG(ERROR) << "mount_segment_to_master_failed ";
        return OPERATION_FAILED;
    }

    return OPERATION_OK;
  }

}
