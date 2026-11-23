#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include "real_client.h"
#include "device/accelerator_registry.h"
#include "test_server_helpers.h"

namespace mooncake {
class AscendStoreTestPeer {
   public:
    static void EnableStaging(RealClient &client) {
        client.ascend_data_staging_ = true;
    }
    static void EnableNofHostBuffers(RealClient &client) {
        EnableStaging(client);
        client.ascend_nof_host_buffers_ = true;
    }
    static void EnableDirect(RealClient &client,
                             std::shared_ptr<NVMeoFInitiator> initiator) {
        client.ascend_data_staging_ = false;
        client.ascend_parameter_direct_ = true;
        client.nof_runtime_.initiator = std::move(initiator);
    }
    static bool PoolIsFree(RealClient &client) {
        return client.client_buffer_allocator_
            ->allocate(client.client_buffer_allocator_->size())
            .has_value();
    }
};
namespace {
class FakeAscend final : public device::AcceleratorDevice {
   public:
    std::vector<std::pair<void *, size_t>> ranges;
    mutable std::atomic<size_t> d2h{0}, h2d{0};
    mutable std::atomic<bool> fail{false};
    mutable std::promise<void> *copy_entered{nullptr};
    mutable std::shared_future<void> copy_release;
    device::AcceleratorVendor Vendor() const override {
        return device::AcceleratorVendor::kAscend;
    }
    bool Available(bool = false) const override { return true; }
    device::PointerInfo QueryPointer(const void *ptr) const override {
        auto address = reinterpret_cast<uintptr_t>(ptr);
        for (auto [base, size] : ranges) {
            auto start = reinterpret_cast<uintptr_t>(base);
            if (address >= start && address - start < size)
                return {.kind = device::MemoryKind::kDevice, .device_id = -1};
        }
        return {.kind = device::MemoryKind::kHost, .device_id = -1};
    }
    bool ValidateRange(const void *ptr, size_t length) const override {
        auto address = reinterpret_cast<uintptr_t>(ptr);
        for (auto [base, size] : ranges) {
            auto start = reinterpret_cast<uintptr_t>(base);
            if (address >= start && length <= size &&
                address - start <= size - length)
                return true;
        }
        return false;
    }
    int32_t CurrentDeviceId() const override { return -1; }
    void SetContext(int32_t) const override {}
    bool Copy(void *dst, const void *src, size_t size,
              device::CopyDirection direction) const override {
        if (direction == device::CopyDirection::kDeviceToHost) d2h += size;
        if (direction == device::CopyDirection::kHostToDevice) h2d += size;
        if (copy_entered) {
            copy_entered->set_value();
            copy_release.wait();
        }
        if (fail) return false;
        std::memcpy(dst, src, size);
        return true;
    }
    PinnedHostBuffer AllocatePinnedHost(size_t) const override { return {}; }
};
class RegistrationProbe : public NVMeoFInitiator {
   public:
    int host_registrations{0}, device_registrations{0}, unregisters{0};
    bool fail_register{false}, fail_unregister{false};
    bool fail_host_register{false};
    NofSegmentHandle *OpenSegment(const std::string &) override {
        return nullptr;
    }
    bool ProbeSegment(const std::string &, uint32_t, std::string *) override {
        return false;
    }
    uint32_t GetBlockSize(const NofSegmentHandle *) override { return 4096; }
    int SubmitIO(NofSegmentHandle *, void *, uint64_t, uint64_t, NofIOOp,
                 NofIOAdaptor *) override {
        return -ENOTSUP;
    }
    int64_t PollCompletion(NofSegmentHandle *, uint32_t) override { return 0; }
    ErrorCode RegisterMemory(void *, size_t) override {
        ++host_registrations;
        return fail_host_register ? ErrorCode::INVALID_PARAMS : ErrorCode::OK;
    }
    ErrorCode RegisterDeviceMemory(void *, size_t) override {
        ++device_registrations;
        return fail_register ? ErrorCode::INVALID_PARAMS : ErrorCode::OK;
    }
    ErrorCode UnregisterMemory(void *) override {
        ++unregisters;
        return fail_unregister ? ErrorCode::INTERNAL_ERROR : ErrorCode::OK;
    }
    NofCapabilities GetCapabilities() const override { return {}; }
};
FakeAscend fake;
device::AcceleratorDeviceRegistrar registration(fake);

class AscendStoreStagingTest : public ::testing::Test {
   protected:
    void SetUp() override {
        fake.d2h = 0;
        fake.h2d = 0;
        fake.fail = false;
        fake.copy_entered = nullptr;
        source.resize(8192);
        destination.resize(8192);
        second.resize(8192);
        fake.ranges = {{source.data(), source.size()},
                       {destination.data(), destination.size()},
                       {second.data(), second.size()}};
        ASSERT_TRUE(master.Start(InProcMasterConfigBuilder().build()));
        receiver = RealClient::create();
        sender = RealClient::create();
        ASSERT_EQ(
            receiver->setup_real("127.0.0.1", "P2PHANDSHAKE", 16 << 20, 1 << 20,
                                 "tcp", "", master.master_address()),
            0);
        ASSERT_EQ(sender->setup_real("127.0.0.1", "P2PHANDSHAKE", 0, 64 << 10,
                                     "tcp", "", master.master_address()),
                  0);
        AscendStoreTestPeer::EnableStaging(*sender);
        ASSERT_EQ(sender->register_buffer(source.data(), source.size()), 0);
        ASSERT_EQ(
            sender->register_buffer(destination.data(), destination.size()), 0);
        ASSERT_EQ(sender->register_buffer(second.data(), second.size()), 0);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<char>(i % 127);
    }
    void TearDown() override {
        fake.copy_entered = nullptr;
        if (sender) sender->tearDownAll();
        if (receiver) receiver->tearDownAll();
        sender.reset();
        receiver.reset();
        master.Stop();
        fake.ranges.clear();
    }
    testing::InProcMaster master;
    std::shared_ptr<RealClient> sender, receiver;
    std::vector<char> source, destination, second;
};
TEST_F(AscendStoreStagingTest, RemoteMultiSliceRoundTripAndDuplicateRead) {
    auto put = sender->batch_put_from_multi_buffers(
        {"kv"}, {{source.data(), source.data() + 4096}}, {{4096, 4096}});
    ASSERT_EQ(put, std::vector<int>{0});
    EXPECT_EQ(fake.d2h, 8192);
    auto get = sender->batch_get_into_multi_buffers(
        {"kv", "kv"},
        {{destination.data(), destination.data() + 3072}, {second.data()}},
        {{3072, 5120}, {8192}}, false);
    ASSERT_EQ(get.size(), 2);
    EXPECT_EQ(get[0], 8192);
    EXPECT_EQ(get[1], 8192);
    EXPECT_EQ(source, destination);
    EXPECT_EQ(source, second);
    EXPECT_GE(fake.h2d, 16384);
    EXPECT_TRUE(AscendStoreTestPeer::PoolIsFree(*sender));
}
TEST_F(AscendStoreStagingTest,
       CopyFailureReturnsBufferAndRejectsInvalidRanges) {
    EXPECT_NE(sender->register_buffer(source.data() + 4096, 8192), 0);
    auto bad = sender->batch_put_from_multi_buffers(
        {"bad-range"}, {{source.data() + 4096}}, {{8192}});
    ASSERT_EQ(bad.size(), 1);
    EXPECT_NE(bad[0], 0);
    fake.fail = true;
    auto failed = sender->batch_put_from_multi_buffers(
        {"failed-copy"}, {{source.data()}}, {{8192}});
    ASSERT_EQ(failed.size(), 1);
    EXPECT_NE(failed[0], 0);
    EXPECT_TRUE(AscendStoreTestPeer::PoolIsFree(*sender));
    fake.fail = false;
    auto success = sender->batch_put_from_multi_buffers(
        {"retry"}, {{source.data()}}, {{8192}});
    EXPECT_EQ(success, std::vector<int>{0});
}
TEST_F(AscendStoreStagingTest, UnregisterWaitsForActiveWrite) {
    std::promise<void> entered, release;
    fake.copy_release = release.get_future().share();
    fake.copy_entered = &entered;
    auto writing = std::async(std::launch::async, [&] {
        return sender->batch_put_from_multi_buffers({"busy"}, {{source.data()}},
                                                    {{8192}});
    });
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(10)),
              std::future_status::ready);
    auto unregistering = std::async(std::launch::async, [&] {
        return sender->unregister_buffer(source.data());
    });
    EXPECT_EQ(unregistering.wait_for(std::chrono::milliseconds(30)),
              std::future_status::timeout);
    release.set_value();
    EXPECT_EQ(writing.get(), std::vector<int>{0});
    EXPECT_EQ(unregistering.get(), 0);
    fake.copy_entered = nullptr;
}
TEST_F(AscendStoreStagingTest, RepeatedRegistrationNeedsMatchingUnregisters) {
    ASSERT_EQ(sender->register_buffer(source.data(), source.size()), 0);
    ASSERT_EQ(sender->unregister_buffer(source.data()), 0);
    EXPECT_EQ(sender->batch_put_from_multi_buffers(
                  {"still-registered"}, {{source.data()}}, {{source.size()}}),
              std::vector<int>{0});
    ASSERT_EQ(sender->unregister_buffer(source.data()), 0);
    EXPECT_NE(sender->batch_put_from_multi_buffers(
                  {"unregistered"}, {{source.data()}}, {{source.size()}})[0],
              0);
}
TEST_F(AscendStoreStagingTest,
       NoFUsesDeviceRegistrationAndRetainsFailedCleanup) {
    for (auto *ptr : {source.data(), destination.data(), second.data()})
        ASSERT_EQ(sender->unregister_buffer(ptr), 0);
    auto probe = std::make_shared<RegistrationProbe>();
    AscendStoreTestPeer::EnableDirect(*sender, probe);
    ASSERT_EQ(sender->register_buffer(source.data(), source.size()), 0);
    EXPECT_EQ(probe->device_registrations, 1);
    EXPECT_EQ(probe->host_registrations, 0);
    probe->fail_unregister = true;
    EXPECT_NE(sender->unregister_buffer(source.data()), 0);
    EXPECT_NE(sender->batch_put_from_multi_buffers(
                  {"cleanup-pending"}, {{source.data()}}, {{source.size()}})[0],
              0);
    probe->fail_unregister = false;
    EXPECT_EQ(sender->unregister_buffer(source.data()), 0);
    EXPECT_EQ(probe->unregisters, 2);
}
TEST_F(AscendStoreStagingTest, FailedDeviceRegistrationCanBeRetried) {
    for (auto *ptr : {source.data(), destination.data(), second.data()})
        ASSERT_EQ(sender->unregister_buffer(ptr), 0);
    auto probe = std::make_shared<RegistrationProbe>();
    probe->fail_register = true;
    AscendStoreTestPeer::EnableDirect(*sender, probe);
    EXPECT_NE(sender->register_buffer(source.data(), source.size()), 0);
    probe->fail_register = false;
    EXPECT_EQ(sender->register_buffer(source.data(), source.size()), 0);
    EXPECT_EQ(probe->device_registrations, 2);
    EXPECT_EQ(sender->unregister_buffer(source.data()), 0);
}

#ifdef USE_NOF
class HostRoundTripInitiator final : public RegistrationProbe {
   public:
    void *source{nullptr}, *destination{nullptr};
    std::array<char, 4096> storage{};

    NofSegmentHandle *OpenSegment(const std::string &) override {
        return reinterpret_cast<NofSegmentHandle *>(&handle_);
    }
    int SubmitIO(NofSegmentHandle *, void *buffer, uint64_t, uint64_t bytes,
                 NofIOOp op, NofIOAdaptor *adaptor) override {
        if (bytes != storage.size() || pending_) return -EINVAL;
        if (op == NofIOOp::kWrite && buffer == source) {
            std::memcpy(storage.data(), buffer, bytes);
        } else if (op == NofIOOp::kRead && buffer == destination) {
            std::memcpy(buffer, storage.data(), bytes);
        } else {
            return -EINVAL;
        }
        pending_ = adaptor;
        return 0;
    }
    int64_t PollCompletion(NofSegmentHandle *, uint32_t) override {
        if (!pending_) return 0;
        auto *adaptor = pending_;
        pending_ = nullptr;
        adaptor->cb(adaptor->ctx, NofIOCompletion{.success = true});
        return 1;
    }

   private:
    uint64_t handle_{0};
    NofIOAdaptor *pending_{nullptr};
};

TEST(AscendNofHostBuffersTest, ReusesRegisteredBuffersWithoutStaging) {
    testing::InProcMaster master;
    ASSERT_TRUE(master.Start(InProcMasterConfigBuilder().build()));
    NoFSegment segment;
    segment.id = generate_uuid();
    segment.name = "mock-ssd";
    segment.base = 0;
    segment.size = 16 << 20;
    segment.te_endpoint = "mock-nof-endpoint";
    ASSERT_TRUE(master.service()
                    ->MountNoFSegment(segment, generate_uuid())
                    .has_value());

    alignas(4096) std::array<char, 4096> source{}, destination{};
    source.fill('x');
    auto initiator = std::make_shared<HostRoundTripInitiator>();
    initiator->source = source.data();
    initiator->destination = destination.data();
    struct FactoryReset {
        ~FactoryReset() { RealClient::SetNofRuntimeFactoryForTesting(nullptr); }
    } factory_reset;
    RealClient::SetNofRuntimeFactoryForTesting([initiator] {
        return NofRuntime{initiator, std::make_shared<SystemDmaAllocator>()};
    });
    auto client = RealClient::create();
    ASSERT_EQ(client->setup_real("127.0.0.1", "P2PHANDSHAKE", 0, 0, "tcp",
                                 "", master.master_address()),
              0);
    AscendStoreTestPeer::EnableNofHostBuffers(*client);

    initiator->fail_host_register = true;
    ASSERT_NE(client->register_buffer(source.data(), source.size()), 0);
    initiator->fail_host_register = false;
    ASSERT_EQ(client->register_buffer(source.data(), source.size()), 0);
    ASSERT_EQ(client->register_buffer(destination.data(), destination.size()), 0);
    EXPECT_NE(client->put_from("memory", source.data(), source.size(), {}), 0);

    ReplicateConfig config;
    config.replica_num = 0;
    config.nof_replica_num = 1;
    config.replica_counts_explicit = true;
    ASSERT_EQ(client->batch_put_from({"kv"}, {source.data()}, {source.size()},
                                    config),
              std::vector<int>{0});
    ASSERT_EQ(client->batch_get_into({"kv"}, {destination.data()},
                                     {destination.size()}),
              std::vector<int64_t>{4096});
    EXPECT_EQ(source, destination);
    EXPECT_EQ(client->get_into("kv", destination.data(), destination.size()),
              4096);
    EXPECT_LT(client->get_into("kv", destination.data() + 1,
                               destination.size()),
              0);
    ASSERT_EQ(client->unregister_buffer(destination.data()), 0);
    EXPECT_LT(client->get_into("kv", destination.data(), destination.size()),
              0);
    EXPECT_EQ(client->tearDownAll(), 0);
}
#endif
}  // namespace
}  // namespace mooncake
