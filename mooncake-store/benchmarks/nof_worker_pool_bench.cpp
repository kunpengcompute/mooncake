#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "spdk/spdk_wrapper.h"
#include "transfer_task.h"
#include "utils.h"

namespace {

constexpr uint64_t KiB = 1024;
constexpr uint64_t MiB = 1024 * KiB;
constexpr uint64_t GiB = 1024 * MiB;

DEFINE_string(
    endpoints, "",
    "Comma-separated NoF transport endpoints. To use multiple "
    "SpdkNofWorkerPool workers with the current implementation, provide "
    "multiple distinct endpoints/namespaces/targets.");
DEFINE_string(op, "read", "Benchmark operation: read, write, or mixed");
DEFINE_uint64(io_size, 128 * KiB, "I/O size in bytes. Must align to device block size.");
DEFINE_uint64(iodepth, 64, "Total in-flight I/Os maintained by the benchmark.");
DEFINE_uint64(duration_sec, 30, "Benchmark duration in seconds.");
DEFINE_uint64(report_interval_ms, 1000, "Statistics report interval in milliseconds.");
DEFINE_uint64(start_lba, 0, "Starting LBA offset within each endpoint.");
DEFINE_uint64(range_bytes, 1 * GiB,
              "Per-endpoint logical range in bytes used by the benchmark. "
              "Must align to block size.");
DEFINE_bool(random_lba, false, "Whether to choose LBA randomly inside the range.");
DEFINE_uint64(seed, 1, "Random seed used when --random_lba or --op=mixed.");
DEFINE_uint64(warmup_sec, 3, "Warmup duration in seconds before statistics collection.");
DEFINE_uint64(nof_workers, 0,
              "Override MC_NOF_WORKERS. 0 means keep environment/default.");
DEFINE_uint64(nof_submit_chunk_bytes, 0,
              "Override MC_NOF_SUBMIT_CHUNK_BYTES. 0 means keep environment/default.");
DEFINE_uint64(nof_inflight_bytes_limit, 0,
              "Override MC_NOF_INFLIGHT_BYTES_LIMIT. 0 means keep environment/default.");
DEFINE_int32(socket_id, -1, "NUMA socket for benchmark buffers. -1 lets SPDK choose.");
DEFINE_bool(fill_on_write, true, "Fill buffers with a deterministic pattern for write/mixed ops.");

enum class BenchOp {
    READ = 0,
    WRITE = 1,
    MIXED = 2,
};

BenchOp ParseBenchOp(const std::string &value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized == "read") {
        return BenchOp::READ;
    }
    if (normalized == "write") {
        return BenchOp::WRITE;
    }
    if (normalized == "mixed") {
        return BenchOp::MIXED;
    }
    throw std::invalid_argument("Invalid --op. Supported values: read, write, mixed");
}

void SetEnvU64IfRequested(const char *name, uint64_t value) {
    if (value == 0) {
        return;
    }
#ifdef _WIN32
    _putenv_s(name, std::to_string(value).c_str());
#else
    setenv(name, std::to_string(value).c_str(), 1);
#endif
}

std::string FormatBytesPerSecond(double bytes_per_sec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (bytes_per_sec >= static_cast<double>(GiB)) {
        oss << (bytes_per_sec / GiB) << " GiB/s";
    } else if (bytes_per_sec >= static_cast<double>(MiB)) {
        oss << (bytes_per_sec / MiB) << " MiB/s";
    } else if (bytes_per_sec >= static_cast<double>(KiB)) {
        oss << (bytes_per_sec / KiB) << " KiB/s";
    } else {
        oss.unsetf(std::ios::fixed);
        oss << bytes_per_sec << " B/s";
    }
    return oss.str();
}

struct EndpointContext {
    std::string endpoint;
    mooncake::nof_seg_handle *seg_handle{nullptr};
    uint32_t block_size{0};
    uint64_t range_blocks{0};
    uint64_t io_blocks{0};
    uint64_t next_block{0};
    std::mt19937_64 rng;

    EndpointContext(std::string ep, uint64_t seed_value)
        : endpoint(std::move(ep)), rng(seed_value) {}
};

struct Slot {
    void *buffer{nullptr};
    size_t buffer_size{0};
    std::shared_ptr<mooncake::SpdkNofOperationState> state;
    std::unique_ptr<mooncake::TransferFuture> future;
    uint64_t io_bytes{0};
    size_t endpoint_index{0};
    bool active{false};
};

struct BenchStats {
    uint64_t submitted_ops{0};
    uint64_t completed_ops{0};
    uint64_t failed_ops{0};
    uint64_t bytes{0};
};

uint64_t NextLba(EndpointContext &endpoint, bool random_lba) {
    if (endpoint.range_blocks <= endpoint.io_blocks) {
        return FLAGS_start_lba;
    }

    if (random_lba) {
        std::uniform_int_distribution<uint64_t> dist(
            0, endpoint.range_blocks - endpoint.io_blocks);
        return FLAGS_start_lba + dist(endpoint.rng);
    }

    uint64_t lba = FLAGS_start_lba + endpoint.next_block;
    endpoint.next_block += endpoint.io_blocks;
    if (endpoint.next_block + endpoint.io_blocks > endpoint.range_blocks) {
        endpoint.next_block = 0;
    }
    return lba;
}

int PickTaskOp(BenchOp op_mode, std::mt19937_64 &rng) {
    switch (op_mode) {
        case BenchOp::READ:
            return 0;
        case BenchOp::WRITE:
            return 1;
        case BenchOp::MIXED: {
            std::uniform_int_distribution<int> dist(0, 1);
            return dist(rng);
        }
    }
    return 0;
}

void FillPattern(void *buffer, size_t size, uint64_t seq) {
    auto *p = reinterpret_cast<uint8_t *>(buffer);
    for (size_t i = 0; i < size; ++i) {
        p[i] = static_cast<uint8_t>((seq + i) & 0xff);
    }
}

void SubmitSlot(Slot &slot, EndpointContext &endpoint, size_t endpoint_index,
                mooncake::SpdkNofWorkerPool &pool, BenchOp op_mode,
                std::mt19937_64 &rng, uint64_t submit_seq, BenchStats &stats,
                BenchStats &endpoint_stats) {
    int op = PickTaskOp(op_mode, rng);
    if (op == 1 && FLAGS_fill_on_write) {
        FillPattern(slot.buffer, slot.buffer_size, submit_seq);
    }

    uint64_t lba = NextLba(endpoint, FLAGS_random_lba);
    slot.io_bytes = slot.buffer_size;
    slot.endpoint_index = endpoint_index;
    slot.state = std::make_shared<mooncake::SpdkNofOperationState>();
    slot.future = std::make_unique<mooncake::TransferFuture>(slot.state);
    mooncake::SpdkNofTask task(endpoint.seg_handle, slot.buffer, lba,
                               static_cast<uint32_t>(endpoint.io_blocks), op,
                               slot.state);
    pool.submitTask(std::move(task));
    slot.active = true;
    ++stats.submitted_ops;
    ++endpoint_stats.submitted_ops;
}

}  // namespace

int main(int argc, char **argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    try {
        BenchOp op_mode = ParseBenchOp(FLAGS_op);
        if (FLAGS_endpoints.empty()) {
            LOG(ERROR) << "--endpoints is required";
            return 1;
        }
        if (FLAGS_iodepth == 0) {
            LOG(ERROR) << "--iodepth must be greater than 0";
            return 1;
        }
        if (FLAGS_duration_sec == 0) {
            LOG(ERROR) << "--duration_sec must be greater than 0";
            return 1;
        }

        SetEnvU64IfRequested("MC_NOF_WORKERS", FLAGS_nof_workers);
        SetEnvU64IfRequested("MC_NOF_SUBMIT_CHUNK_BYTES",
                             FLAGS_nof_submit_chunk_bytes);
        SetEnvU64IfRequested("MC_NOF_INFLIGHT_BYTES_LIMIT",
                             FLAGS_nof_inflight_bytes_limit);

        auto endpoint_strings = mooncake::splitString(FLAGS_endpoints);
        if (endpoint_strings.empty()) {
            LOG(ERROR) << "No valid endpoints parsed from --endpoints";
            return 1;
        }

        auto &wrapper = mooncake::SpdkWrapper::GetInstance();
        if (!wrapper.InitializeEnv()) {
            LOG(ERROR) << "Failed to initialize SPDK environment";
            return 1;
        }

        std::vector<EndpointContext> endpoints;
        endpoints.reserve(endpoint_strings.size());
        std::unordered_set<mooncake::nof_seg_handle *> unique_handles;
        for (size_t i = 0; i < endpoint_strings.size(); ++i) {
            EndpointContext endpoint(endpoint_strings[i], FLAGS_seed + i);
            endpoint.seg_handle = wrapper.OpenNofSegment(endpoint.endpoint);
            if (!endpoint.seg_handle) {
                LOG(ERROR) << "Failed to open NoF endpoint: " << endpoint.endpoint;
                return 1;
            }
            endpoint.block_size = wrapper.GetBlockSize(endpoint.seg_handle);
            if (endpoint.block_size == INVALID_BLOCK_SIZE ||
                endpoint.block_size == 0) {
                LOG(ERROR) << "Invalid block size for endpoint: "
                           << endpoint.endpoint;
                return 1;
            }
            if (FLAGS_io_size % endpoint.block_size != 0) {
                LOG(ERROR) << "--io_size=" << FLAGS_io_size
                           << " is not aligned to block size "
                           << endpoint.block_size << " for endpoint "
                           << endpoint.endpoint;
                return 1;
            }
            if (FLAGS_range_bytes % endpoint.block_size != 0) {
                LOG(ERROR) << "--range_bytes=" << FLAGS_range_bytes
                           << " is not aligned to block size "
                           << endpoint.block_size;
                return 1;
            }
            endpoint.io_blocks = FLAGS_io_size / endpoint.block_size;
            endpoint.range_blocks = FLAGS_range_bytes / endpoint.block_size;
            if (endpoint.range_blocks < endpoint.io_blocks) {
                LOG(ERROR) << "--range_bytes is smaller than one I/O for endpoint "
                           << endpoint.endpoint;
                return 1;
            }
            unique_handles.insert(endpoint.seg_handle);
            endpoints.push_back(std::move(endpoint));
        }

        LOG(INFO) << "Bench config: endpoints=" << endpoints.size()
                  << ", unique_handles=" << unique_handles.size()
                  << ", configured_nof_workers="
                  << (FLAGS_nof_workers == 0 ? mooncake::kDefaultSpdkNofWorkers
                                             : FLAGS_nof_workers)
                  << ", effective_worker_bindings<="
                  << std::min<uint64_t>(
                         unique_handles.size(),
                         FLAGS_nof_workers == 0 ? mooncake::kDefaultSpdkNofWorkers
                                                : FLAGS_nof_workers)
                  << ", io_size=" << FLAGS_io_size
                  << ", iodepth=" << FLAGS_iodepth
                  << ", warmup_sec=" << FLAGS_warmup_sec
                  << ", duration_sec=" << FLAGS_duration_sec;
        if (unique_handles.size() < endpoints.size()) {
            LOG(WARNING)
                << "Some endpoints resolved to the same nof_seg_handle. In the "
                   "current SpdkNofWorkerPool implementation, the same handle "
                   "binds to a single worker thread, so duplicate endpoints will "
                   "not increase worker parallelism.";
        }

        mooncake::SpdkNofWorkerPool pool;
        std::vector<Slot> slots(FLAGS_iodepth);
        for (auto &slot : slots) {
            slot.buffer_size = FLAGS_io_size;
            slot.buffer = wrapper.Alloc(slot.buffer_size, 0x1000, FLAGS_socket_id);
            if (!slot.buffer) {
                LOG(ERROR) << "Failed to allocate DMA buffer of size "
                           << slot.buffer_size;
                return 1;
            }
        }

        BenchStats total_stats;
        BenchStats warmup_base;
        std::vector<BenchStats> endpoint_stats(endpoints.size());
        std::vector<BenchStats> endpoint_warmup_base(endpoints.size());
        std::mt19937_64 op_rng(FLAGS_seed);
        uint64_t submit_seq = 0;

        for (size_t i = 0; i < slots.size(); ++i) {
            auto &slot = slots[i];
            size_t endpoint_index = i % endpoints.size();
            auto &endpoint = endpoints[endpoint_index];
            SubmitSlot(slot, endpoint, endpoint_index, pool, op_mode, op_rng,
                       submit_seq++, total_stats,
                       endpoint_stats[endpoint_index]);
        }

        auto start = std::chrono::steady_clock::now();
        auto warmup_end = start + std::chrono::seconds(FLAGS_warmup_sec);
        auto end = warmup_end + std::chrono::seconds(FLAGS_duration_sec);
        auto next_report =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(FLAGS_report_interval_ms);
        BenchStats last_report_stats = total_stats;
        std::vector<BenchStats> endpoint_last_report_stats = endpoint_stats;
        bool warmup_recorded = (FLAGS_warmup_sec == 0);
        if (warmup_recorded) {
            warmup_base = total_stats;
            endpoint_warmup_base = endpoint_stats;
        }

        while (true) {
            auto now = std::chrono::steady_clock::now();
            bool before_end = now < end;
            bool any_active = false;

            for (size_t i = 0; i < slots.size(); ++i) {
                auto &slot = slots[i];
                if (!slot.active) {
                    continue;
                }
                any_active = true;
                if (!slot.future || !slot.future->isReady()) {
                    continue;
                }

                mooncake::ErrorCode result = slot.future->get();
                slot.active = false;
                slot.future.reset();
                auto &endpoint_stat = endpoint_stats[slot.endpoint_index];
                if (result == mooncake::ErrorCode::OK) {
                    ++total_stats.completed_ops;
                    total_stats.bytes += slot.io_bytes;
                    ++endpoint_stat.completed_ops;
                    endpoint_stat.bytes += slot.io_bytes;
                } else {
                    ++total_stats.failed_ops;
                    ++endpoint_stat.failed_ops;
                }

                if (before_end) {
                    auto &endpoint = endpoints[slot.endpoint_index];
                    SubmitSlot(slot, endpoint, slot.endpoint_index, pool, op_mode,
                               op_rng, submit_seq++, total_stats,
                               endpoint_stat);
                    any_active = true;
                }
            }

            now = std::chrono::steady_clock::now();
            if (!warmup_recorded && now >= warmup_end) {
                warmup_base = total_stats;
                last_report_stats = total_stats;
                endpoint_warmup_base = endpoint_stats;
                endpoint_last_report_stats = endpoint_stats;
                warmup_recorded = true;
                LOG(INFO) << "Warmup finished, starting measurement window";
            }

            if (warmup_recorded && now >= next_report) {
                double sec =
                    static_cast<double>(FLAGS_report_interval_ms) / 1000.0;
                uint64_t delta_bytes = total_stats.bytes - last_report_stats.bytes;
                uint64_t delta_ops =
                    total_stats.completed_ops - last_report_stats.completed_ops;
                uint64_t delta_fail =
                    total_stats.failed_ops - last_report_stats.failed_ops;
                LOG(INFO) << "interval throughput="
                          << FormatBytesPerSecond(delta_bytes / sec)
                          << ", iops=" << std::fixed << std::setprecision(2)
                          << (delta_ops / sec) << ", fail_iops="
                          << (delta_fail / sec)
                          << ", submitted=" << total_stats.submitted_ops
                          << ", completed=" << total_stats.completed_ops
                          << ", failed=" << total_stats.failed_ops;
                for (size_t endpoint_index = 0; endpoint_index < endpoints.size();
                     ++endpoint_index) {
                    uint64_t endpoint_delta_bytes =
                        endpoint_stats[endpoint_index].bytes -
                        endpoint_last_report_stats[endpoint_index].bytes;
                    uint64_t endpoint_delta_ops =
                        endpoint_stats[endpoint_index].completed_ops -
                        endpoint_last_report_stats[endpoint_index].completed_ops;
                    uint64_t endpoint_delta_fail =
                        endpoint_stats[endpoint_index].failed_ops -
                        endpoint_last_report_stats[endpoint_index].failed_ops;
                    LOG(INFO) << "interval endpoint[" << endpoint_index
                              << "] throughput="
                              << FormatBytesPerSecond(endpoint_delta_bytes / sec)
                              << ", iops=" << std::fixed << std::setprecision(2)
                              << (endpoint_delta_ops / sec) << ", fail_iops="
                              << (endpoint_delta_fail / sec)
                              << ", endpoint=" << endpoints[endpoint_index].endpoint;
                }
                last_report_stats = total_stats;
                endpoint_last_report_stats = endpoint_stats;
                next_report =
                    now + std::chrono::milliseconds(FLAGS_report_interval_ms);
            }

            if (!before_end && !any_active) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        for (auto &slot : slots) {
            if (slot.buffer) {
                wrapper.Free(slot.buffer);
                slot.buffer = nullptr;
            }
        }

        BenchStats measured;
        measured.submitted_ops = total_stats.submitted_ops - warmup_base.submitted_ops;
        measured.completed_ops = total_stats.completed_ops - warmup_base.completed_ops;
        measured.failed_ops = total_stats.failed_ops - warmup_base.failed_ops;
        measured.bytes = total_stats.bytes - warmup_base.bytes;
        std::vector<BenchStats> endpoint_measured(endpoint_stats.size());
        for (size_t endpoint_index = 0; endpoint_index < endpoint_stats.size();
             ++endpoint_index) {
            endpoint_measured[endpoint_index].submitted_ops =
                endpoint_stats[endpoint_index].submitted_ops -
                endpoint_warmup_base[endpoint_index].submitted_ops;
            endpoint_measured[endpoint_index].completed_ops =
                endpoint_stats[endpoint_index].completed_ops -
                endpoint_warmup_base[endpoint_index].completed_ops;
            endpoint_measured[endpoint_index].failed_ops =
                endpoint_stats[endpoint_index].failed_ops -
                endpoint_warmup_base[endpoint_index].failed_ops;
            endpoint_measured[endpoint_index].bytes =
                endpoint_stats[endpoint_index].bytes -
                endpoint_warmup_base[endpoint_index].bytes;
        }

        double duration = static_cast<double>(FLAGS_duration_sec);
        double bw = measured.bytes / duration;
        double iops = measured.completed_ops / duration;
        double fail_iops = measured.failed_ops / duration;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "========== NoF Worker Pool Bench ==========\n";
        std::cout << "endpoints=" << endpoints.size() << "\n";
        std::cout << "unique_handles=" << unique_handles.size() << "\n";
        std::cout << "configured_nof_workers="
                  << (FLAGS_nof_workers == 0 ? mooncake::kDefaultSpdkNofWorkers
                                             : FLAGS_nof_workers)
                  << "\n";
        std::cout << "effective_worker_bindings<="
                  << std::min<uint64_t>(
                         unique_handles.size(),
                         FLAGS_nof_workers == 0 ? mooncake::kDefaultSpdkNofWorkers
                                                : FLAGS_nof_workers)
                  << "\n";
        std::cout << "io_size_bytes=" << FLAGS_io_size << "\n";
        std::cout << "iodepth=" << FLAGS_iodepth << "\n";
        std::cout << "duration_sec=" << FLAGS_duration_sec << "\n";
        std::cout << "completed_ops=" << measured.completed_ops << "\n";
        std::cout << "failed_ops=" << measured.failed_ops << "\n";
        std::cout << "bandwidth=" << FormatBytesPerSecond(bw) << "\n";
        std::cout << "iops=" << iops << "\n";
        std::cout << "fail_iops=" << fail_iops << "\n";
        for (size_t endpoint_index = 0; endpoint_index < endpoints.size();
             ++endpoint_index) {
            double endpoint_bw = endpoint_measured[endpoint_index].bytes / duration;
            double endpoint_iops =
                endpoint_measured[endpoint_index].completed_ops / duration;
            double endpoint_fail_iops =
                endpoint_measured[endpoint_index].failed_ops / duration;
            std::cout << "endpoint[" << endpoint_index << "]="
                      << endpoints[endpoint_index].endpoint << "\n";
            std::cout << "endpoint[" << endpoint_index
                      << "].completed_ops="
                      << endpoint_measured[endpoint_index].completed_ops << "\n";
            std::cout << "endpoint[" << endpoint_index
                      << "].failed_ops="
                      << endpoint_measured[endpoint_index].failed_ops << "\n";
            std::cout << "endpoint[" << endpoint_index
                      << "].bandwidth="
                      << FormatBytesPerSecond(endpoint_bw) << "\n";
            std::cout << "endpoint[" << endpoint_index
                      << "].iops=" << endpoint_iops << "\n";
            std::cout << "endpoint[" << endpoint_index
                      << "].fail_iops=" << endpoint_fail_iops << "\n";
        }
        std::cout << "==========================================\n";

        google::ShutdownGoogleLogging();
        return 0;
    } catch (const std::exception &e) {
        LOG(ERROR) << "Benchmark failed: " << e.what();
        google::ShutdownGoogleLogging();
        return 1;
    }
}
