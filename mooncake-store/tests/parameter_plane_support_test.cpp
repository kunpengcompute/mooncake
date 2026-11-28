#include <gtest/gtest.h>
#include <future>
#include <chrono>
#include "transfer_task.h"
#include <ylt/struct_pack.hpp>
#include "client_buffer.h"
#include "replica.h"
#include "nof/io_buffer.h"

namespace mooncake {
TEST(ParameterPlaneSupport, AlignedAllocationsAreBoundedAndReusable) {
    auto pool = ClientBufferAllocator::create(64 * 1024, "tcp");
    auto first = pool->allocateAligned(4096, 4096);
    auto second = pool->allocateAligned(4096, 4096);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(first->ptr()) % 4096, 0);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(second->ptr()) % 4096, 0);
    EXPECT_NE(first->ptr(), second->ptr());
    EXPECT_FALSE(pool->allocateAligned(64 * 1024, 4096));
    EXPECT_FALSE(pool->allocateAligned(SIZE_MAX, 4096));
    EXPECT_FALSE(pool->allocateAligned(1, 3));
    first.reset();
    second.reset();
    EXPECT_TRUE(pool->allocate(64 * 1024));
}
TEST(ParameterPlaneSupport, VectorCursorResetsInsideSliceAndClipsRequest) {
    char a[8]{}, b[16]{};
    NofSglCursor cursor;
    cursor.buffers = {{a, sizeof(a)}, {b, sizeof(b)}};
    cursor.start = 4;
    cursor.length = 12;
    cursor.Reset(0);
    void* ptr = nullptr;
    uint32_t length = 0;
    ASSERT_EQ(cursor.Next(&ptr, &length), 0);
    EXPECT_EQ(ptr, a + 4);
    EXPECT_EQ(length, 4);
    ASSERT_EQ(cursor.Next(&ptr, &length), 0);
    EXPECT_EQ(ptr, b);
    EXPECT_EQ(length, 8);
    EXPECT_NE(cursor.Next(&ptr, &length), 0);
    cursor.Reset(6);
    ASSERT_EQ(cursor.Next(&ptr, &length), 0);
    EXPECT_EQ(ptr, b + 2);
    EXPECT_EQ(length, 6);
}
TEST(ParameterPlaneSupport, ClientFlagDoesNotChangeMasterWireFields) {
    ReplicateConfig config;
    config.replica_num = 2;
    config.nof_replica_num = 1;
    config.dfs_replica_num = 3;
    config.soft_pin_action = SoftPinAction::ENABLE;
    config.soft_pin_ttl_ms = 1234;
    config.host_id = "worker-host";
    config.group_ids = std::vector<std::string>{"group"};
    config.session_ids = std::vector<std::string>{"session"};
    config.preferred_hca_names = std::vector<std::string>{"remote_hca"};
    config.per_key_preferred_hca_names =
        std::vector<std::vector<std::string>>{{"key_hca"}};
    auto before = struct_pack::serialize(config);
    config.replica_counts_explicit = true;
    EXPECT_EQ(before, struct_pack::serialize(config));
    auto decoded = struct_pack::deserialize<ReplicateConfig>(before);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->dfs_replica_num, 3);
    EXPECT_EQ(decoded->soft_pin_action, SoftPinAction::ENABLE);
    EXPECT_EQ(decoded->soft_pin_ttl_ms.value(), 1234);
    EXPECT_EQ(decoded->host_id, "worker-host");
    EXPECT_EQ(decoded->group_ids->at(0), "group");
    ASSERT_TRUE(decoded->session_ids.has_value());
    EXPECT_EQ(decoded->session_ids.value().at(0), "session");
    EXPECT_EQ(decoded->preferred_hca_names.value().at(0), "remote_hca");
    EXPECT_EQ(decoded->per_key_preferred_hca_names.value().at(0).at(0),
              "key_hca");
}
TEST(ParameterPlaneSupport, CompletionWaitsUntilCallbackReleasesBatchPin) {
    TransferEngine engine(false);
    ASSERT_EQ(engine.init(P2PHANDSHAKE, "127.0.0.1:0", "", 0, "tcp"), 0);
    auto id = engine.allocateBatchID(1);
    auto& batch = Transport::toBatchDesc(id);
    batch.task_list.emplace_back();
    auto& task = batch.task_list.back();
    task.batch_id = id;
    task.slice_count = 1;
    task.success_slice_count = 1;
    task.is_finished = true;
    batch.active_completion_callbacks.store(1);
    TransferEngineOperationState state(engine, id, 1, true);
    auto completion = std::async(std::launch::async, [&] {
        state.wait_for_completion();
        return state.get_result();
    });
    EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(20)),
              std::future_status::timeout);
    batch.active_completion_callbacks.store(0);
    EXPECT_EQ(completion.get(), ErrorCode::OK);
}
}  // namespace mooncake
