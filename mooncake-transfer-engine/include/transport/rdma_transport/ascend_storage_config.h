// Copyright 2026 Mooncake Authors
// Licensed under the Apache License, Version 2.0.
#pragma once
#include <json/json.h>
#include <cstdint>
#include <mutex>
#include <string>

namespace mooncake {
enum class AscendTransferMode { Legacy, ParameterDirect, DataStaging };
struct AscendStorageDevice {
    int logical_device_id{-1};
    int physical_device_id{-1};
    std::string eid;
    std::string rdma_device;
    std::string rdma_ip;
    uint8_t rdma_port{0};
    int gid_index{-1};
    bool strict_route{true};
};

// Immutable after initialization. Used only by Store/RDMA, never by P/D Ascend.
class AscendStorageConfig {
   public:
    static AscendStorageConfig &GetInstance();
    int InitializeFromEnvironment();
    AscendTransferMode Mode() const { return mode_; }
    bool HasRoute() const { return mode_ != AscendTransferMode::Legacy; }
    const AscendStorageDevice &Device() const { return device_; }

    // Parsing has no hardware side effects, allowing configuration validation
    // before selecting a worker. Only the selected plane is inspected.
    static int ParseMode(const Json::Value &root, AscendTransferMode &mode);
    static int ParseDevice(const Json::Value &root, int physical_id,
                           AscendTransferMode mode,
                           AscendStorageDevice &device);

   private:
    int Initialize();
    std::once_flag once_;
    int result_{0};
    AscendTransferMode mode_{AscendTransferMode::Legacy};
    AscendStorageDevice device_;
};
}  // namespace mooncake
