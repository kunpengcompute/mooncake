#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT)
#include <acl/acl.h>
#endif

#include "allocator.h"
#include "client_service.h"
#include "master_client.h"
#include "real_client.h"
#include "test_server_helpers.h"
#include "types.h"
#include "common/client_buffer_allocation.h"
#include "common/network.h"

namespace mooncake {

struct NofStagingTestPeer {
    static void ExpectZeroPools(RealClient& client, const char* stage) {
        SCOPED_TRACE(stage);
        std::lock_guard<std::mutex> lock(client.nof_staging_pool_mutex_);
        ASSERT_NE(client.client_buffer_allocator_, nullptr);
        EXPECT_EQ(client.client_buffer_allocator_->size(), 0u);
        EXPECT_EQ(client.client_buffer_allocator_->getBase(), nullptr);
        EXPECT_EQ(client.nof_staging_pool_, nullptr);
    }
};

namespace testing {

enum class HandshakeMode { P2P, Metadata };

namespace {

class EnvGuard {
   public:
    explicit EnvGuard(const char* key) : key_(key) {
        if (const char* value = std::getenv(key_)) {
            old_value_ = value;
        }
    }

    ~EnvGuard() {
        if (old_value_.has_value()) {
            setenv(key_, old_value_->c_str(), 1);
        } else {
            unsetenv(key_);
        }
    }

   private:
    const char* key_;
    std::optional<std::string> old_value_;
};

class VLogGuard {
   public:
    explicit VLogGuard(int new_v) : old_v_(FLAGS_v) { FLAGS_v = new_v; }
    ~VLogGuard() { FLAGS_v = old_v_; }

   private:
    int old_v_;
};

class StrategyCaptureSink : public google::LogSink {
   public:
    void send(google::LogSeverity severity, const char* full_filename,
              const char* base_filename, int line, const struct ::tm* tm_time,
              const char* message, size_t message_len) override {
        (void)severity;
        (void)full_filename;
        (void)base_filename;
        (void)line;
        (void)tm_time;

        std::string msg(message, message_len);
        constexpr char kPrefix[] = "Using transfer strategy: ";
        size_t pos = msg.find(kPrefix);
        if (pos == std::string::npos) {
            return;
        }

        captured_strategy_ = msg.substr(pos + std::strlen(kPrefix));
        TrimInPlace(captured_strategy_);
    }

    const std::string& strategy() const { return captured_strategy_; }

   private:
    static void TrimInPlace(std::string& value) {
        auto begin = value.find_first_not_of(" \t\n\r");
        if (begin == std::string::npos) {
            value.clear();
            return;
        }
        auto end = value.find_last_not_of(" \t\n\r");
        value = value.substr(begin, end - begin + 1);
    }

    std::string captured_strategy_;
};

struct ClientRuntime {
    std::shared_ptr<Client> client;
    std::unique_ptr<SimpleAllocator> io_allocator;
    void* segment_ptr = nullptr;
    size_t segment_size = 0;
    std::string host_name;
};

struct PreparedObject {
    std::string key;
    std::string payload;
    QueryResult query_result;
    Replica::Descriptor replica;
};

Replica::Descriptor FindFirstCompleteMemoryReplica(const QueryResult& result) {
    for (const auto& replica : result.replicas) {
        if (replica.status == ReplicaStatus::COMPLETE &&
            replica.is_memory_replica()) {
            return replica;
        }
    }
    throw std::runtime_error("No complete memory replica found");
}

bool PutHotKeyHelper(LocalHotCache& cache, const std::string& key,
                     const Slice& slice) {
    if (key.empty() || slice.ptr == nullptr || slice.size == 0) {
        return false;
    }
    if (cache.TouchHotKey(key)) {
        return true;
    }

    HotMemBlock* block = cache.GetFreeBlock();
    if (block == nullptr) {
        return false;
    }
    if (slice.size > block->size) {
        block->key_.clear();
        cache.PutHotKey(block);
        return false;
    }

    std::memcpy(block->addr, slice.ptr, slice.size);
    block->size = slice.size;
    block->key_ = key;
    return cache.PutHotKey(block);
}

const char* HandshakeModeName(HandshakeMode mode) {
    switch (mode) {
        case HandshakeMode::P2P:
            return "P2P";
        case HandshakeMode::Metadata:
            return "Metadata";
    }
    return "Unknown";
}

}  // namespace

class TcpLocalMemcpyAutoEnableTest : public ::testing::Test {
   protected:
    static void SetUpTestSuite() {
        google::InitGoogleLogging("TcpLocalMemcpyAutoEnableTest");
        FLAGS_logtostderr = 1;
#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT)
        const char* value = std::getenv("MC_TEST_ASCEND_DEVICE");
        ASSERT_NE(value, nullptr)
            << "Set MC_TEST_ASCEND_DEVICE to the NPU logical device ID for "
               "this Ascend-enabled test binary";
        const std::string device(value);
        ASSERT_FALSE(device.empty());
        ASSERT_EQ(device.find_first_not_of("0123456789"), std::string::npos)
            << "MC_TEST_ASCEND_DEVICE must be a nonnegative integer";
        errno = 0;
        const long device_id = std::strtol(value, nullptr, 10);
        ASSERT_EQ(errno, 0);
        ASSERT_LE(device_id, INT_MAX);

        const auto init_result = aclInit(nullptr);
        ASSERT_TRUE(init_result == ACL_SUCCESS ||
                    init_result == ACL_ERROR_REPEAT_INITIALIZE)
            << "aclInit failed: " << init_result;
        owns_acl_ = init_result == ACL_SUCCESS;
        aclrtContext previous_context = nullptr;
        const auto context_result = aclrtGetCurrentContext(&previous_context);
        ASSERT_TRUE(context_result == ACL_SUCCESS ||
                    context_result == ACL_ERROR_RT_CONTEXT_NULL)
            << "aclrtGetCurrentContext failed: " << context_result;
        if (context_result == ACL_SUCCESS) {
            previous_context_ = previous_context;
        }
        ASSERT_EQ(
            aclrtCreateContext(&test_context_, static_cast<int32_t>(device_id)),
            ACL_SUCCESS);
        ASSERT_NE(test_context_, nullptr);
#endif
    }

    static void TearDownTestSuite() {
#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT)
        if (test_context_) {
            EXPECT_EQ(aclrtDestroyContext(test_context_), ACL_SUCCESS);
            test_context_ = nullptr;
        }
        if (previous_context_) {
            EXPECT_EQ(aclrtSetCurrentContext(previous_context_), ACL_SUCCESS);
            previous_context_ = nullptr;
        }
        if (owns_acl_) {
            EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
            owns_acl_ = false;
        }
#endif
        google::ShutdownGoogleLogging();
    }

    void SetUp() override {
#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT)
        ASSERT_NE(test_context_, nullptr);
        ASSERT_EQ(aclrtSetCurrentContext(test_context_), ACL_SUCCESS);
#endif
        memcpy_guard_ = std::make_unique<EnvGuard>("MC_STORE_MEMCPY");
        vlog_guard_ = std::make_unique<VLogGuard>(1);
        unsetenv("MC_STORE_MEMCPY");

        InProcMasterConfig config;
        config.http_metadata_port = getFreeTcpPort();
        ASSERT_TRUE(master_.Start(config));
        master_address_ = master_.master_address();
        metadata_url_ = master_.metadata_url();
    }

    void TearDown() override {
        CleanupRuntime(remote_runtime_);
        CleanupRuntime(runtime_);
        master_.Stop();
    }

    ClientRuntime CreateRuntime(const std::string& host_name,
                                const std::string& metadata_conn) {
        ClientRuntime runtime;
        runtime.host_name = host_name;

        auto client_opt = Client::Create(host_name, metadata_conn, "tcp",
                                         std::nullopt, master_address_);
        EXPECT_TRUE(client_opt.has_value())
            << "Failed to create client for host " << host_name
            << ", metadata=" << metadata_conn;
        if (!client_opt.has_value()) {
            return runtime;
        }

        runtime.client = client_opt.value();
        runtime.io_allocator =
            std::make_unique<SimpleAllocator>(16 * 1024 * 1024);
        auto reg = runtime.client->RegisterLocalMemory(
            runtime.io_allocator->getBase(), 16 * 1024 * 1024, "cpu:0", false,
            false);
        EXPECT_TRUE(reg.has_value())
            << "RegisterLocalMemory failed: " << toString(reg.error());

        runtime.segment_size = 64 * 1024 * 1024;
        runtime.segment_ptr =
            allocate_buffer_allocator_memory(runtime.segment_size);
        EXPECT_NE(runtime.segment_ptr, nullptr);
        if (runtime.segment_ptr == nullptr) {
            return runtime;
        }

        auto mount = runtime.client->MountSegment(runtime.segment_ptr,
                                                  runtime.segment_size, "tcp");
        EXPECT_TRUE(mount.has_value())
            << "MountSegment failed: " << toString(mount.error());
        if (!mount.has_value()) {
            free_memory("", runtime.segment_ptr);
            runtime.segment_ptr = nullptr;
        }

        return runtime;
    }

    void CleanupRuntime(ClientRuntime& runtime) {
        if (runtime.client && runtime.segment_ptr) {
            auto unmount = runtime.client->UnmountSegment(runtime.segment_ptr,
                                                          runtime.segment_size);
            EXPECT_TRUE(unmount.has_value())
                << "UnmountSegment failed: " << toString(unmount.error());
        }

        runtime.client.reset();
        runtime.io_allocator.reset();

        if (runtime.segment_ptr) {
            free_memory("", runtime.segment_ptr);
            runtime.segment_ptr = nullptr;
        }
        runtime.segment_size = 0;
        runtime.host_name.clear();
    }

    PreparedObject PrepareLocalObject(const std::string& key,
                                      const std::string& payload) {
        return PrepareObjectOnRuntime(runtime_, key, payload);
    }

    PreparedObject PrepareObjectOnRuntime(ClientRuntime& runtime,
                                          const std::string& key,
                                          const std::string& payload) {
        void* write_buf = runtime.io_allocator->allocate(payload.size());
        EXPECT_NE(write_buf, nullptr);
        std::memcpy(write_buf, payload.data(), payload.size());

        std::vector<Slice> write_slices;
        write_slices.emplace_back(Slice{write_buf, payload.size()});

        ReplicateConfig config;
        config.replica_num = 1;
        config.preferred_segment = runtime.host_name;

        auto put = runtime.client->Put(key, write_slices, config);
        runtime.io_allocator->deallocate(write_buf, payload.size());
        EXPECT_TRUE(put.has_value()) << "Put failed: " << toString(put.error());

        auto query = runtime.client->Query(key);
        EXPECT_TRUE(query.has_value())
            << "Query failed: " << toString(query.error());

        auto replica = FindFirstCompleteMemoryReplica(query.value());

        return PreparedObject{key, payload, std::move(query.value()), replica};
    }

    void LogReplicaDiagnostics(const std::string& label,
                               const Replica::Descriptor& replica) {
        const auto& endpoint = replica.get_memory_descriptor()
                                   .buffer_descriptor.transport_endpoint_;
        LOG(INFO) << label << " replica endpoint=" << endpoint;
        LOG(INFO) << label << " client transport endpoint="
                  << runtime_.client->GetTransportEndpoint();
        LOG(INFO) << label << " is local replica="
                  << runtime_.client->IsReplicaOnLocalMemory(replica);
    }

    InProcMaster master_;
    std::string master_address_;
    std::string metadata_url_;
    ClientRuntime runtime_;
    ClientRuntime remote_runtime_;
    std::unique_ptr<EnvGuard> memcpy_guard_;
    std::unique_ptr<VLogGuard> vlog_guard_;
#if defined(USE_ASCEND) || defined(USE_ASCEND_DIRECT)
    inline static bool owns_acl_ = false;
    inline static aclrtContext previous_context_ = nullptr;
    inline static aclrtContext test_context_ = nullptr;
#endif
};

TEST(NofHostStagingSetupTest, RejectsNonBinaryModeBeforeConnecting) {
    EnvGuard staging_guard("MC_STORE_NOF_HOST_STAGING");
    setenv("MC_STORE_NOF_HOST_STAGING", "true", 1);
    RealClient client;
    EXPECT_EQ(
        client.setup_real("127.0.0.1:19093", "P2PHANDSHAKE", 0, 4096, "tcp"),
        toInt(ErrorCode::INVALID_PARAMS));
}

TEST_F(TcpLocalMemcpyAutoEnableTest,
       ZeroPoolMemoryDoesNotCreateNofStagingPool) {
    EnvGuard staging_guard("MC_STORE_NOF_HOST_STAGING");
    EnvGuard replicas_guard("MC_STORE_REPLICA_NUM");
    EnvGuard nof_replicas_guard("MC_STORE_NOF_REPLICA_NUM");
    unsetenv("MC_STORE_REPLICA_NUM");
    unsetenv("MC_STORE_NOF_REPLICA_NUM");
    setenv("MC_STORE_MEMCPY", "0", 1);

    for (const char* staging : {"0", "1"}) {
        SCOPED_TRACE(staging);
        setenv("MC_STORE_NOF_HOST_STAGING", staging, 1);
        std::vector<char> source(128), target(128, 0);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<char>(i);
        RealClient client;
        ASSERT_EQ(
            client.setup_real("127.0.0.1:" + std::to_string(getFreeTcpPort()),
                              "P2PHANDSHAKE", 16 * 1024 * 1024, 0, "tcp", "",
                              master_address_),
            0);
        ASSERT_EQ(client.is_nof_host_staging(), std::string(staging) == "1");
        ASSERT_NO_FATAL_FAILURE(
            NofStagingTestPeer::ExpectZeroPools(client, "setup"));
        ASSERT_EQ(client.register_buffer(source.data(), source.size()), 0);
        ASSERT_EQ(client.register_buffer(target.data(), target.size()), 0);
        ReplicateConfig config;
        config.prefer_alloc_in_same_node = false;
        const std::string key = std::string("zero-pool-memory-") + staging;
        ASSERT_EQ(client.batch_put_from_multi_buffers(
                      {key}, {{source.data(), source.data() + 64}}, {{64, 64}},
                      config),
                  std::vector<int>{0});
        const auto replicas = client.get_replica_desc(key);
        ASSERT_EQ(replicas.size(), 1u);
        ASSERT_TRUE(replicas.front().is_memory_replica());
        ASSERT_EQ(client.batch_get_into_multi_buffers(
                      {key}, {{target.data(), target.data() + 32}}, {{32, 96}},
                      false),
                  std::vector<int>{128});
        EXPECT_EQ(target, source);
        ASSERT_NO_FATAL_FAILURE(
            NofStagingTestPeer::ExpectZeroPools(client, "round trip"));
        ASSERT_EQ(client.remove(key, true), 0);
        ASSERT_EQ(client.unregister_buffer(target.data()), 0);
        ASSERT_EQ(client.unregister_buffer(source.data()), 0);
        ASSERT_EQ(client.tearDownAll(), 0);
    }
}

TEST_F(TcpLocalMemcpyAutoEnableTest, MemoryOnlyBatchPutDoesNotInvokeNofStager) {
    EnvGuard memory_replicas("MC_STORE_REPLICA_NUM");
    EnvGuard nof_replicas("MC_STORE_NOF_REPLICA_NUM");
    unsetenv("MC_STORE_REPLICA_NUM");
    unsetenv("MC_STORE_NOF_REPLICA_NUM");
    // A remote memory replica must also bypass the NoF-specific callback.
    // Testing only local memcpy could hide an incorrectly placed generic hook.
    runtime_ = CreateRuntime("127.0.0.1:18000", "P2PHANDSHAKE");
    remote_runtime_ = CreateRuntime("127.0.0.1:18001", "P2PHANDSHAKE");
    ASSERT_NE(runtime_.client, nullptr);
    ASSERT_NE(remote_runtime_.client, nullptr);
    const std::string payload = "original-memory-payload";
    void* source = runtime_.io_allocator->allocate(payload.size());
    ASSERT_NE(source, nullptr);
    std::memcpy(source, payload.data(), payload.size());
    std::vector<std::vector<Slice>> slices{{{source, payload.size()}}};
    ReplicateConfig config;
    config.replica_num = 1;
    config.nof_replica_num = 0;
    config.preferred_segment = remote_runtime_.host_name;
    bool called = false;
    Client::NofWriteBufferStager nof_stager = [&](const std::vector<Slice>&,
                                                  const Replica::Descriptor&)
        -> tl::expected<std::vector<Slice>, ErrorCode> {
        called = true;
        throw std::runtime_error("memory must not need NoF staging");
    };
    const std::string key = "memory_bypasses_nof_stager";
    auto results =
        runtime_.client->BatchPut({key}, slices, config, {}, nof_stager);
    ASSERT_EQ(results.size(), 1);
    ASSERT_TRUE(results[0]);
    EXPECT_FALSE(called);
    EXPECT_EQ(slices.front().front().ptr, source);
    auto query = runtime_.client->Query(key);
    ASSERT_TRUE(query);
    ASSERT_EQ(query->replicas.size(), 1);
    ASSERT_TRUE(query->replicas.front().is_memory_replica());
    EXPECT_EQ(query->replicas.front().status, ReplicaStatus::COMPLETE);
    EXPECT_EQ(query->replicas.front()
                  .get_memory_descriptor()
                  .buffer_descriptor.transport_endpoint_,
              remote_runtime_.client->GetTransportEndpoint());
    void* output = runtime_.io_allocator->allocate(payload.size());
    ASSERT_NE(output, nullptr);
    std::vector<Slice> read_slices{{output, payload.size()}};
    ASSERT_TRUE(runtime_.client->Get(key, *query, read_slices));
    EXPECT_EQ(std::memcmp(output, payload.data(), payload.size()), 0);
    runtime_.io_allocator->deallocate(output, payload.size());
    runtime_.io_allocator->deallocate(source, payload.size());
}

#ifdef USE_NOF
TEST_F(TcpLocalMemcpyAutoEnableTest,
       NoFStagingFailuresPreserveMemoryAndRevokeOnlyFailedReplicas) {
    EnvGuard memory_replicas("MC_STORE_REPLICA_NUM");
    EnvGuard nof_replicas("MC_STORE_NOF_REPLICA_NUM");
    unsetenv("MC_STORE_REPLICA_NUM");
    unsetenv("MC_STORE_NOF_REPLICA_NUM");
    runtime_ = CreateRuntime("127.0.0.1", "P2PHANDSHAKE");
    ASSERT_NE(runtime_.client, nullptr);
    MasterClient registrar(runtime_.client->getClientId());
    ASSERT_EQ(registrar.Connect(master_address_), ErrorCode::OK);
    NoFSegment segment;
    segment.id = generate_uuid();
    segment.name = "nof_stager_failure_segment";
    segment.base = 0;
    segment.size = 16 * 1024 * 1024;
    segment.block_size = 512;
    // Metadata allocation is real; callbacks fail before SPDK submission, so
    // this regression needs no NVMe target or device memory.
    segment.te_endpoint = "nof_stager_failure_endpoint";
    ASSERT_TRUE(registrar.MountNoFSegment(segment));
    std::string payload(512, 'm');
    ReplicateConfig config;
    config.replica_num = 1;
    config.nof_replica_num = 1;
    const std::vector<ObjectKey> keys = {"nof_error", "nof_exception"};
    std::vector<std::vector<Slice>> slices(keys.size(),
                                           {{payload.data(), payload.size()}});
    size_t calls = 0;
    Client::NofWriteBufferStager nof_stager =
        [&](const std::vector<Slice>& source,
            const Replica::Descriptor& replica)
        -> tl::expected<std::vector<Slice>, ErrorCode> {
        EXPECT_TRUE(replica.is_nof_replica());
        EXPECT_EQ(source.front().ptr, payload.data());
        if (calls++ == 1) throw std::runtime_error("NoF staging failure");
        return tl::unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    };
    std::string generic_staging(payload);
    Client::WriteBufferStager remote_stager = [&](const std::vector<Slice>&) {
        return tl::expected<std::vector<Slice>, ErrorCode>(std::vector<Slice>{
            {generic_staging.data(), generic_staging.size()}});
    };
    auto results = runtime_.client->BatchPut(keys, slices, config,
                                             remote_stager, nof_stager);
    ASSERT_EQ(results.size(), keys.size());
    EXPECT_EQ(calls, keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        ASSERT_TRUE(results[i]);
        auto query = runtime_.client->Query(keys[i]);
        ASSERT_TRUE(query);
        ASSERT_EQ(query->replicas.size(), 1);
        EXPECT_TRUE(query->replicas.front().is_memory_replica());
        EXPECT_EQ(query->replicas.front().status, ReplicaStatus::COMPLETE);
        EXPECT_EQ(slices[i].front().ptr, payload.data());
        std::vector<char> output(payload.size());
        std::vector<Slice> read_slices{{output.data(), output.size()}};
        ASSERT_TRUE(runtime_.client->Get(keys[i], *query, read_slices));
        EXPECT_EQ(std::string(output.begin(), output.end()), payload);
    }

    config.replica_num = 0;
    std::vector<std::vector<Slice>> nof_only_slices{
        {{payload.data(), payload.size()}}};
    auto failed = runtime_.client->BatchPut(
        {"nof_only_failure"}, nof_only_slices, config, {}, nof_stager);
    ASSERT_EQ(failed.size(), 1);
    ASSERT_FALSE(failed[0]);
    EXPECT_EQ(failed[0].error(), ErrorCode::NO_AVAILABLE_HANDLE);
    auto query = runtime_.client->Query("nof_only_failure");
    ASSERT_FALSE(query);
    EXPECT_EQ(query.error(), ErrorCode::OBJECT_NOT_FOUND);
    EXPECT_TRUE(registrar.UnmountNoFSegment(segment.id));
}

#endif

TEST_F(TcpLocalMemcpyAutoEnableTest, P2PLocalReplicaUsesLocalMemcpy) {
    setenv("MC_STORE_MEMCPY", "1", 1);
    runtime_ = CreateRuntime("127.0.0.1", "P2PHANDSHAKE");
    ASSERT_TRUE(runtime_.client != nullptr);

    auto prepared =
        PrepareLocalObject("p2p_local_memcpy_key", "hello-local-memcpy");
    LogReplicaDiagnostics("[P2P]", prepared.replica);

    StrategyCaptureSink sink;
    google::AddLogSink(&sink);

    std::vector<char> out(prepared.payload.size(), '\0');
    std::vector<Slice> read_slices;
    read_slices.emplace_back(Slice{out.data(), out.size()});

    auto get =
        runtime_.client->Get(prepared.key, prepared.query_result, read_slices);

    google::RemoveLogSink(&sink);

    ASSERT_TRUE(get.has_value()) << "Get failed: " << toString(get.error());
    ASSERT_EQ(std::memcmp(out.data(), prepared.payload.data(), out.size()), 0);
    ASSERT_TRUE(runtime_.client->IsReplicaOnLocalMemory(prepared.replica));

    const auto& replica_endpoint = prepared.replica.get_memory_descriptor()
                                       .buffer_descriptor.transport_endpoint_;
    EXPECT_EQ(replica_endpoint, runtime_.client->GetTransportEndpoint());
    EXPECT_EQ(sink.strategy(), "LOCAL_MEMCPY");
}

TEST_F(TcpLocalMemcpyAutoEnableTest, MetadataLocalReplicaUsesLocalMemcpy) {
    setenv("MC_STORE_MEMCPY", "1", 1);
    runtime_ = CreateRuntime("127.0.0.1", metadata_url_);
    ASSERT_TRUE(runtime_.client != nullptr);

    auto prepared =
        PrepareLocalObject("metadata_local_memcpy_key", "hello-local-memcpy");
    LogReplicaDiagnostics("[METADATA]", prepared.replica);

    const auto& replica_endpoint = prepared.replica.get_memory_descriptor()
                                       .buffer_descriptor.transport_endpoint_;
    const auto client_endpoint = runtime_.client->GetTransportEndpoint();
    const bool is_local =
        runtime_.client->IsReplicaOnLocalMemory(prepared.replica);

    StrategyCaptureSink sink;
    google::AddLogSink(&sink);

    std::vector<char> out(prepared.payload.size(), '\0');
    std::vector<Slice> read_slices;
    read_slices.emplace_back(Slice{out.data(), out.size()});

    auto get =
        runtime_.client->Get(prepared.key, prepared.query_result, read_slices);

    google::RemoveLogSink(&sink);

    ASSERT_TRUE(get.has_value()) << "Get failed: " << toString(get.error());
    ASSERT_EQ(std::memcmp(out.data(), prepared.payload.data(), out.size()), 0);
    ASSERT_TRUE(is_local);
    EXPECT_NE(replica_endpoint, client_endpoint);
    EXPECT_EQ(sink.strategy(), "LOCAL_MEMCPY");
}

TEST_F(TcpLocalMemcpyAutoEnableTest,
       P2PRemoteReplicaOnSameTcpHostUsesTransferEngine) {
    remote_runtime_ = CreateRuntime("127.0.0.1:18001", "P2PHANDSHAKE");
    runtime_ = CreateRuntime("127.0.0.1:18002", "P2PHANDSHAKE");
    ASSERT_TRUE(remote_runtime_.client != nullptr);
    ASSERT_TRUE(runtime_.client != nullptr);

    auto prepared = PrepareObjectOnRuntime(
        remote_runtime_, "p2p_remote_same_host_key", "hello-remote-transfer");
    LogReplicaDiagnostics("[P2P_REMOTE]", prepared.replica);

    const auto& replica_endpoint = prepared.replica.get_memory_descriptor()
                                       .buffer_descriptor.transport_endpoint_;
    const auto client_endpoint = runtime_.client->GetTransportEndpoint();
    ASSERT_NE(replica_endpoint, client_endpoint);
    ASSERT_FALSE(runtime_.client->IsReplicaOnLocalMemory(prepared.replica));

    StrategyCaptureSink sink;
    google::AddLogSink(&sink);

    std::vector<char> out(prepared.payload.size(), '\0');
    std::vector<Slice> read_slices;
    read_slices.emplace_back(Slice{out.data(), out.size()});

    auto get =
        runtime_.client->Get(prepared.key, prepared.query_result, read_slices);

    google::RemoveLogSink(&sink);

    ASSERT_TRUE(get.has_value()) << "Get failed: " << toString(get.error());
    ASSERT_EQ(std::memcmp(out.data(), prepared.payload.data(), out.size()), 0);
    EXPECT_EQ(sink.strategy(), "TRANSFER_ENGINE");
}

TEST_F(TcpLocalMemcpyAutoEnableTest,
       MetadataRemoteReplicaOnSameTcpHostUsesTransferEngine) {
    remote_runtime_ = CreateRuntime("metadata-host-a", metadata_url_);
    runtime_ = CreateRuntime("metadata-host-b", metadata_url_);
    ASSERT_TRUE(remote_runtime_.client != nullptr);
    ASSERT_TRUE(runtime_.client != nullptr);

    auto prepared =
        PrepareObjectOnRuntime(remote_runtime_, "metadata_remote_same_host_key",
                               "hello-metadata-remote-transfer");
    LogReplicaDiagnostics("[METADATA_REMOTE]", prepared.replica);

    const auto& replica_endpoint = prepared.replica.get_memory_descriptor()
                                       .buffer_descriptor.transport_endpoint_;
    const auto client_endpoint = runtime_.client->GetTransportEndpoint();
    ASSERT_EQ(replica_endpoint, remote_runtime_.host_name);
    ASSERT_NE(replica_endpoint, runtime_.host_name);
    ASSERT_NE(replica_endpoint, client_endpoint);
    ASSERT_FALSE(runtime_.client->IsReplicaOnLocalMemory(prepared.replica));

    StrategyCaptureSink sink;
    google::AddLogSink(&sink);

    std::vector<char> out(prepared.payload.size(), '\0');
    std::vector<Slice> read_slices;
    read_slices.emplace_back(Slice{out.data(), out.size()});

    auto get =
        runtime_.client->Get(prepared.key, prepared.query_result, read_slices);

    google::RemoveLogSink(&sink);

    ASSERT_TRUE(get.has_value()) << "Get failed: " << toString(get.error());
    ASSERT_EQ(std::memcmp(out.data(), prepared.payload.data(), out.size()), 0);
    EXPECT_EQ(sink.strategy(), "TRANSFER_ENGINE");
}

class HotCacheRedirectStrategyTest
    : public TcpLocalMemcpyAutoEnableTest,
      public ::testing::WithParamInterface<HandshakeMode> {};

TEST_P(HotCacheRedirectStrategyTest, CacheHitUsesLocalMemcpy) {
    EnvGuard cache_size_guard("MC_STORE_LOCAL_HOT_CACHE_SIZE");
    setenv("MC_STORE_LOCAL_HOT_CACHE_SIZE", "33554432", 1);  // 32MB

    const HandshakeMode mode = GetParam();
    const bool is_p2p = mode == HandshakeMode::P2P;
    const std::string metadata_conn = is_p2p ? "P2PHANDSHAKE" : metadata_url_;
    const std::string host_name =
        is_p2p ? "localhost" : "metadata-hot-cache-host";

    runtime_ = CreateRuntime(host_name, metadata_conn);
    ASSERT_TRUE(runtime_.client != nullptr) << HandshakeModeName(mode);
    ASSERT_TRUE(runtime_.client->IsHotCacheEnabled())
        << HandshakeModeName(mode);

    const std::string key = is_p2p ? "p2p_hot_cache_redirect_key"
                                   : "metadata_hot_cache_redirect_key";
    const std::string payload =
        is_p2p ? "p2p-hot-cache-data" : "metadata-hot-cache-data";

    Slice cache_slice{const_cast<char*>(payload.data()), payload.size()};
    ASSERT_TRUE(
        PutHotKeyHelper(*runtime_.client->GetHotCache(), key, cache_slice));
    ASSERT_TRUE(runtime_.client->GetHotCache()->HasHotKey(key));

    Replica::Descriptor replica;
    replica.id = is_p2p ? 1 : 2;
    replica.status = ReplicaStatus::COMPLETE;
    MemoryDescriptor mem_desc;
    mem_desc.buffer_descriptor.transport_endpoint_ = "remote:9999";
    mem_desc.buffer_descriptor.buffer_address_ = 0;
    mem_desc.buffer_descriptor.size_ = payload.size();
    replica.descriptor_variant = mem_desc;

    ASSERT_FALSE(runtime_.client->IsReplicaOnLocalMemory(replica))
        << HandshakeModeName(mode);

    std::vector<Replica::Descriptor> replicas;
    replicas.emplace_back(replica);
    QueryResult query_result(
        std::move(replicas),
        std::chrono::steady_clock::now() + std::chrono::seconds(60));

    StrategyCaptureSink sink;
    google::AddLogSink(&sink);

    std::vector<char> out(payload.size(), '\0');
    std::vector<Slice> read_slices;
    read_slices.emplace_back(Slice{out.data(), out.size()});

    auto get = runtime_.client->Get(key, query_result, read_slices);

    google::RemoveLogSink(&sink);

    ASSERT_TRUE(get.has_value()) << "Get failed: " << toString(get.error());
    ASSERT_EQ(std::memcmp(out.data(), payload.data(), out.size()), 0);
    EXPECT_EQ(sink.strategy(), "LOCAL_MEMCPY");
}

INSTANTIATE_TEST_SUITE_P(
    AllModes, HotCacheRedirectStrategyTest,
    ::testing::Values(HandshakeMode::P2P, HandshakeMode::Metadata),
    [](const ::testing::TestParamInfo<HandshakeMode>& info) {
        switch (info.param) {
            case HandshakeMode::P2P:
                return "P2P";
            case HandshakeMode::Metadata:
                return "Metadata";
        }
        return "Unknown";
    });

}  // namespace testing
}  // namespace mooncake
