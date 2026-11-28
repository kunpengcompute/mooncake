#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef USE_NOF
#include "client_service.h"
#include "embedded_master.h"
#endif
#include "nof/nof_runtime.h"
#include "transfer_task.h"

namespace mooncake {
namespace {

// Mock implementing the interface contract: callbacks fire synchronously
// inside PollCompletion, matching SpdkInitiator's threading contract — which
// is what makes the tests valid.
class MockInitiator : public NVMeoFInitiator {
   public:
    NofSegmentHandle* OpenSegment(const std::string& ep) override {
        auto it = handles_.find(ep);
        if (it != handles_.end()) {
            return it->second;
        }
        // The opaque handle is a forward-declared type (its full definition
        // lives only in spdk_initiator.cpp) and the interface never
        // dereferences it — use the stable address of aligned storage as a
        // unique per-endpoint sentinel pointer (map key). Never new/delete
        // NofSegmentHandle.
        NofSegmentHandle* h = reinterpret_cast<NofSegmentHandle*>(
            &sentinels_[handles_.size() % sentinels_.size()]);
        handles_[ep] = h;
        return h;
    }

    bool ProbeSegment(const std::string&, uint32_t, std::string*) override {
        return probe_ok_;
    }

    uint32_t GetBlockSize(const NofSegmentHandle*) override {
        return block_size_;
    }

    int SubmitIO(NofSegmentHandle*, void*, uint64_t, uint64_t, NofIOOp,
                 NofIOAdaptor* adaptor) override {
        if (fail_submit_) return -1;
        if (submit_connection_error_ && !pending_.empty()) return -ENXIO;
        pending_.push_back(adaptor);
        return 0;
    }

    int64_t PollCompletion(NofSegmentHandle*,
                           uint32_t max_completions) override {
        if (poll_connection_error_) return -ENXIO;
        int64_t n = 0;
        while (!pending_.empty() &&
               (max_completions == 0 || n < max_completions)) {
            auto* adaptor = pending_.front();
            pending_.pop_front();
            NofIOCompletion c;
            c.success = !fail_io_;
            c.sc = 1;
            c.sct = 0;
            if (fail_io_) {
                c.error_string = "mock_io_error";
            }
            adaptor->cb(adaptor->ctx, c);
            ++n;
        }
        return poll_error_ == 0 ? n : poll_error_;
    }

    bool AbortSegment(NofSegmentHandle*) override {
        // Model synchronous cancellation without calling PollCompletion, which
        // deliberately keeps reporting a dead connection in these tests.
        while (!pending_.empty()) {
            auto* adaptor = pending_.front();
            pending_.pop_front();
            NofIOCompletion completion;
            completion.error_string = "controller_error:aborted";
            adaptor->cb(adaptor->ctx, completion);
        }
        aborts_.fetch_add(1, std::memory_order_release);
        return true;
    }

    ErrorCode RegisterMemory(void*, size_t) override { return ErrorCode::OK; }
    ErrorCode UnregisterMemory(void*) override { return ErrorCode::OK; }
    NofCapabilities GetCapabilities() const override { return {}; }

    uint32_t block_size_ = 4096;
    bool fail_submit_ = false;
    bool submit_connection_error_ = false;
    bool poll_connection_error_ = false;
    std::atomic<int> aborts_{0};
    bool fail_io_ = false;
    bool probe_ok_ = true;
    int poll_error_ = 0;
    std::deque<NofIOAdaptor*> pending_;
    std::map<std::string, NofSegmentHandle*> handles_;
    std::array<uint64_t, 16> sentinels_{};
};

class VectorMockInitiator : public MockInitiator {
   public:
    int SubmitIOV(NofSegmentHandle* handle,
                  const std::vector<NofBuffer>& buffers, uint64_t byte_offset,
                  uint64_t byte_length, NofIOOp op, NofIOAdaptor* adaptor,
                  size_t buffer_offset) override {
        NofSglCursor cursor;
        cursor.buffers = buffers;
        cursor.start = buffer_offset;
        cursor.length = byte_length;
        cursor.Reset(0);
        return SubmitVector(
            handle, byte_offset, byte_length, op, adaptor,
            [&](void** addr, uint32_t* len) { return cursor.Next(addr, len); });
    }

    int SubmitIOV(NofSegmentHandle* handle, uint64_t byte_offset,
                  uint64_t byte_length, NofIOOp op,
                  NofSglIOAdaptor* adaptor) override {
        adaptor->reset(adaptor->ctx, 0);
        return SubmitVector(handle, byte_offset, byte_length, op, adaptor,
                            [&](void** addr, uint32_t* len) {
                                return adaptor->next(adaptor->ctx, addr, len);
                            });
    }

    NofCapabilities GetCapabilities() const override {
        NofCapabilities caps;
        caps.supports_sgl = true;
        return caps;
    }
    std::vector<uint8_t> received;
    size_t vector_calls{0};

   private:
    template <typename Next>
    int SubmitVector(NofSegmentHandle* handle, uint64_t byte_offset,
                     uint64_t byte_length, NofIOOp op, NofIOAdaptor* adaptor,
                     Next next) {
        ++vector_calls;
        if (fail_submit_) return -ENOMEM;
        if (byte_offset > received.size() ||
            byte_length > received.size() - byte_offset) {
            return -EINVAL;
        }
        size_t copied = 0;
        while (copied < byte_length) {
            void* address = nullptr;
            uint32_t length = 0;
            if (next(&address, &length) != 0 || length == 0 ||
                length > byte_length - copied) {
                return -EINVAL;
            }
            auto* stored = received.data() + byte_offset + copied;
            if (op == NofIOOp::kRead) {
                std::memcpy(address, stored, length);
            } else {
                std::memcpy(stored, address, length);
            }
            copied += length;
        }
        return SubmitIO(handle, nullptr, byte_offset, byte_length, op, adaptor);
    }
};

#ifdef USE_NOF
TEST(NofClientTest, BatchPutAndGetWithDiscontiguousSlices) {
    EmbeddedMaster master;
    ASSERT_TRUE(master.Start(InProcMasterConfigBuilder().build()));

    NoFSegment segment;
    segment.id = generate_uuid();
    segment.name = "mock-ssd";
    segment.base = 0;
    segment.size = 16 * 1024 * 1024;
    segment.te_endpoint = "mock-nof-endpoint";
    ASSERT_TRUE(master.service()
                    ->MountNoFSegment(segment, generate_uuid())
                    .has_value());

    auto initiator = std::make_shared<VectorMockInitiator>();
    initiator->received.resize(segment.size);
    auto client = Client::Create("127.0.0.1", "P2PHANDSHAKE", "tcp",
                                 std::nullopt, master.master_address(), nullptr,
                                 {}, "default", initiator);
    ASSERT_TRUE(client.has_value());

    // An object spans two buffers, with an untouched gap in the middle.
    constexpr size_t block = 4096;
    alignas(block) std::array<uint8_t, 3 * block> source{}, destination{};
    std::fill_n(source.begin(), block, 0x31);
    std::fill_n(source.begin() + 2 * block, block, 0x72);
    destination.fill(0xff);
    const std::vector<std::string> keys{"discontiguous-nof-key"};
    std::vector<std::vector<Slice>> put_slices{
        {{source.data(), block}, {source.data() + 2 * block, block}}};
    ReplicateConfig config;
    config.replica_num = 0;
    config.nof_replica_num = 1;
    config.replica_counts_explicit = true;
    auto put = (*client)->BatchPut(keys, put_slices, config);
    ASSERT_EQ(put.size(), 1u);
    ASSERT_TRUE(put.front().has_value());

    std::unordered_map<std::string, std::vector<Slice>> get_slices{
        {keys.front(),
         {{destination.data(), block},
          {destination.data() + 2 * block, block}}}};
    auto get = (*client)->BatchGet(keys, get_slices);
    ASSERT_EQ(get.size(), 1u);
    ASSERT_TRUE(get.front().has_value());
    EXPECT_TRUE(std::equal(source.begin(), source.begin() + block,
                           destination.begin()));
    EXPECT_TRUE(std::equal(source.begin() + 2 * block, source.end(),
                           destination.begin() + 2 * block));
    EXPECT_TRUE(std::all_of(destination.begin() + block,
                            destination.begin() + 2 * block,
                            [](uint8_t byte) { return byte == 0xff; }));
}
#endif

TEST(NofWorkerPoolTest, VectorBuffersKeepOffsetsAcrossChunks) {
    auto mock = std::make_shared<VectorMockInitiator>();
    constexpr size_t size = 2 * 1024 * 1024;
    constexpr size_t offset = 8192;
    mock->received.resize(offset + size);
    std::vector<uint8_t> first(2048, 0x31), second(size - 2048, 0x72);
    {
        NofWorkerPool pool(mock, 0);
        auto* seg = mock->OpenSegment("vector");
        auto state = std::make_shared<NofOperationState>();
        NofTask task(seg, nullptr, offset, size, NofIOOp::kWrite, state);
        task.buffers = {{first.data(), first.size()},
                        {second.data(), second.size()}};
        pool.submitTask(std::move(task));
        state->wait_for_completion();
        ASSERT_EQ(state->get_result(), ErrorCode::OK);
    }
    EXPECT_GE(mock->vector_calls, 1u);
    EXPECT_TRUE(std::equal(first.begin(), first.end(),
                           mock->received.begin() + offset));
    EXPECT_TRUE(std::equal(second.begin(), second.end(),
                           mock->received.begin() + offset + first.size()));
    EXPECT_TRUE(std::all_of(mock->received.begin(),
                            mock->received.begin() + offset,
                            [](auto byte) { return byte == 0; }));
}

TEST(NofWorkerPoolTest, VectorSubmitFailureCompletesWithoutOutstandingIO) {
    auto mock = std::make_shared<VectorMockInitiator>();
    mock->fail_submit_ = true;
    NofWorkerPool pool(mock, 0);
    auto state = std::make_shared<NofOperationState>();
    std::array<uint8_t, 8192> buffer{};
    NofTask task(mock->OpenSegment("failed-vector"), nullptr, 0, buffer.size(),
                 NofIOOp::kWrite, state);
    task.buffers = {{buffer.data(), 4096}, {buffer.data() + 4096, 4096}};
    pool.submitTask(std::move(task));
    state->wait_for_completion();
    EXPECT_EQ(state->get_result(), ErrorCode::TRANSFER_FAIL);
}

TEST(NofInterfaceTest, UnsupportedDeviceMemoryDoesNotRegisterHostPages) {
    MockInitiator mock;
    std::array<uint8_t, 8> buffer{};
    EXPECT_EQ(mock.RegisterDeviceMemory(buffer.data(), buffer.size()),
              ErrorCode::INVALID_PARAMS);
    EXPECT_EQ(
        mock.SubmitIOV(nullptr, {{buffer.data(), 4}, {buffer.data() + 4, 4}}, 0,
                       8, NofIOOp::kRead, nullptr),
        -ENOTSUP);
}

TEST(NofSglCursorTest, ResetAndBoundedRangeCrossBufferBoundary) {
    std::array<char, 16> first{}, second{};
    NofSglCursor cursor;
    cursor.buffers = {{first.data(), first.size()},
                      {second.data(), second.size()}};
    cursor.start = 12;
    cursor.length = 12;
    cursor.Reset(0);
    void* address = nullptr;
    uint32_t length = 0;
    ASSERT_EQ(cursor.Next(&address, &length), 0);
    EXPECT_EQ(address, first.data() + 12);
    EXPECT_EQ(length, 4u);
    ASSERT_EQ(cursor.Next(&address, &length), 0);
    EXPECT_EQ(address, second.data());
    EXPECT_EQ(length, 8u);
    EXPECT_EQ(cursor.Next(&address, &length), -EINVAL);
    cursor.Reset(6);
    ASSERT_EQ(cursor.Next(&address, &length), 0);
    EXPECT_EQ(address, second.data() + 2);
    EXPECT_EQ(length, 6u);
}

TEST(NofSglCursorTest, HostRdmaEntriesStayInsideRegistrationPages) {
    constexpr size_t kIoBytes = 128 * 1024;
    constexpr size_t kPageTail = 32 * 1024;
    constexpr uint32_t kResumeOffset = 4096;
    std::vector<uint8_t> backing(kNofRegistrationPageSize + kIoBytes);
    void* boundary = backing.data() + kPageTail;
    size_t available = backing.size() - kPageTail;
    ASSERT_NE(std::align(kNofRegistrationPageSize, kIoBytes - kPageTail,
                         boundary, available),
              nullptr);
    auto* start = static_cast<uint8_t*>(boundary) - kPageTail;

    NofSglCursor cursor;
    cursor.buffers = {{start, kIoBytes}};
    cursor.length = kIoBytes;
    cursor.split_at_registration_page = true;
    cursor.Reset(0);

    void* address = nullptr;
    uint32_t length = 0;
    ASSERT_EQ(cursor.Next(&address, &length), 0);
    EXPECT_EQ(address, start);
    EXPECT_EQ(length, kPageTail);
    ASSERT_EQ(cursor.Next(&address, &length), 0);
    EXPECT_EQ(address, boundary);
    EXPECT_EQ(length, kIoBytes - kPageTail);
    EXPECT_EQ(cursor.Next(&address, &length), -EINVAL);

    cursor.Reset(kResumeOffset);
    ASSERT_EQ(cursor.Next(&address, &length), 0);
    EXPECT_EQ(address, start + kResumeOffset);
    EXPECT_EQ(length, kPageTail - kResumeOffset);
    ASSERT_EQ(cursor.Next(&address, &length), 0);
    EXPECT_EQ(address, boundary);
    EXPECT_EQ(length, kIoBytes - kPageTail);
}

TEST(NofWorkerPoolTest, SubmitAndComplete) {
    auto mock = std::make_shared<MockInitiator>();
    NofWorkerPool pool(mock, 0);
    auto* seg = mock->OpenSegment("trtype:TCP traddr:127.0.0.1 ns:1");
    auto state = std::make_shared<NofOperationState>();
    std::vector<uint8_t> buf(8192);
    NofTask task(seg, buf.data(), /*byte_off=*/0, /*byte_len=*/8192,
                 NofIOOp::kWrite, state);
    pool.submitTask(std::move(task));
    state->wait_for_completion();
    EXPECT_TRUE(state->is_completed());
    EXPECT_EQ(state->get_result(), ErrorCode::OK);
}

TEST(NofWorkerPoolTest, FailurePropagatesErrorDetail) {
    auto mock = std::make_shared<MockInitiator>();
    mock->fail_io_ = true;
    NofWorkerPool pool(mock, 0);
    auto* seg = mock->OpenSegment("trtype:TCP traddr:127.0.0.1 ns:1");
    auto state = std::make_shared<NofOperationState>();
    std::vector<uint8_t> buf(8192);
    NofTask task(seg, buf.data(), 0, 8192, NofIOOp::kWrite, state);
    pool.submitTask(std::move(task));
    state->wait_for_completion();
    EXPECT_TRUE(state->is_completed());
    EXPECT_EQ(state->get_result(), ErrorCode::TRANSFER_FAIL);
    EXPECT_EQ(state->error_detail(), "mock_io_error");
}

TEST(NofWorkerPoolTest, DisconnectCompletesPartiallySubmittedWriteOnce) {
    auto mock = std::make_shared<MockInitiator>();
    mock->fail_io_ = true;
    mock->poll_error_ = -ENXIO;
    const NofQos qos(mock->block_size_);
    // The first poll aborts every submitted sub-I/O while the last block is
    // still waiting in the QoS queue.
    const size_t size =
        (static_cast<size_t>(qos.inflight_blocks_limit) + 1) * qos.block_size;
    std::vector<uint8_t> buffer(size);
    auto* seg = mock->OpenSegment("disconnected-write");
    auto state = std::make_shared<NofOperationState>();
    {
        NofWorkerPool pool(mock, 0);
        pool.submitTask(NofTask(seg, buffer.data(), 0, buffer.size(),
                                NofIOOp::kWrite, state));
        state->wait_for_completion();
    }
    // Joining the worker also exercises removal from the QoS queue.
    EXPECT_EQ(state->get_result(), ErrorCode::TRANSFER_FAIL);
    EXPECT_EQ(state->error_detail(), "mock_io_error");
}

TEST(NofWorkerPoolTest, SubmitFailureFailsTask) {
    auto mock = std::make_shared<MockInitiator>();
    mock->fail_submit_ = true;
    NofWorkerPool pool(mock, 0);
    auto* seg = mock->OpenSegment("trtype:TCP traddr:127.0.0.1 ns:1");
    auto state = std::make_shared<NofOperationState>();
    std::vector<uint8_t> buf(8192);
    NofTask task(seg, buf.data(), 0, 8192, NofIOOp::kWrite, state);
    pool.submitTask(std::move(task));
    state->wait_for_completion();
    EXPECT_TRUE(state->is_completed());
    EXPECT_EQ(state->get_result(), ErrorCode::TRANSFER_FAIL);
}

// A dead target must fail both fully-submitted and still-queued operations,
// drain their callbacks, and let the worker destructor join.
TEST(NofWorkerPoolTest, ConnectionFailureDrainsCallbacksAndRejectsStaleHandle) {
    auto mock = std::make_shared<MockInitiator>();
    mock->poll_connection_error_ = true;
    NofWorkerPool pool(mock, 0);
    auto* seg = mock->OpenSegment("failed");
    std::vector<uint8_t> buffer(8192);
    auto first = std::make_shared<NofOperationState>();
    pool.submitTask(
        NofTask(seg, buffer.data(), 0, buffer.size(), NofIOOp::kWrite, first));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!first->is_completed() ||
            mock->aborts_.load(std::memory_order_acquire) == 0) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(first->is_completed());
    EXPECT_EQ(first->get_result(), ErrorCode::TRANSFER_FAIL);
    EXPECT_GT(mock->aborts_.load(std::memory_order_acquire), 0);
    auto second = std::make_shared<NofOperationState>();
    pool.submitTask(
        NofTask(seg, buffer.data(), 0, buffer.size(), NofIOOp::kRead, second));
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!second->is_completed() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(second->is_completed());
    EXPECT_EQ(second->get_result(), ErrorCode::TRANSFER_FAIL);
}

TEST(NofWorkerPoolTest, ConnectionFailureDuringPartialSubmissionDrainsTask) {
    auto mock = std::make_shared<MockInitiator>();
    mock->submit_connection_error_ = true;
    NofWorkerPool pool(mock, 0);
    auto* seg = mock->OpenSegment("partial");
    // Larger than a submit chunk: first submission succeeds, second fails.
    std::vector<uint8_t> buffer(4 * kDefaultNofSubmitChunkBytes);
    auto state = std::make_shared<NofOperationState>();
    pool.submitTask(
        NofTask(seg, buffer.data(), 0, buffer.size(), NofIOOp::kWrite, state));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!state->is_completed() ||
            mock->aborts_.load(std::memory_order_acquire) == 0) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(state->is_completed());
    EXPECT_EQ(state->get_result(), ErrorCode::TRANSFER_FAIL);
    EXPECT_GT(mock->aborts_.load(std::memory_order_acquire), 0);
}

TEST(NofRuntimeTest, NonNofBuildReturnsNullInitiator) {
#ifndef USE_NOF
    auto rt = CreateNofRuntime();
    EXPECT_EQ(rt.initiator, nullptr);
    EXPECT_NE(rt.dma_allocator, nullptr);
#endif
}

}  // namespace
}  // namespace mooncake
