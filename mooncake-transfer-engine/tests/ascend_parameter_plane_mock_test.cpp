#include <gtest/gtest.h>
#include <acl/acl.h>
#include <atomic>
#include <array>
#include <thread>
#include <vector>
#include "transport/rdma_transport/ascend_parameter_plane.h"

namespace {
std::atomic<int> ub_registers{0}, ub_unregisters{0}, mr_registers{0},
    mr_unregisters{0};
std::atomic<bool> fail_deregister{false};
int pointer_device = 7;
}  // namespace

extern "C" aclError aclrtPointerGetAttributes(const void*,
                                              aclrtPtrAttributes* attr) {
    attr->location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    attr->location.id = pointer_device;
    return ACL_SUCCESS;
}
extern "C" int halMemRegUbSegment(uint32_t, uint64_t, uint64_t) {
    ++ub_registers;
    return 0;
}
extern "C" int halMemUnRegUbSegment(uint32_t, uint64_t) {
    ++ub_unregisters;
    return 0;
}
extern "C" int __wrap_ibv_query_device(ibv_context*, ibv_device_attr* attr) {
    attr->max_mr_size = 4096;
    return 0;
}
extern "C" ibv_mr* __wrap_ibv_reg_mr(ibv_pd* pd, void* addr, size_t length,
                                     int) {
    auto* mr = new ibv_mr{};
    mr->pd = pd;
    mr->addr = addr;
    mr->length = length;
    mr->lkey = ++mr_registers;
    mr->rkey = mr->lkey + 100;
    return mr;
}
extern "C" ibv_mr* __wrap_ibv_reg_mr_iova2(ibv_pd* pd, void* addr,
                                           size_t length, uint64_t,
                                           unsigned int access) {
    return __wrap_ibv_reg_mr(pd, addr, length, access);
}
extern "C" int __wrap_ibv_dereg_mr(ibv_mr* mr) {
    if (fail_deregister) return EIO;
    ++mr_unregisters;
    delete mr;
    return 0;
}

namespace mooncake {
class AscendParameterPlaneTestPeer {
   public:
    static void Enable() {
        auto& plane = AscendParameterPlane::GetInstance();
        plane.Shutdown();
        plane.initialization_attempted_ = true;
        plane.initialization_result_ = 0;
        plane.enabled_ = true;
        plane.device_.logical_device_id = 7;
    }
};
namespace {
class AscendRegistrationTest : public testing::Test {
   protected:
    void SetUp() override {
        AscendParameterPlaneTestPeer::Enable();
        ub_registers = ub_unregisters = mr_registers = mr_unregisters = 0;
        fail_deregister = false;
        pointer_device = 7;
        pd.context = &context;
        other_pd.context = &context;
    }
    void TearDown() override {
        fail_deregister = false;
        AscendParameterPlane::GetInstance().Shutdown();
    }
    AscendParameterPlane& plane = AscendParameterPlane::GetInstance();
    std::array<char, 8192> memory{};
    ibv_context context{};
    ibv_pd pd{}, other_pd{};
};

TEST_F(AscendRegistrationTest, SharedUbReferenceAndPerPdMrCache) {
    ASSERT_EQ(plane.RegisterRegion(memory.data(), memory.size()), 0);
    ASSERT_EQ(plane.RegisterRegion(memory.data(), memory.size()), 0);
    EXPECT_EQ(ub_registers, 1);
    ibv_mr *first = nullptr, *again = nullptr, *other = nullptr;
    ASSERT_EQ(plane.GetOrCreateMr(&pd, memory.data(), 512,
                                  IBV_ACCESS_LOCAL_WRITE, &first),
              0);
    ASSERT_EQ(plane.GetOrCreateMr(&pd, memory.data() + 512, 512,
                                  IBV_ACCESS_LOCAL_WRITE, &again),
              0);
    ASSERT_EQ(plane.GetOrCreateMr(&other_pd, memory.data(), 512,
                                  IBV_ACCESS_LOCAL_WRITE, &other),
              0);
    EXPECT_EQ(first, again);
    EXPECT_NE(first, other);
    EXPECT_EQ(mr_registers, 2);
    ASSERT_EQ(plane.UnregisterRegion(memory.data()), 0);
    EXPECT_EQ(mr_unregisters, 0);
    EXPECT_TRUE(plane.Contains(memory.data(), memory.size()));
    ASSERT_EQ(plane.UnregisterRegion(memory.data()), 0);
    EXPECT_EQ(mr_unregisters, 2);
    EXPECT_EQ(ub_unregisters, 1);
}
TEST_F(AscendRegistrationTest, FailedDeregistrationRetainsRangeForRetry) {
    ASSERT_EQ(plane.RegisterRegion(memory.data(), memory.size()), 0);
    ibv_mr* mr = nullptr;
    ASSERT_EQ(plane.GetOrCreateMr(&pd, memory.data(), 512,
                                  IBV_ACCESS_LOCAL_WRITE, &mr),
              0);
    fail_deregister = true;
    EXPECT_NE(plane.UnregisterRegion(memory.data()), 0);
    EXPECT_TRUE(plane.Contains(memory.data(), 512));
    EXPECT_EQ(ub_unregisters, 0);
    fail_deregister = false;
    EXPECT_EQ(plane.UnregisterRegion(memory.data()), 0);
    EXPECT_FALSE(plane.Contains(memory.data(), 512));
    EXPECT_EQ(ub_unregisters, 1);
}
TEST_F(AscendRegistrationTest, ConcurrentLookupsCreateOneMrAndInvalidateOnePd) {
    ASSERT_EQ(plane.RegisterRegion(memory.data(), memory.size()), 0);
    std::array<ibv_mr*, 8> results{};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < results.size(); ++i)
        threads.emplace_back([&, i] {
            EXPECT_EQ(plane.GetOrCreateMr(&pd, memory.data(), 512,
                                          IBV_ACCESS_LOCAL_WRITE, &results[i]),
                      0);
        });
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(mr_registers, 1);
    for (auto* mr : results) EXPECT_EQ(mr, results[0]);
    ibv_mr* other = nullptr;
    ASSERT_EQ(plane.GetOrCreateMr(&other_pd, memory.data(), 512,
                                  IBV_ACCESS_LOCAL_WRITE, &other),
              0);
    plane.InvalidatePd(&pd);
    EXPECT_EQ(mr_unregisters, 1);
    ibv_mr* again = nullptr;
    EXPECT_EQ(plane.GetOrCreateMr(&other_pd, memory.data(), 512,
                                  IBV_ACCESS_LOCAL_WRITE, &again),
              0);
    EXPECT_EQ(other, again);
    EXPECT_EQ(mr_registers, 2);
}
TEST_F(AscendRegistrationTest, RejectsForeignDeviceAndOverlappingRanges) {
    pointer_device = 8;
    EXPECT_EQ(plane.RegisterRegion(memory.data(), memory.size()), -EXDEV);
    pointer_device = 7;
    ASSERT_EQ(plane.RegisterRegion(memory.data(), memory.size()), 0);
    EXPECT_EQ(plane.RegisterRegion(memory.data() + 128, 1024), -EADDRINUSE);
    ibv_mr* mr = nullptr;
    EXPECT_EQ(plane.GetOrCreateMr(&pd, memory.data(), memory.size(),
                                  IBV_ACCESS_LOCAL_WRITE, &mr),
              -E2BIG);
}
}  // namespace
}  // namespace mooncake
