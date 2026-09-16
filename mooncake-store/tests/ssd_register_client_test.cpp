#include <cstdlib>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ssd_register_client.h"

namespace mooncake {
namespace {

struct MockMasterClientState {
    std::deque<ErrorCode> connect_results;
    tl::expected<void, ErrorCode> mount_result{};
    tl::expected<std::vector<NoFSegmentOwnerInfo>, ErrorCode> lookup_result{
        std::vector<NoFSegmentOwnerInfo>{}};
    std::deque<tl::expected<void, ErrorCode>> unmount_results;
    std::vector<std::string> connected_addresses;
    std::vector<NoFSegment> mounted_segments;
    std::vector<std::string> lookup_names;
    std::vector<UUID> unmount_requests;
};

MockMasterClientState* g_mock = nullptr;

ErrorCode NextConnectResult() {
    if (g_mock->connect_results.empty()) {
        return ErrorCode::OK;
    }
    auto result = g_mock->connect_results.front();
    g_mock->connect_results.pop_front();
    return result;
}

}  // namespace

extern "C" ErrorCode
WrapMasterClientConnect(MasterClient*, const std::string& address) asm(
    "__wrap__ZN8mooncake12MasterClient7ConnectERKNSt7__cxx1112basic_"
    "stringIcSt11char_traitsIcESaIcEEE");
extern "C" tl::expected<void, ErrorCode>
WrapMasterClientMountNoFSegment(MasterClient*, const NoFSegment& segment) asm(
    "__wrap__ZN8mooncake12MasterClient15MountNoFSegmentERKNS_10NoFSegmentE");
extern "C" tl::expected<std::vector<NoFSegmentOwnerInfo>, ErrorCode>
WrapMasterClientGetNoFSegmentsByName(MasterClient*, const std::string& name) asm(
    "__wrap__ZN8mooncake12MasterClient20GetNoFSegmentsByNameERKNSt7__"
    "cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
extern "C" tl::expected<void, ErrorCode>
WrapMasterClientUnmountNoFSegment(MasterClient*, const UUID& segment_id) asm(
    "__wrap__ZN8mooncake12MasterClient17UnmountNoFSegmentERKSt4pairImmE");

extern "C" ErrorCode WrapMasterClientConnect(MasterClient*,
                                             const std::string& address) {
    g_mock->connected_addresses.push_back(address);
    return NextConnectResult();
}

extern "C" tl::expected<void, ErrorCode> WrapMasterClientMountNoFSegment(
    MasterClient*, const NoFSegment& segment) {
    g_mock->mounted_segments.push_back(segment);
    return g_mock->mount_result;
}

extern "C" tl::expected<std::vector<NoFSegmentOwnerInfo>, ErrorCode>
WrapMasterClientGetNoFSegmentsByName(MasterClient*, const std::string& name) {
    g_mock->lookup_names.push_back(name);
    return g_mock->lookup_result;
}

extern "C" tl::expected<void, ErrorCode> WrapMasterClientUnmountNoFSegment(
    MasterClient*, const UUID& segment_id) {
    g_mock->unmount_requests.push_back(segment_id);
    if (g_mock->unmount_results.empty()) {
        return {};
    }
    auto result = g_mock->unmount_results.front();
    g_mock->unmount_results.pop_front();
    return result;
}

namespace {

class NoFRegisterClientTest : public ::testing::Test {
   protected:
    void SetUp() override { g_mock = &mock_; }

    void TearDown() override {
        unsetenv("MC_NOF_TRTYPE");
        g_mock = nullptr;
    }

    MockMasterClientState mock_;
};

TEST_F(NoFRegisterClientTest, RegisterBuildsRdmaEndpointAndMountsSegment) {
    NoFRegisterClient client;

    EXPECT_EQ(client.set_register("nqn.test", 3, "192.168.1.10", 4420, 4096,
                                  8192, "master:50051"),
              OPERATION_OK);
    ASSERT_EQ(mock_.connected_addresses,
              std::vector<std::string>({"master:50051"}));
    ASSERT_EQ(mock_.mounted_segments.size(), 1U);
    const auto& segment = mock_.mounted_segments.front();
    EXPECT_EQ(segment.base, 4096U);
    EXPECT_EQ(segment.size, 8192U);
    EXPECT_EQ(segment.name, segment.te_endpoint);
    EXPECT_EQ(segment.te_endpoint,
              "traddr:192.168.1.10 trsvcid:4420 subnqn:nqn.test "
              "trtype:RDMA adrfam:IPv4 ns:3");
}

TEST_F(NoFRegisterClientTest, RegisterUsesTcpAndFallsBackForInvalidTransport) {
    NoFRegisterClient client;

    setenv("MC_NOF_TRTYPE", "tcp", 1);
    EXPECT_EQ(client.set_register("nqn", 1, "host", 4420, 0, 1, "master"),
              OPERATION_OK);
    EXPECT_NE(mock_.mounted_segments.back().te_endpoint.find("trtype:TCP"),
              std::string::npos);

    setenv("MC_NOF_TRTYPE", "invalid", 1);
    EXPECT_EQ(client.set_register("nqn", 2, "host", 4420, 0, 1, "master"),
              OPERATION_OK);
    EXPECT_NE(mock_.mounted_segments.back().te_endpoint.find("trtype:RDMA"),
              std::string::npos);
}

TEST_F(NoFRegisterClientTest, RegisterPropagatesConnectAndMountFailures) {
    NoFRegisterClient client;
    mock_.connect_results.push_back(ErrorCode::RPC_FAIL);
    EXPECT_EQ(client.set_register("nqn", 1, "host", 4420, 0, 1, "master"),
              OPERATION_FAILED);
    EXPECT_TRUE(mock_.mounted_segments.empty());

    mock_.mount_result = tl::unexpected(ErrorCode::RPC_FAIL);
    EXPECT_EQ(client.set_register("nqn", 1, "host", 4420, 0, 1, "master"),
              OPERATION_FAILED);
}

TEST_F(NoFRegisterClientTest, UnregisterValidatesConnectAndLookupResult) {
    NoFRegisterClient client;
    mock_.connect_results.push_back(ErrorCode::RPC_FAIL);
    EXPECT_EQ(
        client.set_unregister_by_endpoint("nqn", 1, "host", 4420, "master"),
        OPERATION_FAILED);

    mock_.lookup_result = tl::unexpected(ErrorCode::RPC_FAIL);
    EXPECT_EQ(
        client.set_unregister_by_endpoint("nqn", 1, "host", 4420, "master"),
        OPERATION_FAILED);

    mock_.lookup_result = std::vector<NoFSegmentOwnerInfo>{};
    EXPECT_EQ(
        client.set_unregister_by_endpoint("nqn", 1, "host", 4420, "master"),
        OPERATION_FAILED);
}

TEST_F(NoFRegisterClientTest, UnregisterAttemptsEveryMatchingSegment) {
    NoFSegmentOwnerInfo first{};
    first.client_id = generate_uuid();
    first.segment_id = generate_uuid();
    NoFSegmentOwnerInfo second{};
    second.client_id = generate_uuid();
    second.segment_id = generate_uuid();
    mock_.lookup_result = std::vector<NoFSegmentOwnerInfo>{first, second};
    mock_.unmount_results.push_back(tl::expected<void, ErrorCode>{});
    mock_.unmount_results.push_back(tl::unexpected(ErrorCode::RPC_FAIL));
    NoFRegisterClient client;

    EXPECT_EQ(
        client.set_unregister_by_endpoint("nqn", 4, "host", 4420, "master"),
        OPERATION_FAILED);
    EXPECT_EQ(mock_.unmount_requests.size(), 2U);
    ASSERT_EQ(mock_.lookup_names.size(), 1U);
    EXPECT_EQ(mock_.lookup_names.front(),
              "traddr:host trsvcid:4420 subnqn:nqn trtype:RDMA "
              "adrfam:IPv4 ns:4");
}

TEST_F(NoFRegisterClientTest, UnregisterHandlesOwnerConnectFailure) {
    NoFSegmentOwnerInfo segment{};
    segment.client_id = generate_uuid();
    segment.segment_id = generate_uuid();
    mock_.lookup_result = std::vector<NoFSegmentOwnerInfo>{segment};
    mock_.connect_results.push_back(ErrorCode::OK);
    mock_.connect_results.push_back(ErrorCode::RPC_FAIL);
    NoFRegisterClient client;

    EXPECT_EQ(
        client.set_unregister_by_endpoint("nqn", 1, "host", 4420, "master"),
        OPERATION_FAILED);
    EXPECT_TRUE(mock_.unmount_requests.empty());
}

TEST_F(NoFRegisterClientTest, UnregisterSucceedsWhenAllSegmentsUnmount) {
    NoFSegmentOwnerInfo segment{};
    segment.client_id = generate_uuid();
    segment.segment_id = generate_uuid();
    mock_.lookup_result = std::vector<NoFSegmentOwnerInfo>{segment};
    NoFRegisterClient client;

    EXPECT_EQ(
        client.set_unregister_by_endpoint("nqn", 1, "host", 4420, "master"),
        OPERATION_OK);
    EXPECT_EQ(mock_.unmount_requests, std::vector<UUID>{segment.segment_id});
}

}  // namespace
}  // namespace mooncake
