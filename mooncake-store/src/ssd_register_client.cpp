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
    segment.te_endpoint = traddr + ":" + std::to_string(trsvcid) + ":" + std::to_string(nsid);
    auto mount_result = master_client_.MountNoFSegment(segment);
    if (!mount_result) {
        LOG(ERROR) << "mount_segment_to_master_failed ";
        return OPERATION_FAILED;
    }

    const std::string key = "sglang_mooncake_warmup_key" + segment.name;
    std::vector<size_t> slice_lengths = {10, 20 ,30};
    auto start_result = master_client_.PutStart(key, slice_lengths, ReplicateConfig{});
    if (!start_result) {
        ErrorCode err = start_result.error();
        if (err == ErrorCode::OBJECT_ALREADY_EXISTS) {
            VLOG(1) << "object_already_exists key=" << key;
            return {};
        }
        if (err == ErrorCode::NO_AVAILABLE_HANDLE) {
            LOG(WARNING) << "Failed to start put operation for key=" << key
                         << PUT_NO_SPACE_HELPER_STR;
        } else {
            LOG(ERROR) << "Failed to start put operation for key=" << key
                       << ": " << toString(err);
        }
        return OPERATION_FAILED;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto end_result = master_client_.PutEnd(key, ReplicaType::NOF_SSD);
    if (!end_result) {
        ErrorCode err = end_result.error();
        LOG(ERROR) << "Failed to end put operation: " << err;
        return OPERATION_FAILED;
    }

    return OPERATION_OK;
  }

}
