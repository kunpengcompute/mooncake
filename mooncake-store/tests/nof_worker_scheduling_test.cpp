// CPU-only regression test of the real NoF worker loop with a fake SPDK
// completion source. Do not link spdk_wrapper.cpp or mooncake_store here.
#include "transfer_task.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <thread>

namespace {
struct Completion {
    spdk_nvme_cmd_cb callback;
    void* context;
};
std::deque<Completion> pending;
std::vector<uint32_t> submitted_blocks;
unsigned polls = 0;
bool fail_submit = false;
}

extern "C" const char* spdk_nvme_cpl_get_status_string(
    const struct spdk_nvme_status*) {
    return "mock completion";
}

namespace mooncake {
struct ctrlr_info {};
SpdkWrapper::SpdkWrapper() = default;
SpdkWrapper::~SpdkWrapper() = default;
SpdkWrapper& SpdkWrapper::GetInstance() {
    static SpdkWrapper instance;
    return instance;
}
uint32_t SpdkWrapper::GetBlockSize(const nof_seg_handle*) { return 512; }
int SpdkWrapper::SubmitRequestVExt(
    const nof_seg_handle*, uint64_t, uint32_t blocks, int,
    spdk_nvme_cmd_cb callback, void* context,
    spdk_nvme_req_reset_sgl_cb reset, spdk_nvme_req_next_sge_cb next,
    SpdkNofMemoryKind, spdk_nvme_ns_cmd_ext_io_opts*) {
    if (fail_submit) return -ENOMEM;
    reset(context, 0);
    uint32_t remaining = blocks * 512;
    while (remaining) {
        void* address = nullptr;
        uint32_t length = 0;
        assert(next(context, &address, &length) == 0);
        assert(address && length && length <= remaining);
        remaining -= length;
    }
    submitted_blocks.push_back(blocks);
    pending.push_back({callback, context});
    return 0;
}
int64_t SpdkWrapper::NvmePollProcessCompletion(nof_seg_handle*, uint32_t) {
    ++polls;
    int64_t completed = 0;
    while (!pending.empty()) {
        auto item = pending.front();
        pending.pop_front();
        spdk_nvme_cpl completion{};
        item.callback(item.context, &completion);
        ++completed;
    }
    return completed;
}
}  // namespace mooncake

int main(int argc, char** argv) {
    using namespace mooncake;
    assert(argc == 2);
    const std::string mode = argv[1];
    const bool small_limit = mode == "small_limit";
    fail_submit = mode == "submit_failure";
    assert(mode == "partial_credit" || small_limit || fail_submit);
    setenv("MC_NOF_WORKERS", "1", 1);
    setenv("MC_NOF_SUBMIT_CHUNK_BYTES", "2048", 1);  // Four sectors.
    setenv("MC_NOF_INFLIGHT_BYTES_LIMIT", small_limit ? "1024" : "3072", 1);
    const uint32_t total_blocks = small_limit ? 9 : 10;
    std::vector<char> data(total_blocks * 512);
    auto state = std::make_shared<SpdkNofOperationState>();
    {
        SpdkNofWorkerPool pool;
        pool.submitTask(SpdkNofTask(
            reinterpret_cast<nof_seg_handle*>(1), {{data.data(), data.size()}},
            0, total_blocks, kSpdkNofOpRead, SpdkNofMemoryKind::HOST_DMA,
            {}, state));
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (!state->is_completed()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "Worker failed to reach completion polling\n";
                std::_Exit(1);  // A regressed destructor would also hang.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(state->get_result() == (fail_submit ? ErrorCode::TRANSFER_FAIL
                                                  : ErrorCode::OK));
    }
    assert(pending.empty());
    if (fail_submit) {
        assert(submitted_blocks.empty());
    } else {
        assert(polls > 0);
        const std::vector<uint32_t> expected =
            small_limit ? std::vector<uint32_t>{2, 2, 2, 2, 1}
                        : std::vector<uint32_t>{4, 4, 2};
        assert(submitted_blocks == expected);
    }
    std::cout << mode << " passed\n";
}
