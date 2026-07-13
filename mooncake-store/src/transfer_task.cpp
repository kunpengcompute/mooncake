#include "transfer_task.h"

#include <glog/logging.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <new>
#include <set>
#include "common.h"
#include "transfer_engine.h"
#include "transport/transport.h"
#include "spdk/spdk_wrapper.h"

static bool IsTruthyEnv(const char *value) {
    if (!value) {
        return false;
    }
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized == "1" || normalized == "true" || normalized == "yes" ||
           normalized == "on";
}

static bool IsSpdkNofDebugEnabled() {
    static const bool enabled = IsTruthyEnv(std::getenv("MC_NOF_DEBUG"));
    return enabled;
}

static int GetSpdkNofDebugIntervalMs() {
    static const int interval_ms = []() {
        const char *raw_value = std::getenv("MC_NOF_DEBUG_INTERVAL_MS");
        if (!raw_value) {
            return 1000;
        }
        char *end_ptr = nullptr;
        long parsed = std::strtol(raw_value, &end_ptr, 10);
        if (end_ptr == raw_value || (end_ptr != nullptr && *end_ptr != '\0') || parsed <= 0) {
            return 1000;
        }
        return static_cast<int>(parsed);
    }();
    return interval_ms;
}

static int GetPositiveEnvOrDefault(const char *name, int default_value) {
    const char *raw_value = std::getenv(name);
    if (!raw_value || raw_value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    char *end_ptr = nullptr;
    long parsed = std::strtol(raw_value, &end_ptr, 10);
    if (errno != 0 || end_ptr == raw_value ||
        (end_ptr != nullptr && *end_ptr != '\0') || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max()) {
        LOG(WARNING) << "Invalid value for " << name << ": " << raw_value
                     << ", using default " << default_value;
        return default_value;
    }

    return static_cast<int>(parsed);
}

static int GetSpdkNofSubmitChunkBytes() {
    static const int value = GetPositiveEnvOrDefault(
        "MC_NOF_SUBMIT_CHUNK_BYTES", mooncake::kDefaultSpdkNofSubmitChunkBytes);
    return value;
}

static int GetSpdkNofInflightBytesLimit() {
    static const int value = GetPositiveEnvOrDefault(
        "MC_NOF_INFLIGHT_BYTES_LIMIT", mooncake::kDefaultSpdkNofInflightBytesLimit);
    return value;
}

static int GetSpdkNofWorkerCount() {
    static const int value = GetPositiveEnvOrDefault(
        "MC_NOF_WORKERS", mooncake::kDefaultSpdkNofWorkers);
    return value;
}

static int CountSpdkNofQueuedTasks(const mooncake::SpdkNofTask *head) {
    int count = 0;
    const mooncake::SpdkNofTask *cursor = head;
    while (cursor != nullptr) {
        ++count;
        cursor = cursor->nxt;
    }
    return count;
}

static inline void SpdkNofTaskCompletion(mooncake::SpdkNofTask *task) {
    if (task->remaining_lba == 0 && task->outstanding_sub_io == 0) {
        if (task->nof_qos) {
            task->nof_qos->active_tasks.erase(task);
        }
        task->state->set_completed(task->failed ? 
            mooncake::ErrorCode::TRANSFER_FAIL : 
            mooncake::ErrorCode::OK);
        if (!task->on_chain && !task->abandoned) {
            delete task;
        }
    }
}

static void nvmf_io_complete(void *ctx, const struct spdk_nvme_cpl *cpl) {
    if (!ctx) {
        LOG(ERROR) << "nvmf_io_complete ctx is null";
        return;
    }

    mooncake::SpdkNofSubTask *sub_task = reinterpret_cast<mooncake::SpdkNofSubTask *>(ctx);
    mooncake::SpdkNofTask *task = sub_task->task;
    mooncake::SpdkNofQos *nof_qos = task->nof_qos;
    int op = task->op;
    if (task->abandoned) {
        if (spdk_nvme_cpl_is_error(cpl)) {
            LOG(ERROR) << "spdk_nof_io_completion_after_segment_failed op="
                       << op << " seg_handle=" << task->seg_handle
                       << " lba=" << task->lba
                       << " submit_lba_count=" << sub_task->submit_lba_count
                       << " status="
                       << spdk_nvme_cpl_get_status_string(&cpl->status);
        }
        task->abandoned_sub_io--;
        sub_task->sub_task_pool->push(sub_task);
        if (task->abandoned_sub_io <= 0) {
            delete task;
        }
        return;
    }

    if (--(*task->io_count) < 0) {
        LOG(ERROR) << "total oustanding io < 0";
    }

    if (--(task->outstanding_sub_io) < 0) {
        LOG(ERROR) << "task oustanding io < 0";
    }

    nof_qos->inflight_blocks[op] -= sub_task->submit_lba_count;
    if (nof_qos->inflight_blocks[op] < 0) {
        LOG(ERROR) << "task oustanding io < 0";
    }

    if (spdk_nvme_cpl_is_error(cpl)) {
        LOG(ERROR) << "spdk_nof_io_completion_error op=" << op
                   << " seg_handle=" << task->seg_handle
                   << " lba=" << task->lba
                   << " submit_lba_count=" << sub_task->submit_lba_count
                   << " status="
                   << spdk_nvme_cpl_get_status_string(&cpl->status);
        task->remaining_lba = 0;
        task->failed = true;
    }

    SpdkNofTaskCompletion(task);

    sub_task->sub_task_pool->push(sub_task);
}
namespace mooncake {

SpdkNofQos::SpdkNofQos(uint32_t block_size) {
    int block_size_int = static_cast<int>(block_size);
    if (block_size_int <= 0) {
        block_size_int = 1;
    }

    blocks_per_chunk = std::max(1, GetSpdkNofSubmitChunkBytes() / block_size_int);
    inflight_blocks_limit = std::max(1, GetSpdkNofInflightBytesLimit() / block_size_int);
    for (int i = 0; i < kSpdkNofOpNum; ++i) {
        inflight_blocks[i] = 0;
        head[i] = nullptr;
        tail[i] = nullptr;
    }
}

// ============================================================================
// FilereadWorkerPool Implementation
// ============================================================================
// to fully utilize the available ssd bandwidth, we use a default of 10 worker
// threads.
constexpr int kDefaultFilereadWorkers = 10;

FilereadWorkerPool::FilereadWorkerPool(std::shared_ptr<StorageBackend>& backend)
    : shutdown_(false) {
    VLOG(1) << "Creating FilereadWorkerPool with " << kDefaultFilereadWorkers
            << " workers";

    // Start worker threads
    workers_.reserve(kDefaultFilereadWorkers);
    for (int i = 0; i < kDefaultFilereadWorkers; ++i) {
        workers_.emplace_back(&FilereadWorkerPool::workerThread, this);
    }
    backend_ = backend;
}

FilereadWorkerPool::~FilereadWorkerPool() {
    // Signal shutdown
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_.store(true);
    }
    queue_cv_.notify_all();

    // Wait for all workers to finish
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    VLOG(1) << "FilereadWorkerPool destroyed";
}

void FilereadWorkerPool::submitTask(FilereadTask task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutdown_.load()) {
            LOG(WARNING)
                << "Attempting to submit task to shutdown FilereadWorkerPool";
            task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            return;
        }
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

void FilereadWorkerPool::workerThread() {
    VLOG(2) << "FilereadWorkerPool worker thread started";

    while (true) {
        FilereadTask task("", 0, {}, nullptr);

        // Wait for task or shutdown signal
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return shutdown_.load() || !task_queue_.empty();
            });

            if (shutdown_.load() && task_queue_.empty()) {
                break;
            }

            if (!task_queue_.empty()) {
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
        }

        // Execute the task if we have one
        if (task.state) {
            try {
                if (!backend_) {
                    LOG(ERROR)
                        << "Backend is not initialized, cannot load object";
                    task.state->set_completed(ErrorCode::TRANSFER_FAIL);
                    continue;
                }

                auto load_result = backend_->LoadObject(
                    task.file_path, task.slices, task.object_size);
                if (load_result) {
                    VLOG(2) << "Fileread task completed successfully with "
                            << task.file_path;
                    task.state->set_completed(ErrorCode::OK);
                } else {
                    LOG(ERROR)
                        << "Fileread task failed for file: " << task.file_path
                        << " with error: " << toString(load_result.error());
                    task.state->set_completed(ErrorCode::TRANSFER_FAIL);
                }
            } catch (const std::exception& e) {
                LOG(ERROR) << "Exception during async fileread: " << e.what();
                task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            }
        }
    }

    VLOG(2) << "FilereadWorkerPool worker thread exiting";
}

// ============================================================================
// SpdkNofWorkerPool Implementation
// ============================================================================
// to fully utilize the available ssd bandwidth, we use a default of 4 worker
// threads.

SpdkNofWorkerPool::SpdkNofWorkerPool(int numa_socket_id)
    : worker_count_(GetSpdkNofWorkerCount()),
      numa_socket_id_(numa_socket_id),
      task_queue_(std::make_unique<std::queue<SpdkNofTask>[]>(worker_count_)),
      queue_mutex_(std::make_unique<std::mutex[]>(worker_count_)),
      queue_cv_(std::make_unique<std::condition_variable[]>(worker_count_)),
      shutdown_(false) {
    VLOG(1) << "Creating SpdkNofWorkerPool with " << worker_count_
            << " workers";

    // Start worker threads
    workers_.reserve(worker_count_);
    for (int i = 0; i < worker_count_; ++i) {
        workers_.emplace_back(&SpdkNofWorkerPool::workerThread, this, i);
    }
}

SpdkNofWorkerPool::~SpdkNofWorkerPool() {
    if (shutdown_.exchange(true)) {
        return;
    }

    for (int i = 0; i < worker_count_; ++i) {
        queue_cv_[i].notify_all();
    }

    for (auto &worker: workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    VLOG(1) << "SpdkNofWorkerPool destroyed";
}

void SpdkNofWorkerPool::submitTask(SpdkNofTask task) {
    if (!task.state) {
        LOG(ERROR) << "Attempting to submit spdk nof task without state";
        return;
    }

    if (shutdown_.load()) {
        LOG(WARNING)
            << "Attempting to submit task to shutdown SpdkNofWorkerPool";
        task.state->set_completed(ErrorCode::TRANSFER_FAIL);
        return;
    }

    int worker_idx = -1;
    {
        std::lock_guard<std::mutex> lock(seg_mutex_);
        nof_seg_handle *seg = task.seg_handle;
        bool new_binding = false;
        if (seg_to_worker_.find(seg) != seg_to_worker_.end()) {
            worker_idx = seg_to_worker_[seg];
        } else {
            worker_idx = (seg_num++ % worker_count_);
            seg_to_worker_[seg] = worker_idx;
            new_binding = true;
        }
        if (new_binding && IsSpdkNofDebugEnabled()) {
            LOG(INFO) << "nof_worker_bind seg_handle=" << seg
                      << " worker_idx=" << worker_idx;
        }
    }
    if (worker_idx < 0 || worker_idx >= worker_count_) {
        LOG(ERROR) << "seg is not bind to invalid worker " << worker_idx;
        task.state->set_completed(ErrorCode::TRANSFER_FAIL);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_[worker_idx]);
        task_queue_[worker_idx].push(std::move(task));
    }
    queue_cv_[worker_idx].notify_one();
}

static bool HasBufferedTask(
    const std::map<nof_seg_handle *, SpdkNofQos *> &seg_to_qos) {
    for (const auto &[_, nof_qos]: seg_to_qos) {
        if (!nof_qos->Empty()) {
            return true;
        }
    }
    return false;
}

static void FailSpdkNofSegmentTasks(nof_seg_handle *seg_handle,
                                    SpdkNofQos *nof_qos,
                                    int64_t *total_outstanding_io,
                                    int work_idx,
                                    const std::string& reason) {
    if (!nof_qos) {
        return;
    }

    int64_t failed_outstanding = 0;
    std::vector<SpdkNofTask *> tasks(nof_qos->active_tasks.begin(),
                                     nof_qos->active_tasks.end());
    for (auto *task : tasks) {
        if (!task) {
            continue;
        }
        failed_outstanding += std::max(0, task->outstanding_sub_io);
        task->failed = true;
        task->abandoned = task->outstanding_sub_io > 0;
        task->abandoned_sub_io = std::max(0, task->outstanding_sub_io);
        task->remaining_lba = 0;
        task->outstanding_sub_io = 0;
        task->on_chain = false;
        task->state->set_completed(ErrorCode::TRANSFER_FAIL);
    }

    for (int i = 0; i < kSpdkNofOpNum; ++i) {
        nof_qos->head[i] = nullptr;
        nof_qos->tail[i] = nullptr;
        nof_qos->inflight_blocks[i] = 0;
    }
    nof_qos->active_tasks.clear();

    if (total_outstanding_io) {
        *total_outstanding_io =
            std::max<int64_t>(0, *total_outstanding_io - failed_outstanding);
    }

    LOG(ERROR) << "spdk_nof_segment_failed worker_idx=" << work_idx
               << " seg_handle=" << seg_handle
               << " failed_tasks=" << tasks.size()
               << " failed_outstanding_io=" << failed_outstanding
               << " reason=" << reason;
}

constexpr int kSpdkNofSubTaskChunkSize = 4096;

static inline bool CheckSubTaskPool(
    std::stack<SpdkNofSubTask *> &sub_task_pool,
    std::vector<SpdkNofSubTask *> &sub_task_chunks, int work_idx) {
    if (!sub_task_pool.empty()) {
        return true;
    }
    SpdkNofSubTask *sub_tasks =
        new (std::nothrow) SpdkNofSubTask[kSpdkNofSubTaskChunkSize];
    if (!sub_tasks) {
         LOG(ERROR) << "alloc SpdkNofSubTask failed, worker " << work_idx;
         return false;
    }
    sub_task_chunks.push_back(sub_tasks);

    for (int i = 0; i < kSpdkNofSubTaskChunkSize; ++i) {
        sub_tasks[i].sub_task_pool = &sub_task_pool;
        sub_task_pool.push(&sub_tasks[i]);
    }

    return true;
}

void SpdkNofWorkerPool::workerThread(int work_idx) {
    bindToSocket(numa_socket_id_);
    VLOG(2) << "SpdkNofWorkerPool worker thread started";

    int64_t total_outstanding_io = 0;
    // std::set<nof_seg_handle *> seg_set;
    std::map<nof_seg_handle *, SpdkNofQos *> seg_to_qos;
    std::stack<SpdkNofSubTask *> sub_task_pool;
    std::vector<SpdkNofSubTask *> sub_task_chunks;
    std::set<nof_seg_handle *> failed_seg_handles;
    auto &task_queue = task_queue_[work_idx];
    auto &queue_cv = queue_cv_[work_idx];
    auto &queue_mutex = queue_mutex_[work_idx];
    auto last_debug_snapshot = std::chrono::steady_clock::now();

    if (!CheckSubTaskPool(sub_task_pool, sub_task_chunks, work_idx)) {
        return;
    }

    while (true) {
        // Wait for task or shutdown signal
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this, &task_queue, &total_outstanding_io,
                                 &seg_to_qos] {
                return shutdown_.load() || !task_queue.empty() ||
                       total_outstanding_io || HasBufferedTask(seg_to_qos);
            });

            if (shutdown_.load() && task_queue.empty() &&
                (total_outstanding_io == 0) && !HasBufferedTask(seg_to_qos)) {
                break;
            }

            while (!task_queue.empty()) {
                SpdkNofTask *task = new (std::nothrow)
                    SpdkNofTask(std::move(task_queue.front()));
                if (task == nullptr) {
                    LOG(ERROR) << "alloc SpdkNofTask failed, worker " << work_idx;
                    continue;
                }
                if (failed_seg_handles.count(task->seg_handle)) {
                    LOG(ERROR) << "reject spdk nof task on failed segment"
                               << ", worker_idx=" << work_idx
                               << ", seg_handle=" << task->seg_handle;
                    task->state->set_completed(ErrorCode::TRANSFER_FAIL);
                    delete task;
                    task_queue.pop();
                    continue;
                }

                SpdkNofQos *nof_qos = nullptr;
                auto it = seg_to_qos.find(task->seg_handle);
                if (it == seg_to_qos.end()) {
                    SpdkNofQos *qos =
                        new (std::nothrow) SpdkNofQos(
                            SpdkWrapper::GetInstance().GetBlockSize(task->seg_handle));
                    if (qos == nullptr) {
                        LOG(ERROR) << "alloc SpdkNofQos failed, worker " << work_idx;
                        delete task;
                        continue;
                    }
                    seg_to_qos[task->seg_handle] = qos;
                    nof_qos = qos;
                    if (IsSpdkNofDebugEnabled()) {
                        LOG(INFO) << "nof_qos_create worker_idx=" << work_idx
                                  << " seg_handle=" << task->seg_handle
                                  << " blocks_per_chunk=" << qos->blocks_per_chunk
                                  << " inflight_blocks_limit="
                                  << qos->inflight_blocks_limit;
                    }
                } else {
                    nof_qos = it->second;
                }
                task->io_count = &total_outstanding_io;
                task->nof_qos = nof_qos;
                task->on_chain = true;
                nof_qos->PushTask(task);
                task_queue.pop();
            }
        }

        for (auto &[seg_handle, nof_qos]: seg_to_qos) {
            if (failed_seg_handles.count(seg_handle)) {
                continue;
            }
            uint32_t block_size = SpdkWrapper::GetInstance().GetBlockSize(seg_handle);
            for (int i = 0; i < kSpdkNofOpNum; ++i) {
                int avail_blocks = nof_qos->inflight_blocks_limit - nof_qos->inflight_blocks[i];
                while (nof_qos->head[i] && avail_blocks > 0) {
                    SpdkNofTask *task = nof_qos->head[i];
                    SpdkNofSubTask *sub_task;
                    while (task->remaining_lba > 0 && avail_blocks > 0) {
                        uint32_t submit_lba_count = std::min(avail_blocks, 
                            std::min(task->remaining_lba, nof_qos->blocks_per_chunk));
                        int lba_off = task->lba_count - task->remaining_lba;
                        uint64_t submit_lba= task->lba + lba_off;
                        void *submit_ptr = reinterpret_cast<void *>
                            (reinterpret_cast<char *>(task->ptr) + lba_off * block_size);

                        if (!CheckSubTaskPool(sub_task_pool, sub_task_chunks,
                                              work_idx)) {
                            task->failed = true;
                            task->remaining_lba = 0;
                            break;
                        }
                        sub_task = sub_task_pool.top();
                        sub_task_pool.pop();
                        sub_task->task = task;
                        sub_task->submit_lba_count = submit_lba_count;
                        
                        int ret = SpdkWrapper::GetInstance().SubmitRequest(task->seg_handle,
                            submit_ptr, submit_lba, submit_lba_count, task->op, nvmf_io_complete, sub_task);
                        if (ret != 0) {
                            LOG(ERROR) << "work " << work_idx << ", seg " << task->seg_handle << " submit io fail";
                            task->failed = true;
                            task->remaining_lba = 0;
                            if (SpdkWrapper::GetInstance().IsConnectionError(ret)) {
                                FailSpdkNofSegmentTasks(
                                    seg_handle, nof_qos, &total_outstanding_io,
                                    work_idx,
                                    "submit_connection_error:" +
                                        std::to_string(ret));
                                failed_seg_handles.insert(seg_handle);
                                SpdkWrapper::GetInstance()
                                    .InvalidateNofController(seg_handle);
                                {
                                    std::lock_guard<std::mutex> lock(seg_mutex_);
                                    seg_to_worker_.erase(seg_handle);
                                }
                                break;
                            }
                        } else {
                            task->idx++;
                            task->remaining_lba -= submit_lba_count;
                            nof_qos->inflight_blocks[i] += submit_lba_count;
                            avail_blocks -= submit_lba_count;
                            task->outstanding_sub_io++;
                            total_outstanding_io++;
                        }
                    }
                    if (task->remaining_lba == 0) {
                        nof_qos->PopTask(i);
                        task->on_chain = false;
                        SpdkNofTaskCompletion(task);
                    }
                }
            }
        }

        for (auto& it : seg_to_qos) {
            if (failed_seg_handles.count(it.first)) {
                continue;
            }
            int32_t ret =
                SpdkWrapper::GetInstance().NvmePollAdminCompletions(it.first);
            if (ret < 0) {
                LOG(ERROR) << "poll admin completion error: ret " << ret;
                if (SpdkWrapper::GetInstance().IsConnectionError(ret)) {
                    FailSpdkNofSegmentTasks(
                        it.first, it.second, &total_outstanding_io, work_idx,
                        "admin_connection_error:" + std::to_string(ret));
                    failed_seg_handles.insert(it.first);
                    SpdkWrapper::GetInstance().InvalidateNofController(it.first);
                    {
                        std::lock_guard<std::mutex> lock(seg_mutex_);
                        seg_to_worker_.erase(it.first);
                    }
                }
            }
        }

        if (total_outstanding_io > 0) {
            int64_t ret = 0;
            for (auto &it: seg_to_qos) {
                if (failed_seg_handles.count(it.first)) {
                    continue;
                }
                ret = SpdkWrapper::GetInstance().NvmePollProcessCompletion(it.first, 0);
                if (ret < 0) {
                    LOG(ERROR) << "poll completion error: ret " << ret;
                    if (SpdkWrapper::GetInstance().IsConnectionError(
                            static_cast<int32_t>(ret))) {
                        FailSpdkNofSegmentTasks(
                            it.first, it.second, &total_outstanding_io,
                            work_idx,
                            "io_poll_connection_error:" +
                                std::to_string(ret));
                        failed_seg_handles.insert(it.first);
                        SpdkWrapper::GetInstance().InvalidateNofController(
                            it.first);
                        {
                            std::lock_guard<std::mutex> lock(seg_mutex_);
                            seg_to_worker_.erase(it.first);
                        }
                    }
                }
            }
        }

        if (IsSpdkNofDebugEnabled()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_debug_snapshot);
            if (elapsed.count() >= GetSpdkNofDebugIntervalMs()) {
                for (const auto &[seg_handle, nof_qos] : seg_to_qos) {
                    if (failed_seg_handles.count(seg_handle)) {
                        continue;
                    }
                    LOG(INFO) << "nof_qos_state worker_idx=" << work_idx
                              << " seg_handle=" << seg_handle
                              << " inflight_read=" << nof_qos->inflight_blocks[0]
                              << " inflight_write=" << nof_qos->inflight_blocks[1]
                              << " inflight_limit="
                              << nof_qos->inflight_blocks_limit
                              << " queued_read="
                              << CountSpdkNofQueuedTasks(nof_qos->head[0])
                              << " queued_write="
                              << CountSpdkNofQueuedTasks(nof_qos->head[1])
                              << " total_outstanding_io=" << total_outstanding_io;
                }
                last_debug_snapshot = now;
            }
        }
    }

    for (auto *sub_tasks : sub_task_chunks) {
        delete[] sub_tasks;
    }
    for (auto &[_, nof_qos] : seg_to_qos) {
        delete nof_qos;
    }
    seg_to_qos.clear();

    VLOG(2) << "SpdkNofWorkerPool worker thread exiting";
}

// ============================================================================
// MemcpyWorkerPool Implementation
// ============================================================================
// Since memcpy is bound by memory bandwidth, we only need one worker thread.
constexpr int kDefaultMemcpyWorkers = 1;

MemcpyWorkerPool::MemcpyWorkerPool() : shutdown_(false) {
    VLOG(1) << "Creating MemcpyWorkerPool with " << kDefaultMemcpyWorkers
            << " workers";

    // Start worker threads
    workers_.reserve(kDefaultMemcpyWorkers);
    for (int i = 0; i < kDefaultMemcpyWorkers; ++i) {
        workers_.emplace_back(&MemcpyWorkerPool::workerThread, this);
    }
}

MemcpyWorkerPool::~MemcpyWorkerPool() {
    // Signal shutdown
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_.store(true);
    }
    queue_cv_.notify_all();

    // Wait for all workers to finish
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    VLOG(1) << "MemcpyWorkerPool destroyed";
}

void MemcpyWorkerPool::submitTask(MemcpyTask task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutdown_.load()) {
            LOG(WARNING)
                << "Attempting to submit task to shutdown MemcpyWorkerPool";
            task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            return;
        }
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

void MemcpyWorkerPool::workerThread() {
    VLOG(2) << "MemcpyWorkerPool worker thread started";

    while (true) {
        MemcpyTask task({}, nullptr);

        // Wait for task or shutdown signal
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return shutdown_.load() || !task_queue_.empty();
            });

            if (shutdown_.load() && task_queue_.empty()) {
                break;
            }

            if (!task_queue_.empty()) {
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
        }

        // Execute the task if we have one
        if (task.state) {
            try {
                for (const auto& op : task.operations) {
                    std::memcpy(op.dest, op.src, op.size);
                }

                VLOG(2) << "Memcpy task completed successfully with "
                        << task.operations.size() << " operations";
                task.state->set_completed(ErrorCode::OK);
            } catch (const std::exception& e) {
                LOG(ERROR) << "Exception during async memcpy: " << e.what();
                task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            }
        }
    }

    VLOG(2) << "MemcpyWorkerPool worker thread exiting";
}

// ============================================================================
// TransferEngineOperationState Implementation
// ============================================================================

bool TransferEngineOperationState::is_completed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result_.has_value()) {
        return true;
    }

    check_task_status();
    return result_.has_value();
}

void TransferEngineOperationState::check_task_status() {
    // Check all transfers in the batch
    bool all_completed = true;
    bool has_failure = false;

    for (size_t i = 0; i < batch_size_; ++i) {
        TransferStatus status;
        Status s = engine_.getTransferStatus(batch_id_, i, status);
        if (!s.ok()) {
            LOG(ERROR) << "Failed to get transfer status for batch "
                       << batch_id_ << " task " << i << " with error "
                       << s.message();
            set_result_internal(ErrorCode::TRANSFER_FAIL);
            return;
        }

        switch (status.s) {
            case TransferStatusEnum::COMPLETED:
                // This transfer is done, continue checking others
                break;
            case TransferStatusEnum::FAILED:
            case TransferStatusEnum::CANCELED:
            case TransferStatusEnum::INVALID:
#ifndef USE_ASCEND_DIRECT
                LOG(ERROR) << "Transfer failed for batch " << batch_id_
                           << " task " << i << " with status "
                           << static_cast<int>(status.s);
#endif
                has_failure = true;
                break;
            default:
                // Transfer is still pending (PENDING, RUNNING, etc.)
                all_completed = false;
                break;
        }
    }

    if (has_failure) {
        VLOG(1) << "Setting batch " << batch_id_
                << " result to TRANSFER_FAIL due to task failures";
        set_result_internal(ErrorCode::TRANSFER_FAIL);
        return;
    }

    if (all_completed) {
        set_result_internal(ErrorCode::OK);
        return;
    }

    return;
}

void TransferEngineOperationState::set_result_internal(ErrorCode error_code) {
    if (result_.has_value()) {
        LOG(ERROR) << "Attempting to set result multiple times for batch "
                   << batch_id_
                   << ". Previous result: " << static_cast<int>(result_.value())
                   << ", attempted new result: " << static_cast<int>(error_code)
                   << ". This indicates a race condition or logic error.";
        return;  // Don't crash, just return early
    }

    VLOG(1) << "Setting transfer result for batch " << batch_id_ << " to "
            << static_cast<int>(error_code);
    result_.emplace(error_code);
}

void TransferEngineOperationState::wait_for_completion() {
    if (is_completed()) {
        return;
    }

    constexpr int64_t timeout_seconds = 60;

#ifdef USE_EVENT_DRIVEN_COMPLETION
    VLOG(1) << "Waiting for transfer engine completion for batch " << batch_id_;

    // Wait directly on BatchDesc's condition variable.
    auto& batch_desc = Transport::toBatchDesc(batch_id_);
    bool completed;
    bool failed = false;

    // Fast path: if already finished, avoid taking the mutex and waiting.
    // Use acquire here to pair with the writer's release-store, because this
    // path may skip taking the mutex. It ensures all prior updates are visible.
    completed = batch_desc.is_finished.load(std::memory_order_acquire);
    if (!completed) {
        // Use the same mutex as the notifier when updating the predicate to
        // avoid missed notifications. The predicate is re-checked under the
        // lock. Under the mutex, relaxed is sufficient; the mutex acquire
        // orders prior writes.
        std::unique_lock<std::mutex> lock(batch_desc.completion_mutex);
        completed = batch_desc.completion_cv.wait_for(
            lock, std::chrono::seconds(timeout_seconds), [&batch_desc] {
                return batch_desc.is_finished.load(std::memory_order_relaxed);
            });
    }  // Explicitly release completion_mutex before acquiring mutex_

    // Once completion is observed, read failure flag.
    if (completed) {
        failed = batch_desc.has_failure.load(std::memory_order_relaxed);
    }

    ErrorCode error_code =
        completed ? (failed ? ErrorCode::TRANSFER_FAIL : ErrorCode::OK)
                  : ErrorCode::TRANSFER_FAIL;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        set_result_internal(error_code);
    }

    if (completed) {
        VLOG(1) << "Transfer engine operation completed for batch " << batch_id_
                << " with result: " << static_cast<int>(error_code);
    } else {
        LOG(ERROR) << "Failed to complete transfers after " << timeout_seconds
                   << " seconds for batch " << batch_id_;
    }
#else
    VLOG(1) << "Starting transfer engine polling for batch " << batch_id_;

    constexpr int64_t kOneSecondInNano = 1000 * 1000 * 1000;
    const int64_t start_ts = getCurrentTimeInNano();

    while (true) {
        if (getCurrentTimeInNano() - start_ts >
            timeout_seconds * kOneSecondInNano) {
            LOG(ERROR) << "Failed to complete transfers after "
                       << timeout_seconds << " seconds for batch " << batch_id_;
            set_result_internal(ErrorCode::TRANSFER_FAIL);
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        check_task_status();
        if (result_.has_value()) {
            VLOG(1) << "Transfer engine operation completed for batch "
                    << batch_id_
                    << " with result: " << static_cast<int>(result_.value());
            break;
        }
        // Continue polling
        VLOG(1) << "Transfer engine operation still pending for batch "
                << batch_id_;
    }
#endif
}

// ============================================================================
// TransferFuture Implementation
// ============================================================================

TransferFuture::TransferFuture(std::shared_ptr<OperationState> state)
    : state_(std::move(state)) {
    if (!state_) {
        LOG(ERROR) << "TransferFuture requires valid state";
        throw std::invalid_argument("TransferFuture requires valid state");
    }
}

bool TransferFuture::isReady() const { return state_->is_completed(); }

ErrorCode TransferFuture::wait() {
    if (!isReady()) {
        state_->wait_for_completion();
    }
    return state_->get_result();
}

ErrorCode TransferFuture::get() { return wait(); }

TransferStrategy TransferFuture::strategy() const {
    return state_->get_strategy();
}

// ============================================================================
// TransferSubmitter Implementation
// ============================================================================

TransferSubmitter::TransferSubmitter(TransferEngine& engine,
                                     std::shared_ptr<StorageBackend>& backend,
                                     TransferMetric* transfer_metric,
                                     int numa_socket_id)
    : engine_(engine),
      memcpy_pool_(std::make_unique<MemcpyWorkerPool>()),
#ifdef USE_NOF
      spdk_nvmf_pool_(std::make_unique<SpdkNofWorkerPool>(numa_socket_id)),
#else
      spdk_nvmf_pool_(nullptr),
#endif
      fileread_pool_(std::make_unique<FilereadWorkerPool>(backend)),
      transfer_metric_(transfer_metric) {
    // Read MC_STORE_MEMCPY environment variable, default to false (disabled)
    const char* env_value = std::getenv("MC_STORE_MEMCPY");
    if (env_value == nullptr) {
        memcpy_enabled_ = false;  // Default: disabled
    } else {
        std::string env_str(env_value);
        // Convert to lowercase for case-insensitive comparison
        std::transform(env_str.begin(), env_str.end(), env_str.begin(),
                       ::tolower);
        if (env_str == "false" || env_str == "0" || env_str == "no" ||
            env_str == "off") {
            memcpy_enabled_ = false;
        } else if (env_str == "true" || env_str == "1" || env_str == "yes" ||
                   env_str == "on") {
            memcpy_enabled_ = true;
        } else {
            LOG(WARNING) << "Invalid value for MC_STORE_MEMCPY: " << env_str
                         << ", defaulting to enabled";
            memcpy_enabled_ = true;
        }
    }

    VLOG(1) << "TransferSubmitter initialized with memcpy_enabled="
            << memcpy_enabled_;
}

std::optional<TransferFuture> TransferSubmitter::submit(
    const Replica::Descriptor& replica, std::vector<Slice>& slices,
    TransferRequest::OpCode op_code, void *ptr, size_t size) {
    std::optional<TransferFuture> future;

    if (replica.is_memory_replica()) {
        auto& mem_desc = replica.get_memory_descriptor();
        auto& handle = mem_desc.buffer_descriptor;

        if (!validateTransferParams(handle, slices)) {
            return std::nullopt;
        }

        TransferStrategy strategy = selectStrategy(handle, slices);

        switch (strategy) {
            case TransferStrategy::LOCAL_MEMCPY:
                future = submitMemcpyOperation(handle, slices, op_code);
                break;
            case TransferStrategy::TRANSFER_ENGINE:
                future = submitTransferEngineOperation(handle, slices, op_code);
                break;
            default:
                LOG(ERROR) << "Unknown transfer strategy: " << strategy;
                return std::nullopt;
        }
    } else if (replica.is_nof_replica()) {
        auto& ssd_desc = replica.get_nof_descriptor();
        auto& handle = ssd_desc.buffer_descriptor;

        if (!ptr || (size == 0)) {
            return std::nullopt;
        }

        future = submitSpdkNofOperation(handle, ptr, size, op_code);
    } else {
        future = submitFileReadOperation(replica, slices, op_code);
    }

    // Update metrics on successful submission
    if (future.has_value()) {
        updateTransferMetrics(slices, op_code);
    }

    return future;
}

std::optional<TransferFuture> TransferSubmitter::submit_batch(
    const std::vector<Replica::Descriptor>& replicas,
    std::vector<std::vector<Slice>>& all_slices,
    TransferRequest::OpCode op_code) {
    std::optional<TransferFuture> future;
    std::vector<TransferRequest> requests;
    for (size_t i = 0; i < replicas.size(); ++i) {
        auto& replica = replicas[i];
        auto& slices = all_slices[i];
        auto& mem_desc = replica.get_memory_descriptor();
        if (!validateTransferParams(mem_desc.buffer_descriptor, slices)) {
            return std::nullopt;
        }
        auto& handle = mem_desc.buffer_descriptor;
        uint64_t offset = 0;
        SegmentHandle seg = engine_.openSegment(handle.transport_endpoint_);
        if (seg == static_cast<uint64_t>(ERR_INVALID_ARGUMENT)) {
            LOG(ERROR) << "Failed to open segment "
                       << handle.transport_endpoint_;
            return std::nullopt;
        }
        for (auto slice : slices) {
            TransferRequest request;
            request.opcode = op_code;
            request.source = static_cast<char*>(slice.ptr);
            request.target_id = seg;
            request.target_offset = handle.buffer_address_ + offset;
            request.length = slice.size;
            requests.emplace_back(request);
            offset += slice.size;
        }
    }
    future = submitTransfer(requests);
    // Update metrics on successful submission
    if (future.has_value()) {
        for (auto& slices : all_slices) {
            updateTransferMetrics(slices, op_code);
        }
    }
    return future;
}

std::optional<TransferFuture> TransferSubmitter::submitMemcpyOperation(
    const AllocatedBuffer::Descriptor& handle, const std::vector<Slice>& slices,
    const TransferRequest::OpCode op_code) {
    auto state = std::make_shared<MemcpyOperationState>();

    // Create memcpy operations
    std::vector<MemcpyOperation> operations;
    operations.reserve(slices.size());
    uint64_t base_address = static_cast<uint64_t>(handle.buffer_address_);
    uint64_t offset = 0;

    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& slice = slices[i];

        if (slice.ptr == nullptr) continue;

        void* dest;
        const void* src;

        if (op_code == TransferRequest::READ) {
            // READ: from handle (remote buffer) to slice (local
            // buffer)
            dest = slice.ptr;
            src = reinterpret_cast<const void*>(base_address + offset);
        } else {
            // WRITE: from slice (local buffer) to handle (remote
            // buffer)
            dest = reinterpret_cast<void*>(base_address + offset);
            src = slice.ptr;
        }
        offset += slice.size;

        operations.emplace_back(dest, src, slice.size);
    }

    // Submit memcpy operations to worker pool for async execution
    MemcpyTask task(std::move(operations), state);
    memcpy_pool_->submitTask(std::move(task));

    VLOG(1) << "Memcpy transfer submitted to worker pool with " << slices.size()
            << " operations";

    return TransferFuture(state);
}

std::optional<TransferFuture> TransferSubmitter::submitTransfer(
    std::vector<TransferRequest>& requests) {
    // Allocate batch ID
    const size_t batch_size = requests.size();
    BatchID batch_id = engine_.allocateBatchID(batch_size);
    if (batch_id == INVALID_BATCH_ID) {
        LOG(ERROR) << "Failed to allocate batch ID";
        return std::nullopt;
    }

    // Submit transfer
    Status s = engine_.submitTransfer(batch_id, requests);
    if (!s.ok()) {
        LOG(ERROR) << "Failed to submit all transfers, error code is "
                   << s.code();
        // Note: batch_id will be freed by TransferEngineOperationState
        // destructor if we create the state object, otherwise we need to free
        // it here
        engine_.freeBatchID(batch_id);
        return std::nullopt;
    }

    if (batch_id == INVALID_BATCH_ID) {  // INVALID_BATCH_ID
        LOG(ERROR) << "Invalid batch ID for transfer engine operation";
        return std::nullopt;
    }

    // Create state with transfer engine context - no polling thread
    // needed
    auto state = std::make_shared<TransferEngineOperationState>(
        engine_, batch_id, batch_size);

    return TransferFuture(state);
}

std::optional<TransferFuture> TransferSubmitter::submitTransferEngineOperation(
    const AllocatedBuffer::Descriptor& handle, const std::vector<Slice>& slices,
    const TransferRequest::OpCode op_code) {
    if (handle.transport_endpoint_.empty()) {
        LOG(ERROR) << "Transport endpoint is empty for handle with address "
                   << handle.buffer_address_;
        return std::nullopt;
    }
    SegmentHandle seg = engine_.openSegment(handle.transport_endpoint_);

    if (seg == static_cast<uint64_t>(ERR_INVALID_ARGUMENT)) {
        LOG(ERROR) << "Failed to open segment for endpoint='"
                   << handle.transport_endpoint_ << "'";
        return std::nullopt;
    }

    // Create transfer requests
    std::vector<TransferRequest> requests;
    requests.reserve(slices.size());
    uint64_t base_address = static_cast<uint64_t>(handle.buffer_address_);
    uint64_t offset = 0;

    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& slice = slices[i];
        if (slice.ptr == nullptr) continue;

        TransferRequest request;
        request.opcode = op_code;
        request.source = static_cast<char*>(slice.ptr);
        request.target_id = seg;
        request.target_offset = base_address + offset;
        request.length = slice.size;

        offset += slice.size;
        requests.emplace_back(request);
    }
    return submitTransfer(requests);
}

std::optional<TransferFuture> TransferSubmitter::submitSpdkNofOperation(
    const AllocatedBuffer::Descriptor& handle, void *ptr, size_t size,
    const TransferRequest::OpCode op_code) {
#ifndef USE_NOF
    LOG(ERROR) << "NoF transfer requested while USE_NOF is disabled"
               << ", endpoint=" << handle.transport_endpoint_;
    return std::nullopt;
#else
    if (handle.transport_endpoint_.empty() || handle.size_ < size) {
        LOG(ERROR) << "Transport endpoint " << handle.transport_endpoint_
                   << " buffer size " << handle.size_ << ", request size" << size;
        return std::nullopt;
    }

    nof_seg_handle *seg_handle = SpdkWrapper::GetInstance().OpenNofSegment(handle.transport_endpoint_);
    if (!seg_handle) {
        LOG(ERROR) << "Failed to open segment for endpoint='"
                   << handle.transport_endpoint_ << "'";
        return std::nullopt;
    }

    uint32_t block_size = SpdkWrapper::GetInstance().GetBlockSize(seg_handle);
    if ((block_size == INVALID_BLOCK_SIZE) || (handle.buffer_address_ % block_size) || (size % block_size)) {
        LOG(ERROR) << "request off " << handle.buffer_address_
                   << ", size " << size << " is not " << block_size << " align";
        return std::nullopt;
    }

    auto state = std::make_shared<SpdkNofOperationState>();
    SpdkNofTask task(seg_handle, ptr, handle.buffer_address_ / block_size, size / block_size, op_code, state);
    spdk_nvmf_pool_->submitTask(std::move(task));

    VLOG(1) << "Spdk nvmf transfer submitted to " << handle.transport_endpoint_;
    return TransferFuture(state);
#endif
}

std::optional<TransferFuture> TransferSubmitter::submitFileReadOperation(
    const Replica::Descriptor& replica, std::vector<Slice>& slices,
    TransferRequest::OpCode op_code) {
    auto state = std::make_shared<FilereadOperationState>();
    auto disk_replica = replica.get_disk_descriptor();
    std::string file_path = disk_replica.file_path;
    size_t file_length = disk_replica.object_size;

    // Submit memcpy operations to worker pool for async execution
    FilereadTask task(file_path, file_length, slices, state);
    fileread_pool_->submitTask(std::move(task));

    VLOG(1) << "Fileread transfer submitted to worker pool with " << file_path;

    return TransferFuture(state);
}

TransferStrategy TransferSubmitter::selectStrategy(
    const AllocatedBuffer::Descriptor& handle,
    const std::vector<Slice>& slices) const {
    // Check if memcpy operations are enabled via environment variable
    if (!memcpy_enabled_) {
        VLOG(2) << "Memcpy operations disabled via MC_STORE_MEMCPY environment "
                   "variable";
        return TransferStrategy::TRANSFER_ENGINE;
    }

    // Check conditions for local memcpy optimization
    if (isLocalTransfer(handle)) {
        return TransferStrategy::LOCAL_MEMCPY;
    }

    return TransferStrategy::TRANSFER_ENGINE;
}

bool TransferSubmitter::isLocalTransfer(
    const AllocatedBuffer::Descriptor& handle) const {
    std::string local_ep = engine_.getLocalIpAndPort();

    if (!local_ep.empty()) {
        return !handle.transport_endpoint_.empty() &&
               handle.transport_endpoint_ == local_ep;
    }

    // Without a local endpoint we cannot prove locality; disable memcpy.
    return false;
}

bool TransferSubmitter::validateTransferParams(
    const AllocatedBuffer::Descriptor& handle,
    const std::vector<Slice>& slices) const {
    uint64_t all_slice_len = 0;
    for (auto slice : slices) {
        all_slice_len += slice.size;
    }
    if (handle.size_ != all_slice_len) {
        LOG(ERROR) << "handles len:" << handle.size_
                   << ", all_slice_len:" << all_slice_len;
        return false;
    }
    return true;
}

void TransferSubmitter::updateTransferMetrics(const std::vector<Slice>& slices,
                                              TransferRequest::OpCode op_code) {
    size_t total_bytes = 0;
    for (const auto& slice : slices) {
        total_bytes += slice.size;
    }

    if (transfer_metric_ == nullptr) {
        return;
    }

    if (op_code == TransferRequest::READ) {
        transfer_metric_->total_read_bytes.inc(total_bytes);

    } else if (op_code == TransferRequest::WRITE) {
        transfer_metric_->total_write_bytes.inc(total_bytes);
    }
}

}  // namespace mooncake
