#include <gtest/gtest.h>
#include "transport/rdma_transport/ascend_storage_config.h"
using namespace mooncake;
namespace {
Json::Value Config() {
    Json::Value root;
    root["protocol"] = "rdma";
    root["ascend_transfer"]["mode"] = "data_staging";
    auto &plane = root["data_plane"];
    plane["schema_version"] = 1;
    plane["network"] = "rdma";
    plane["strict_route"] = true;
    plane["devices"]["7"]["rdma_device"] = "hns_data_7";
    plane["devices"]["7"]["rdma_ip"] = "192.0.2.17";
    return root;
}
}  // namespace
TEST(AscendStorageConfig, Legacy) {
    AscendTransferMode mode;
    EXPECT_EQ(
        AscendStorageConfig::ParseMode(Json::Value(Json::objectValue), mode),
        0);
    EXPECT_EQ(mode, AscendTransferMode::Legacy);
}
TEST(AscendStorageConfig, PhysicalMappingAndInactivePlane) {
    auto root = Config();
    root["parameter_plane"] = "invalid but inactive";
    AscendStorageDevice device;
    EXPECT_EQ(AscendStorageConfig::ParseDevice(
                  root, 7, AscendTransferMode::DataStaging, device),
              0);
    EXPECT_EQ(device.physical_device_id, 7);
    EXPECT_EQ(device.rdma_device, "hns_data_7");
    EXPECT_NE(AscendStorageConfig::ParseDevice(
                  root, 0, AscendTransferMode::DataStaging, device),
              0);
}
TEST(AscendStorageConfig, DirectCompatibility) {
    auto root = Config();
    root.removeMember("ascend_transfer");
    root["parameter_plane"] = root["data_plane"];
    root.removeMember("data_plane");
    root["parameter_plane"]["memory_registration"] = "ub";
    AscendTransferMode mode;
    AscendStorageDevice device;
    ASSERT_EQ(AscendStorageConfig::ParseMode(root, mode), 0);
    EXPECT_EQ(mode, AscendTransferMode::ParameterDirect);
    EXPECT_EQ(AscendStorageConfig::ParseDevice(root, 7, mode, device), 0);
}
TEST(AscendStorageConfig, ExplicitDirectConfiguration) {
    auto root = Config();
    root["ascend_transfer"]["mode"] = "parameter_direct";
    root["parameter_plane"] = root["data_plane"];
    root.removeMember("data_plane");
    root["parameter_plane"]["memory_registration"] = "ub";
    AscendTransferMode mode;
    AscendStorageDevice device;
    ASSERT_EQ(AscendStorageConfig::ParseMode(root, mode), 0);
    ASSERT_EQ(mode, AscendTransferMode::ParameterDirect);
    ASSERT_EQ(AscendStorageConfig::ParseDevice(root, 7, mode, device), 0);
    EXPECT_EQ(device.rdma_device, "hns_data_7");
    EXPECT_EQ(device.rdma_ip, "192.0.2.17");

    root["parameter_plane"]["devices"]["7"].removeMember("rdma_ip");
    EXPECT_NE(AscendStorageConfig::ParseDevice(root, 7, mode, device), 0);
}
TEST(AscendStorageConfig, RejectAmbiguousOrInvalidModes) {
    auto root = Config();
    AscendTransferMode mode;
    root.removeMember("ascend_transfer");
    EXPECT_NE(AscendStorageConfig::ParseMode(root, mode), 0);
    root["ascend_transfer"]["mode"] = "fallback";
    EXPECT_NE(AscendStorageConfig::ParseMode(root, mode), 0);
    root["ascend_transfer"]["mode"] = "parameter_direct";
    EXPECT_NE(AscendStorageConfig::ParseMode(root, mode), 0);
}
TEST(AscendStorageConfig, StrictSchemaTypesAndProtocol) {
    AscendTransferMode mode;
    for (auto value : {Json::Value("1"), Json::Value(true), Json::Value(2)}) {
        auto root = Config();
        root["data_plane"]["schema_version"] = value;
        EXPECT_NE(AscendStorageConfig::ParseMode(root, mode), 0);
    }
    auto root = Config();
    root["data_plane"]["strict_route"] = false;
    EXPECT_NE(AscendStorageConfig::ParseMode(root, mode), 0);
    root = Config();
    root["protocol"] = "ascend";
    EXPECT_NE(AscendStorageConfig::ParseMode(root, mode), 0);
}
TEST(AscendStorageConfig, RejectRouteConflictAndBadAddress) {
    auto root = Config();
    AscendStorageDevice device;
    root["device_name"] = "other_nic";
    EXPECT_NE(AscendStorageConfig::ParseDevice(
                  root, 7, AscendTransferMode::DataStaging, device),
              0);
    root["device_name"] = "hns_data_7";
    EXPECT_EQ(AscendStorageConfig::ParseDevice(
                  root, 7, AscendTransferMode::DataStaging, device),
              0);
    root["data_plane"]["devices"]["7"]["rdma_ip"] = "bad";
    EXPECT_NE(AscendStorageConfig::ParseDevice(
                  root, 7, AscendTransferMode::DataStaging, device),
              0);
}

#include "transport/rdma_transport/ascend_parameter_plane.h"
TEST(AscendMrRange, ReusesRegionForDifferentSmallRequests) {
    uintptr_t first = 0, second = 0;
    size_t a = 0, b = 0;
    ASSERT_TRUE(mooncake::AscendParameterPlane::GetMrRange(
        4096, 16384, 8192, 512, 65536, first, a));
    ASSERT_TRUE(mooncake::AscendParameterPlane::GetMrRange(
        4096, 16384, 12288, 1024, 65536, second, b));
    EXPECT_EQ(first, 4096);
    EXPECT_EQ(second, first);
    EXPECT_EQ(a, 16384);
    EXPECT_EQ(b, a);
}
TEST(AscendMrRange, SplitsLargeRegionsAndCoversBoundaryCrossingRequests) {
    uintptr_t base = 0;
    size_t length = 0;
    ASSERT_TRUE(mooncake::AscendParameterPlane::GetMrRange(
        4096, 16384, 12288, 512, 4096, base, length));
    EXPECT_EQ(base, 12288);
    EXPECT_EQ(length, 4096);
    ASSERT_TRUE(mooncake::AscendParameterPlane::GetMrRange(
        4096, 16384, 8000, 512, 4096, base, length));
    EXPECT_EQ(base, 8000);
    EXPECT_EQ(length, 512);
    EXPECT_FALSE(mooncake::AscendParameterPlane::GetMrRange(
        4096, 16384, 8000, 8192, 4096, base, length));
    EXPECT_FALSE(mooncake::AscendParameterPlane::GetMrRange(
        4096, 16384, 20480, 512, 4096, base, length));
}
