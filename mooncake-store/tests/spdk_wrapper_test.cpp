#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "spdk/spdk_wrapper.h"

namespace {

struct SpdkMockState {
    int env_init_result{0};
    int probe_result{0};
    bool attach_controller{true};
    bool controller_failed{false};
    bool namespace_active{true};
    bool namespace_exists{true};
    bool allocate_qpair{true};
    bool complete_io{true};
    bool completion_error{false};
    bool corrupt_read{false};
    int read_submit_result{0};
    int write_submit_result{0};
    int qpair_poll_result{0};
    int admin_poll_result{0};
    bool emit_namespace_event{false};
    uint32_t sector_size{4096};
    uint64_t namespace_size{1024ULL * 1024 * 1024};
    uint32_t namespace_id{1};
    std::vector<uint32_t> active_namespaces{1};
    std::vector<unsigned char> media;
    spdk_nvme_transport_id trid{};
    spdk_nvme_aer_cb aer_callback{nullptr};
    void* aer_callback_arg{nullptr};
    int env_init_calls{0};
    int env_fini_calls{0};
    int probe_calls{0};
    int detach_calls{0};
    int qpair_alloc_calls{0};
    int qpair_free_calls{0};
    int admin_poll_calls{0};
    int qpair_poll_calls{0};
    int read_calls{0};
    int write_calls{0};
};

SpdkMockState g_spdk;
unsigned char g_ctrlr_token;
unsigned char g_namespace_token;
unsigned char g_qpair_token;

spdk_nvme_ctrlr* FakeController() {
    return reinterpret_cast<spdk_nvme_ctrlr*>(&g_ctrlr_token);
}

spdk_nvme_ns* FakeNamespace() {
    return reinterpret_cast<spdk_nvme_ns*>(&g_namespace_token);
}

spdk_nvme_qpair* FakeQpair() {
    return reinterpret_cast<spdk_nvme_qpair*>(&g_qpair_token);
}

void ResetSpdkMock() { g_spdk = SpdkMockState{}; }

struct TestEnvironmentSetup {
    TestEnvironmentSetup() {
        setenv("MC_SPDK_NOF_ADMIN_POLL_INTERVAL_MS", "0", 1);
        setenv("MC_SPDK_NOF_NAMESPACE_REFRESH_TIMEOUT_MS", "0", 1);
        setenv("MC_SPDK_NOF_NAMESPACE_REFRESH_POLL_INTERVAL_MS", "0", 1);
    }
} g_test_environment_setup;

const std::string kEndpoint =
    "traddr:192.168.1.10 trsvcid:4420 "
    "subnqn:nqn.2016-06.io.spdk:cnode1 trtype:RDMA "
    "adrfam:IPv4 ns:1";

}  // namespace

extern "C" {

void __wrap_spdk_env_opts_init(spdk_env_opts* opts) {
    std::memset(opts, 0, sizeof(*opts));
}

int __wrap_spdk_env_init(const spdk_env_opts*) {
    ++g_spdk.env_init_calls;
    return g_spdk.env_init_result;
}

void __wrap_spdk_env_fini() { ++g_spdk.env_fini_calls; }

void* __wrap_spdk_zmalloc(size_t size, size_t, uint64_t*, int, uint32_t) {
    return std::calloc(1, size);
}

void __wrap_spdk_free(void* ptr) { std::free(ptr); }

int __wrap_spdk_nvme_transport_id_parse(spdk_nvme_transport_id* trid,
                                        const char* text) {
    if (!trid || !text || std::strstr(text, "invalid")) {
        return -1;
    }
    std::memset(trid, 0, sizeof(*trid));
    trid->trtype = SPDK_NVME_TRANSPORT_RDMA;
    trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
    std::strncpy(trid->traddr, "192.168.1.10", sizeof(trid->traddr) - 1);
    std::strncpy(trid->trsvcid, "4420", sizeof(trid->trsvcid) - 1);
    std::strncpy(trid->subnqn, "nqn.2016-06.io.spdk:cnode1",
                 sizeof(trid->subnqn) - 1);
    return 0;
}

const char* __wrap_spdk_nvme_cpl_get_status_string(const spdk_nvme_status*) {
    return "mock completion error";
}

int __wrap_spdk_nvme_probe(const spdk_nvme_transport_id* trid, void* cb_ctx,
                           spdk_nvme_probe_cb probe_cb,
                           spdk_nvme_attach_cb attach_cb, spdk_nvme_remove_cb) {
    ++g_spdk.probe_calls;
    if (g_spdk.probe_result != 0) {
        return g_spdk.probe_result;
    }
    g_spdk.trid = *trid;
    spdk_nvme_ctrlr_opts opts{};
    if (probe_cb && !probe_cb(cb_ctx, trid, &opts)) {
        return 0;
    }
    if (g_spdk.attach_controller && attach_cb) {
        attach_cb(cb_ctx, trid, FakeController(), &opts);
    }
    return 0;
}

int __wrap_spdk_nvme_detach(spdk_nvme_ctrlr*) {
    ++g_spdk.detach_calls;
    return 0;
}

void __wrap_spdk_nvme_ctrlr_register_aer_callback(spdk_nvme_ctrlr*,
                                                  spdk_nvme_aer_cb callback,
                                                  void* callback_arg) {
    g_spdk.aer_callback = callback;
    g_spdk.aer_callback_arg = callback_arg;
}

const spdk_nvme_transport_id* __wrap_spdk_nvme_ctrlr_get_transport_id(
    spdk_nvme_ctrlr*) {
    return &g_spdk.trid;
}

bool __wrap_spdk_nvme_ctrlr_is_failed(spdk_nvme_ctrlr*) {
    return g_spdk.controller_failed;
}

bool __wrap_spdk_nvme_ctrlr_is_active_ns(spdk_nvme_ctrlr*, uint32_t nsid) {
    return g_spdk.namespace_active &&
           std::find(g_spdk.active_namespaces.begin(),
                     g_spdk.active_namespaces.end(),
                     nsid) != g_spdk.active_namespaces.end();
}

uint32_t __wrap_spdk_nvme_ctrlr_get_first_active_ns(spdk_nvme_ctrlr*) {
    return g_spdk.active_namespaces.empty() ? 0
                                            : g_spdk.active_namespaces.front();
}

uint32_t __wrap_spdk_nvme_ctrlr_get_next_active_ns(spdk_nvme_ctrlr*,
                                                   uint32_t previous) {
    auto it = std::find(g_spdk.active_namespaces.begin(),
                        g_spdk.active_namespaces.end(), previous);
    if (it == g_spdk.active_namespaces.end() ||
        ++it == g_spdk.active_namespaces.end()) {
        return 0;
    }
    return *it;
}

spdk_nvme_ns* __wrap_spdk_nvme_ctrlr_get_ns(spdk_nvme_ctrlr*, uint32_t nsid) {
    return g_spdk.namespace_exists && nsid == g_spdk.namespace_id
               ? FakeNamespace()
               : nullptr;
}

spdk_nvme_qpair* __wrap_spdk_nvme_ctrlr_alloc_io_qpair(
    spdk_nvme_ctrlr*, const spdk_nvme_io_qpair_opts*, size_t) {
    ++g_spdk.qpair_alloc_calls;
    return g_spdk.allocate_qpair ? FakeQpair() : nullptr;
}

int __wrap_spdk_nvme_ctrlr_free_io_qpair(spdk_nvme_qpair*) {
    ++g_spdk.qpair_free_calls;
    return 0;
}

int32_t __wrap_spdk_nvme_ctrlr_process_admin_completions(spdk_nvme_ctrlr*) {
    ++g_spdk.admin_poll_calls;
    if (g_spdk.emit_namespace_event && g_spdk.aer_callback) {
        g_spdk.emit_namespace_event = false;
        spdk_nvme_cpl completion{};
        spdk_nvme_async_event_completion event{};
        event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
        event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED;
        completion.cdw0 = event.raw;
        g_spdk.aer_callback(g_spdk.aer_callback_arg, &completion);
    }
    return g_spdk.admin_poll_result;
}

spdk_nvme_ctrlr* __wrap_spdk_nvme_ns_get_ctrlr(spdk_nvme_ns*) {
    return FakeController();
}

uint32_t __wrap_spdk_nvme_ns_get_sector_size(spdk_nvme_ns*) {
    return g_spdk.sector_size;
}

uint64_t __wrap_spdk_nvme_ns_get_size(spdk_nvme_ns*) {
    return g_spdk.namespace_size;
}

uint32_t __wrap_spdk_nvme_ns_get_id(spdk_nvme_ns*) {
    return g_spdk.namespace_id;
}

int32_t __wrap_spdk_nvme_qpair_process_completions(spdk_nvme_qpair*, uint32_t) {
    ++g_spdk.qpair_poll_calls;
    return g_spdk.qpair_poll_result;
}

int __wrap_spdk_nvme_ns_cmd_write(spdk_nvme_ns*, spdk_nvme_qpair*,
                                  void* payload, uint64_t, uint32_t lba_count,
                                  spdk_nvme_cmd_cb callback, void* callback_arg,
                                  uint32_t) {
    ++g_spdk.write_calls;
    if (g_spdk.write_submit_result != 0) {
        return g_spdk.write_submit_result;
    }
    if (g_spdk.complete_io) {
        const size_t size = static_cast<size_t>(lba_count) * g_spdk.sector_size;
        g_spdk.media.assign(static_cast<unsigned char*>(payload),
                            static_cast<unsigned char*>(payload) + size);
        spdk_nvme_cpl completion{};
        if (g_spdk.completion_error) {
            completion.status.sct = SPDK_NVME_SCT_GENERIC;
            completion.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        }
        if (callback) {
            callback(callback_arg, &completion);
        }
    }
    return 0;
}

int __wrap_spdk_nvme_ns_cmd_read(spdk_nvme_ns*, spdk_nvme_qpair*, void* payload,
                                 uint64_t, uint32_t lba_count,
                                 spdk_nvme_cmd_cb callback, void* callback_arg,
                                 uint32_t) {
    ++g_spdk.read_calls;
    if (g_spdk.read_submit_result != 0) {
        return g_spdk.read_submit_result;
    }
    if (g_spdk.complete_io) {
        const size_t size = static_cast<size_t>(lba_count) * g_spdk.sector_size;
        if (!g_spdk.corrupt_read && g_spdk.media.size() >= size) {
            std::memcpy(payload, g_spdk.media.data(), size);
        }
        spdk_nvme_cpl completion{};
        if (g_spdk.completion_error) {
            completion.status.sct = SPDK_NVME_SCT_GENERIC;
            completion.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        }
        if (callback) {
            callback(callback_arg, &completion);
        }
    }
    return 0;
}

}  // extern "C"

namespace mooncake {
namespace {

class SpdkWrapperTest : public ::testing::Test {
   protected:
    void SetUp() override {
        SpdkWrapper::GetInstance().Cleanup();
        ResetSpdkMock();
    }

    void TearDown() override { SpdkWrapper::GetInstance().Cleanup(); }
};

TEST_F(SpdkWrapperTest, InitializesAllocatesAndCleansUpEnvironment) {
    auto& wrapper = SpdkWrapper::GetInstance();
    EXPECT_TRUE(wrapper.InitializeEnv());
    EXPECT_TRUE(wrapper.InitializeEnv());
    EXPECT_EQ(g_spdk.env_init_calls, 1);
    void* buffer = wrapper.Alloc(4096, 4096);
    ASSERT_NE(buffer, nullptr);
    wrapper.Free(buffer);
    wrapper.Free(nullptr);
    wrapper.Cleanup();
    EXPECT_EQ(g_spdk.env_fini_calls, 1);
}

TEST_F(SpdkWrapperTest, ReportsEnvironmentInitializationFailure) {
    g_spdk.env_init_result = -ENOMEM;
    auto& wrapper = SpdkWrapper::GetInstance();
    EXPECT_FALSE(wrapper.InitializeEnv());
    EXPECT_EQ(wrapper.Alloc(4096, 4096), nullptr);
    std::string reason;
    EXPECT_FALSE(wrapper.ProbeNofSegment(kEndpoint, 1, &reason));
    EXPECT_EQ(reason, "spdk_env_init_fail");
}

TEST_F(SpdkWrapperTest, ClassifiesConnectionAndNamespaceErrors) {
    const auto& wrapper = SpdkWrapper::GetInstance();
    EXPECT_FALSE(wrapper.IsConnectionError(0));
    EXPECT_FALSE(wrapper.IsConnectionError(-EINVAL));
    for (int error : {ENXIO, EIO, ECONNRESET, ENODEV, EPIPE, ETIMEDOUT}) {
        EXPECT_TRUE(wrapper.IsConnectionError(-error));
    }
    spdk_nvme_status status{};
    EXPECT_FALSE(wrapper.IsNamespaceFailureStatus(nullptr));
    status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
    EXPECT_TRUE(wrapper.IsNamespaceFailureStatus(&status));
    status.sct = SPDK_NVME_SCT_GENERIC;
    status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
    EXPECT_TRUE(wrapper.IsNamespaceFailureStatus(&status));
    status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
    EXPECT_TRUE(wrapper.IsNamespaceFailureStatus(&status));
    status.sc = SPDK_NVME_SC_SUCCESS;
    EXPECT_FALSE(wrapper.IsNamespaceFailureStatus(&status));
    status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
    EXPECT_FALSE(wrapper.IsNamespaceFailureStatus(&status));
}

TEST_F(SpdkWrapperTest, OpensCachesEnumeratesAndClosesNamespace) {
    auto& wrapper = SpdkWrapper::GetInstance();
    g_spdk.active_namespaces = {1, 2, 4};
    auto* handle = wrapper.OpenNofSegment(kEndpoint);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), handle);
    EXPECT_EQ(g_spdk.probe_calls, 1);
    EXPECT_EQ(g_spdk.qpair_alloc_calls, 1);
    EXPECT_EQ(wrapper.GetBlockSize(handle), 4096U);
    EXPECT_EQ(wrapper.GetNamespaceCapacityBytes(handle), g_spdk.namespace_size);
    EXPECT_EQ(wrapper.GetActiveNamespaces(kEndpoint),
              std::vector<uint32_t>({1, 2, 4}));
    wrapper.CloseNofSegment(kEndpoint);
    EXPECT_EQ(g_spdk.qpair_free_calls, 1);
    auto* reopened = wrapper.OpenNofSegment(kEndpoint);
    ASSERT_NE(reopened, nullptr);
    wrapper.CloseNofSegment(reopened);
    EXPECT_EQ(g_spdk.qpair_free_calls, 2);
}

TEST_F(SpdkWrapperTest, RejectsInvalidEndpointAndMissingNamespace) {
    auto& wrapper = SpdkWrapper::GetInstance();
    EXPECT_TRUE(wrapper.GetControllerKey("invalid endpoint").empty());
    EXPECT_EQ(wrapper.OpenNofSegment("invalid endpoint"), nullptr);
    EXPECT_TRUE(wrapper.GetActiveNamespaces("invalid endpoint").empty());
    wrapper.CloseNofSegment("invalid endpoint");
    wrapper.InvalidateNofController("invalid endpoint");
    g_spdk.namespace_exists = false;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
}

TEST_F(SpdkWrapperTest, BuildsSameControllerKeyForDifferentNamespaces) {
    auto& wrapper = SpdkWrapper::GetInstance();
    std::string ns2 = kEndpoint;
    ns2.back() = '2';
    const auto key = wrapper.GetControllerKey(kEndpoint);
    EXPECT_FALSE(key.empty());
    EXPECT_EQ(key, wrapper.GetControllerKey(ns2));
    EXPECT_EQ(key.find("ns:"), std::string::npos);
}

TEST_F(SpdkWrapperTest, RetriesControllerGeometryAndQpairFailures) {
    auto& wrapper = SpdkWrapper::GetInstance();
    g_spdk.controller_failed = true;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
    EXPECT_EQ(g_spdk.probe_calls, 2);
    EXPECT_EQ(g_spdk.detach_calls, 2);

    wrapper.Cleanup();
    ResetSpdkMock();
    g_spdk.sector_size = 0;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
    EXPECT_EQ(g_spdk.detach_calls, 2);

    wrapper.Cleanup();
    ResetSpdkMock();
    g_spdk.allocate_qpair = false;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
    EXPECT_EQ(g_spdk.qpair_alloc_calls, 2);
}

TEST_F(SpdkWrapperTest, HandlesProbeAndAttachFailures) {
    auto& wrapper = SpdkWrapper::GetInstance();
    g_spdk.probe_result = -1;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
    wrapper.Cleanup();
    ResetSpdkMock();
    g_spdk.attach_controller = false;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
    EXPECT_EQ(g_spdk.probe_calls, 2);
}

TEST_F(SpdkWrapperTest, RefreshesNamespaceAndReportsAdminFailure) {
    auto& wrapper = SpdkWrapper::GetInstance();
    g_spdk.namespace_active = false;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
    EXPECT_GT(g_spdk.admin_poll_calls, 0);
    wrapper.Cleanup();
    ResetSpdkMock();
    g_spdk.namespace_active = false;
    g_spdk.admin_poll_result = -ENXIO;
    EXPECT_EQ(wrapper.OpenNofSegment(kEndpoint), nullptr);
    EXPECT_EQ(g_spdk.detach_calls, 2);
}

TEST_F(SpdkWrapperTest, PollsAdminIoAndConsumesNamespaceEvent) {
    auto& wrapper = SpdkWrapper::GetInstance();
    auto* handle = wrapper.OpenNofSegment(kEndpoint);
    ASSERT_NE(handle, nullptr);
    g_spdk.admin_poll_result = 1;
    g_spdk.emit_namespace_event = true;
    EXPECT_EQ(wrapper.NvmePollAdminCompletions(handle), 1);
    auto changed = wrapper.ConsumeNamespaceAttributeChangedControllers();
    ASSERT_EQ(changed.size(), 1U);
    EXPECT_EQ(changed.front(), wrapper.GetControllerKey(kEndpoint));
    EXPECT_TRUE(wrapper.ConsumeNamespaceAttributeChangedControllers().empty());
    g_spdk.qpair_poll_result = 3;
    EXPECT_EQ(wrapper.NvmePollProcessCompletion(handle, 8), 3);
    EXPECT_EQ(g_spdk.qpair_poll_calls, 1);
}

TEST_F(SpdkWrapperTest, SubmitsReadsWritesAndRejectsInvalidRequests) {
    auto& wrapper = SpdkWrapper::GetInstance();
    auto* handle = wrapper.OpenNofSegment(kEndpoint);
    ASSERT_NE(handle, nullptr);
    std::vector<unsigned char> data(4096, 0x5a);
    EXPECT_EQ(
        wrapper.SubmitRequest(handle, data.data(), 0, 1, 1, nullptr, nullptr),
        0);
    EXPECT_EQ(
        wrapper.SubmitRequest(handle, data.data(), 0, 1, 0, nullptr, nullptr),
        0);
    EXPECT_EQ(g_spdk.write_calls, 1);
    EXPECT_EQ(g_spdk.read_calls, 1);
    EXPECT_EQ(
        wrapper.SubmitRequest(nullptr, data.data(), 0, 1, 0, nullptr, nullptr),
        -ENXIO);
    EXPECT_EQ(wrapper.SubmitRequest(handle, nullptr, 0, 1, 0, nullptr, nullptr),
              -ENXIO);
    EXPECT_EQ(
        wrapper.SubmitRequest(handle, data.data(), 0, 0, 0, nullptr, nullptr),
        -ENXIO);
}

TEST_F(SpdkWrapperTest, InvalidatesAndCleansStaleControllerOnReopen) {
    auto& wrapper = SpdkWrapper::GetInstance();
    auto* handle = wrapper.OpenNofSegment(kEndpoint);
    ASSERT_NE(handle, nullptr);
    wrapper.InvalidateNofController(handle);
    EXPECT_EQ(wrapper.GetBlockSize(handle), INVALID_BLOCK_SIZE);
    auto* reopened = wrapper.OpenNofSegment(kEndpoint);
    ASSERT_NE(reopened, nullptr);
    EXPECT_GE(g_spdk.detach_calls, 1);
    EXPECT_GE(g_spdk.qpair_free_calls, 1);
}

TEST_F(SpdkWrapperTest, WriteReadProbeSucceedsAndDetectsMismatch) {
    auto& wrapper = SpdkWrapper::GetInstance();
    std::string reason;
    EXPECT_TRUE(wrapper.ProbeNofSegment(kEndpoint, 10, &reason));
    EXPECT_EQ(g_spdk.write_calls, 1);
    EXPECT_EQ(g_spdk.read_calls, 1);
    g_spdk.corrupt_read = true;
    EXPECT_FALSE(wrapper.ProbeNofSegment(kEndpoint, 10, &reason));
    EXPECT_EQ(reason, "namespace_io_error:probe_mismatch");
}

TEST_F(SpdkWrapperTest, ProbeReportsSubmissionAndCompletionFailures) {
    auto& wrapper = SpdkWrapper::GetInstance();
    std::string reason;
    g_spdk.write_submit_result = -ENOMEM;
    EXPECT_FALSE(wrapper.ProbeNofSegment(kEndpoint, 10, &reason));
    EXPECT_EQ(reason, "submit_fail:write_probe");
    g_spdk.write_submit_result = 0;
    g_spdk.completion_error = true;
    EXPECT_FALSE(wrapper.ProbeNofSegment(kEndpoint, 10, &reason));
    EXPECT_FALSE(reason.empty());
}

TEST_F(SpdkWrapperTest, ProbeReportsTimeoutAndConnectionFailure) {
    auto& wrapper = SpdkWrapper::GetInstance();
    std::string reason;
    g_spdk.complete_io = false;
    EXPECT_FALSE(wrapper.ProbeNofSegment(kEndpoint, 0, &reason));
    EXPECT_EQ(reason, "completion_timeout:write_probe");
    wrapper.Cleanup();
    ResetSpdkMock();
    g_spdk.write_submit_result = -ENXIO;
    EXPECT_FALSE(wrapper.ProbeNofSegment(kEndpoint, 10, &reason));
    EXPECT_EQ(reason, "controller_error:-6");
}

TEST_F(SpdkWrapperTest, NullHandlesReturnDefensiveErrors) {
    auto& wrapper = SpdkWrapper::GetInstance();
    EXPECT_EQ(wrapper.NvmePollProcessCompletion(nullptr, 1), -ENXIO);
    EXPECT_EQ(wrapper.NvmePollAdminCompletions(nullptr), -EINVAL);
    EXPECT_EQ(wrapper.GetBlockSize(nullptr), INVALID_BLOCK_SIZE);
    EXPECT_EQ(wrapper.GetNamespaceCapacityBytes(nullptr), 0U);
    wrapper.RecordNamespaceAttributeChanged(nullptr);
    wrapper.InvalidateNofController(static_cast<nof_seg_handle*>(nullptr));
    wrapper.CloseNofSegment(static_cast<nof_seg_handle*>(nullptr));
}

}  // namespace
}  // namespace mooncake
