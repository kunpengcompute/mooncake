#include <glog/logging.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <thread>
#include "spdk/spdk_wrapper.h"

#if defined(USE_SPDK_GPU_DMABUF)
#include <spdk/gpu_dmabuf.h>
#define MOONCAKE_HAS_SPDK_GPU_DMABUF 1
#else
#define MOONCAKE_HAS_SPDK_GPU_DMABUF 0
#endif

namespace mooncake {
namespace {

bool ParseEnvU64(const char *name, uint64_t *out) {
    const char *val = std::getenv(name);
    if (!val || *val == '\0') {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(val, &end, 10);
    if (errno != 0 || end == val || (end && *end != '\0')) {
        LOG(WARNING) << "Invalid value for " << name << ": " << val;
        return false;
    }

    *out = static_cast<uint64_t>(parsed);
    return true;
}

bool ParseEnvBool(const char *name, bool *out) {
    uint64_t v = 0;
    if (!ParseEnvU64(name, &v)) {
        return false;
    }
    *out = (v != 0);
    return true;
}

void ApplyCtrlrOptsFromEnv(struct spdk_nvme_ctrlr_opts *opts) {
    uint64_t v = 0;
    bool bv = false;
    opts->keep_alive_timeout_ms = 0;

    if (ParseEnvU64("MC_NVME_NUM_IO_QUEUES", &v)) {
        opts->num_io_queues = static_cast<uint32_t>(v);
    }
    if (ParseEnvU64("MC_NVME_IO_QUEUE_SIZE", &v)) {
        opts->io_queue_size = static_cast<uint32_t>(v);
    }
    if (ParseEnvU64("MC_NVME_IO_QUEUE_REQUESTS", &v)) {
        opts->io_queue_requests = static_cast<uint32_t>(v);
    }
    if (ParseEnvU64("MC_NVME_TRANSPORT_ACK_TIMEOUT", &v)) {
        opts->transport_ack_timeout = static_cast<uint8_t>(v);
    }
    if (ParseEnvU64("MC_NVME_ADMIN_QUEUE_SIZE", &v)) {
        opts->admin_queue_size = static_cast<uint16_t>(v);
    }
    if (ParseEnvU64("MC_NVME_FABRICS_CONNECT_TIMEOUT_US", &v)) {
        opts->fabrics_connect_timeout_us = v;
    }
    if (ParseEnvBool("MC_NVME_HEADER_DIGEST", &bv)) {
        opts->header_digest = bv;
    }
    if (ParseEnvBool("MC_NVME_DATA_DIGEST", &bv)) {
        opts->data_digest = bv;
    }
    LOG(INFO) << "NVMe ctrlr opts: num_io_queues=" << opts->num_io_queues
              << ", io_queue_size=" << opts->io_queue_size
              << ", io_queue_requests=" << opts->io_queue_requests
              << ", keep_alive_timeout_ms=" << opts->keep_alive_timeout_ms
              << ", transport_ack_timeout="
              << static_cast<int>(opts->transport_ack_timeout)
              << ", admin_queue_size=" << opts->admin_queue_size
              << ", fabrics_connect_timeout_us="
              << opts->fabrics_connect_timeout_us
              << ", header_digest=" << opts->header_digest
              << ", data_digest=" << opts->data_digest;
}

}  // namespace

struct nof_seg_handle {
    struct spdk_nvme_qpair *qpair;
    struct spdk_nvme_ns *ns;
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_memory_domain *rdma_domain;
};

struct tr_info {
    struct spdk_nvme_transport_id trid;
    std::string ctrlr_key;
    uint32_t ns;
};

struct ctrlr_info {
    struct spdk_nvme_ctrlr *ctrlr;
    std::map<uint32_t, std::unique_ptr<nof_seg_handle>> ns_seg;
    std::mutex ns_mutex;
};

SpdkWrapper::SpdkWrapper() = default;

SpdkWrapper::~SpdkWrapper() { Cleanup(); }

SpdkWrapper &SpdkWrapper::GetInstance() {
    static SpdkWrapper ins;
    return ins;
}

bool SpdkWrapper::InitializeEnv() {
    if (initialized.load(std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(init_mutex);
    if (initialized.load(std::memory_order_acquire)) {
        return true;
    }

    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "mooncake";

    int rc = spdk_env_init(&opts);
    if (rc != 0) {
        fprintf(stderr, "SPDK init failed: %d\n", rc);
        return false;
    }

    // Mark SPDK as initialized.
    initialized.store(true, std::memory_order_release);

    bool enable_gpu_dmabuf = false;
    uint64_t cuda_device_id = 0;
    if (ParseEnvBool("MC_SPDK_GPU_DMABUF", &enable_gpu_dmabuf) &&
        enable_gpu_dmabuf) {
        int device_id = -1;
        if (ParseEnvU64("MC_SPDK_GPU_DMABUF_DEVICE_ID", &cuda_device_id)) {
            device_id = static_cast<int>(cuda_device_id);
        }
        rc = CreateGpuDmabufDomain(device_id);
        if (rc != 0) {
            LOG(ERROR) << "Failed to create SPDK GPU dma-buf domain, rc="
                       << rc;
            spdk_env_fini();
            initialized.store(false, std::memory_order_release);
            return false;
        }
        LOG(INFO) << "SPDK GPU dma-buf domain enabled, cuda_device_id="
                  << device_id;
    }

    rc = InitializeHostScratch();
    if (rc != 0) {
        LOG(ERROR) << "Failed to initialize NoF host scratch pool, rc=" << rc;
        Cleanup();
        return false;
    }
    return true;
}

void SpdkWrapper::Cleanup() {
    if (initialized.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
            if (gpu_dmabuf_domain_ != nullptr) {
#if MOONCAKE_HAS_SPDK_GPU_DMABUF
                spdk_gpu_dmabuf_memory_domain_destroy(gpu_dmabuf_domain_);
#endif
                gpu_dmabuf_domain_ = nullptr;
            }
            gpu_memory_regions_.clear();
            gpu_dmabuf_enabled_.store(false, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lock(ctrlrs_mutex);
            for (auto &[_, info] : connected_ctrlrs) {
                if (info) {
                    // Free all qpairs and segment handles
                    for (auto &[_, seg] : info->ns_seg) {
                        if (seg && seg->qpair) {
                            spdk_nvme_ctrlr_free_io_qpair(seg->qpair);
                        }
                    }
                    // Detach controller
                    if (info->ctrlr) {
                        spdk_nvme_detach(info->ctrlr);
                    }
                }
            }
            connected_ctrlrs.clear();
        }

        {
            std::lock_guard<std::mutex> lock(probe_buffers_mutex_);
            for (auto &[_, probe_buffer] : probe_buffers_) {
                if (probe_buffer && probe_buffer->ptr) {
                    spdk_free(probe_buffer->ptr);
                    probe_buffer->ptr = nullptr;
                    probe_buffer->size = 0;
                }
            }
            probe_buffers_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(host_scratch_mutex_);
            while (!host_scratch_free_slots_.empty()) {
                host_scratch_free_slots_.pop();
            }
            if (host_scratch_slab_ != nullptr) {
                spdk_free(host_scratch_slab_);
                host_scratch_slab_ = nullptr;
            }
            host_scratch_slot_size_ = 0;
            host_scratch_read_slot_count_ = 0;
        }
        spdk_env_fini();
        initialized.store(false, std::memory_order_release);
    }
}

int SpdkWrapper::InitializeHostScratch() {
    uint64_t slot_size = 4096;
    uint64_t read_slot_count = 1024;
    ParseEnvU64("MC_NOF_HOST_SCRATCH_SLOT_SIZE", &slot_size);
    ParseEnvU64("MC_NOF_HOST_SCRATCH_SLOTS", &read_slot_count);
    if (slot_size == 0 || read_slot_count == 0 ||
        read_slot_count == std::numeric_limits<uint64_t>::max() ||
        slot_size > std::numeric_limits<size_t>::max() /
                        (read_slot_count + 1)) {
        return -EINVAL;
    }

    const size_t total_size =
        static_cast<size_t>(slot_size * (read_slot_count + 1));
    void *slab = spdk_zmalloc(total_size, 4096, nullptr,
                              SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (slab == nullptr) {
        return -ENOMEM;
    }

    std::lock_guard<std::mutex> lock(host_scratch_mutex_);
    host_scratch_slab_ = slab;
    host_scratch_slot_size_ = static_cast<size_t>(slot_size);
    host_scratch_read_slot_count_ = static_cast<size_t>(read_slot_count);
    for (size_t i = 1; i <= host_scratch_read_slot_count_; ++i) {
        host_scratch_free_slots_.push(i);
    }
    LOG(INFO) << "Initialized NoF host scratch pool: slot_size="
              << host_scratch_slot_size_
              << ", read_slots=" << host_scratch_read_slot_count_;
    return 0;
}

int SpdkWrapper::AcquireHostScratch(size_t size, bool for_write, void **ptr,
                                    std::shared_ptr<void> *owner) {
    if (ptr == nullptr || owner == nullptr || size == 0) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(host_scratch_mutex_);
    if (host_scratch_slab_ == nullptr || size > host_scratch_slot_size_) {
        return -E2BIG;
    }

    if (for_write) {
        *ptr = host_scratch_slab_;
        owner->reset();
        return 0;
    }
    if (host_scratch_free_slots_.empty()) {
        return -EAGAIN;
    }

    const size_t slot_index = host_scratch_free_slots_.top();
    host_scratch_free_slots_.pop();
    *ptr = static_cast<char *>(host_scratch_slab_) +
           slot_index * host_scratch_slot_size_;
    *owner = std::shared_ptr<void>(
        *ptr, [this, slot_index](void *) { ReleaseHostScratchSlot(slot_index); });
    return 0;
}

void SpdkWrapper::ReleaseHostScratchSlot(size_t slot_index) {
    std::lock_guard<std::mutex> lock(host_scratch_mutex_);
    if (host_scratch_slab_ != nullptr && slot_index != 0 &&
        slot_index <= host_scratch_read_slot_count_) {
        host_scratch_free_slots_.push(slot_index);
    }
}

bool SpdkWrapper::IsGpuDmabufEnabled() const {
    return gpu_dmabuf_enabled_.load(std::memory_order_acquire);
}

int SpdkWrapper::CreateGpuDmabufDomain(int cuda_device_id) {
#if MOONCAKE_HAS_SPDK_GPU_DMABUF
    {
        std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
        if (gpu_dmabuf_domain_ == nullptr) {
            struct spdk_gpu_dmabuf_memory_domain_opts opts;
            spdk_gpu_dmabuf_memory_domain_get_opts(&opts);
            opts.cuda_device_id = cuda_device_id;

            int rc =
                spdk_gpu_dmabuf_memory_domain_create(&gpu_dmabuf_domain_,
                                                     &opts);
            if (rc != 0) {
                gpu_dmabuf_domain_ = nullptr;
                gpu_dmabuf_enabled_.store(false, std::memory_order_release);
                return rc;
            }
        }
        gpu_dmabuf_enabled_.store(true, std::memory_order_release);
    }

    return 0;
#else
    (void)cuda_device_id;
    return -ENOTSUP;
#endif
}

int SpdkWrapper::InitializeGpuDmabufDomain(int cuda_device_id) {
    if (!InitializeEnv()) {
        return -ENODEV;
    }

    return CreateGpuDmabufDomain(cuda_device_id);
}

int SpdkWrapper::RegisterGpuBuffer(const nof_seg_handle *seg_handle, void *ptr,
                                   size_t size) {
    if (!IsGpuDmabufEnabled()) {
        return -ENOTSUP;
    }
    if (!seg_handle || !seg_handle->rdma_domain || !ptr || size == 0) {
        return -EINVAL;
    }
    if (!IsGpuMemoryRegionRegistered(ptr, size)) {
        return 0;
    }

#if MOONCAKE_HAS_SPDK_GPU_DMABUF
    return spdk_gpu_dmabuf_memory_domain_register(
        gpu_dmabuf_domain_, seg_handle->rdma_domain, ptr, size);
#else
    return -ENOTSUP;
#endif
}

int SpdkWrapper::RegisterGpuMemoryRegion(void *ptr, size_t size) {
    if (!IsGpuDmabufEnabled()) {
        return 0;
    }
    if (!ptr || size == 0) {
        return -EINVAL;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (size > std::numeric_limits<uintptr_t>::max() - addr) {
        return -EINVAL;
    }
    uintptr_t end = addr + size;

    {
        std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
        auto next = gpu_memory_regions_.upper_bound(addr);
        if (next != gpu_memory_regions_.begin()) {
            auto prev = std::prev(next);
            uintptr_t prev_addr = prev->first;
            size_t prev_size = prev->second;
            uintptr_t prev_end = prev_addr + prev_size;
            if (addr >= prev_addr && end <= prev_end) {
                return 0;
            }
            if (addr < prev_end) {
                return -EINVAL;
            }
        }
        if (next != gpu_memory_regions_.end() && end > next->first) {
            return -EINVAL;
        }

        gpu_memory_regions_[addr] = size;
    }

    std::vector<nof_seg_handle *> seg_handles;
    {
        std::lock_guard<std::mutex> lock(ctrlrs_mutex);
        for (auto &[_, info] : connected_ctrlrs) {
            if (!info) {
                continue;
            }
            std::lock_guard<std::mutex> ns_lock(info->ns_mutex);
            for (auto &[_, seg] : info->ns_seg) {
                if (seg && seg->rdma_domain != nullptr) {
                    seg_handles.push_back(seg.get());
                }
            }
        }
    }

    for (nof_seg_handle *seg_handle : seg_handles) {
        int rc = RegisterGpuBuffer(seg_handle, ptr, size);
        if (rc != 0) {
            {
                std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
                auto it = gpu_memory_regions_.find(addr);
                if (it != gpu_memory_regions_.end() && it->second == size) {
                    gpu_memory_regions_.erase(it);
                }
            }
            InvalidateGpuBuffer(ptr, size);
            return rc;
        }
    }
    return 0;
}

bool SpdkWrapper::IsGpuMemoryRegionRegistered(void *ptr, size_t size) {
    if (!ptr || size == 0) {
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
    auto it = gpu_memory_regions_.upper_bound(addr);
    if (it == gpu_memory_regions_.begin()) {
        return false;
    }
    --it;

    uintptr_t region_addr = it->first;
    size_t region_size = it->second;
    size_t offset = addr - region_addr;
    return offset <= region_size && size <= region_size - offset;
}

int SpdkWrapper::ClassifyMemoryRegion(void *ptr, size_t size,
                                      SpdkNofMemoryKind *memory_kind) {
    if (!ptr || size == 0 || !memory_kind) {
        return -EINVAL;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (size > std::numeric_limits<uintptr_t>::max() - addr) {
        return -EINVAL;
    }
    uintptr_t end = addr + size;

    std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
    auto next = gpu_memory_regions_.upper_bound(addr);
    if (next != gpu_memory_regions_.begin()) {
        auto current = std::prev(next);
        uintptr_t region_addr = current->first;
        size_t region_size = current->second;
        if (region_size > std::numeric_limits<uintptr_t>::max() - region_addr) {
            return -EINVAL;
        }
        uintptr_t region_end = region_addr + region_size;
        if (addr >= region_addr && end <= region_end) {
            *memory_kind = SpdkNofMemoryKind::GPU_DMABUF;
            return 0;
        }
        if (addr < region_end) {
            return -ERANGE;
        }
    }

    if (next != gpu_memory_regions_.end() && end > next->first) {
        return -ERANGE;
    }

    *memory_kind = SpdkNofMemoryKind::HOST_DMA;
    return 0;
}

void SpdkWrapper::UnregisterGpuMemoryRegion(void *ptr) {
    if (!ptr) {
        return;
    }

    size_t size = 0;
    {
        std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
        auto it = gpu_memory_regions_.find(reinterpret_cast<uintptr_t>(ptr));
        if (it == gpu_memory_regions_.end()) {
            return;
        }
        size = it->second;
        gpu_memory_regions_.erase(it);
    }

    InvalidateGpuBuffer(ptr, size);
}

void SpdkWrapper::InvalidateGpuBuffer(void *ptr, size_t size) {
    if (!IsGpuDmabufEnabled() || !ptr || size == 0) {
        return;
    }

#if MOONCAKE_HAS_SPDK_GPU_DMABUF
    spdk_gpu_dmabuf_memory_domain_invalidate(gpu_dmabuf_domain_, ptr, size);
#endif
}

void *SpdkWrapper::Alloc(size_t size, size_t align, int socket_id) {
    if (!InitializeEnv()) {
        return nullptr;
    }

    return spdk_zmalloc(size, align, nullptr, socket_id, SPDK_MALLOC_DMA);
}

void SpdkWrapper::Free(void *ptr) {
    if (ptr) {
        spdk_free(ptr);
    }
}

void SpdkWrapper::ProbeReadComplete(void *ctx,
                                    const struct spdk_nvme_cpl *cpl) {
    auto *probe_ctx = reinterpret_cast<ProbeRequestContext *>(ctx);
    if (spdk_nvme_cpl_is_error(cpl)) {
        {
            std::lock_guard<std::mutex> lock(probe_ctx->error_mutex);
            probe_ctx->error_reason =
                std::string("completion_error:") +
                spdk_nvme_cpl_get_status_string(&cpl->status);
        }
        probe_ctx->success.store(false, std::memory_order_release);
    } else {
        probe_ctx->success.store(true, std::memory_order_release);
    }
    probe_ctx->done.store(true, std::memory_order_release);
    if (probe_ctx->owner != nullptr) {
        probe_ctx->owner->RecycleProbeRequestContext(probe_ctx);
    }
}

void SpdkWrapper::ReplenishProbeRequestContextPoolLocked(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        auto probe_ctx = std::make_unique<ProbeRequestContext>();
        probe_request_context_pool_.push(probe_ctx.get());
        probe_request_contexts_.push_back(std::move(probe_ctx));
    }
}

SpdkWrapper::ProbeRequestContext *SpdkWrapper::AcquireProbeRequestContext() {
    std::lock_guard<std::mutex> lock(probe_request_context_pool_mutex_);
    if (probe_request_context_pool_.empty()) {
        ReplenishProbeRequestContextPoolLocked(8);
    }
    auto *probe_ctx = probe_request_context_pool_.top();
    probe_request_context_pool_.pop();
    probe_ctx->Reset(this);
    return probe_ctx;
}

void SpdkWrapper::RecycleProbeRequestContext(ProbeRequestContext *ctx) {
    if (ctx == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(probe_request_context_pool_mutex_);
    probe_request_context_pool_.push(ctx);
}

int64_t SpdkWrapper::NvmePollProcessCompletion(nof_seg_handle *seg,
                                               uint32_t complete_per_seg) {
    return spdk_nvme_qpair_process_completions(seg->qpair, complete_per_seg);
}

int SpdkWrapper::ParseTransPortStr(const std::string &tr_str, tr_info *info) {
    std::memset(&info->trid, 0, sizeof(info->trid));
    info->ns = 1;

    if (spdk_nvme_transport_id_parse(&info->trid, tr_str.c_str()) != 0) {
        LOG(ERROR) << "Error parsing transport address";
        return -1;
    }

    std::string ns_prefix = "ns:";
    size_t ns_pos = tr_str.find(ns_prefix);
    if (ns_pos != std::string::npos) {
        size_t ns_start = ns_pos + ns_prefix.length();
        size_t ns_end = tr_str.find_first_of(" \t", ns_start);

        std::string ns_str;
        if (ns_end == std::string::npos) {
            ns_str = tr_str.substr(ns_start);
        } else {
            ns_str = tr_str.substr(ns_start, ns_end - ns_start);
        }

        try {
            info->ns = std::stoul(ns_str);
        } catch (const std::exception &e) {
            LOG(ERROR) << "Failed to parse ns value: " << ns_str
                       << ", error: " << e.what();
            return -1;
        }
    } else {
        LOG(ERROR) << "No ns field found in transport string";
    }

    info->ctrlr_key = std::string(info->trid.traddr) + "|" +
                      std::string(info->trid.trsvcid) + "|" +
                      std::string(info->trid.subnqn) + "|" +
                      std::to_string(static_cast<int>(info->trid.trtype));

    LOG(INFO) << "traddr:" << info->trid.traddr
              << "trsvcid:" << info->trid.trsvcid << "ns:" << info->ns
              << "subnqn:" << info->trid.subnqn
              << "trtype:" << info->trid.trtype;

    return 0;
}

int SpdkWrapper::ConnectController(const struct spdk_nvme_transport_id *trid,
                                   ctrlr_info *info) {
    auto probe_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                       struct spdk_nvme_ctrlr_opts *opts) -> bool {
        ApplyCtrlrOptsFromEnv(opts);
        LOG(INFO) << "Attaching to " << trid->traddr << " " << trid->subnqn;
        return true;
    };

    auto attach_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                        struct spdk_nvme_ctrlr *ctrlr,
                        const struct spdk_nvme_ctrlr_opts *opts) {
        LOG(INFO) << "Attached to " << trid->traddr << " " << trid->subnqn;
        ctrlr_info *info = (ctrlr_info *)cb_ctx;
        info->ctrlr = ctrlr;
    };

    return spdk_nvme_probe(trid, (void *)info, probe_cb, attach_cb, NULL);
}

nof_seg_handle *SpdkWrapper::OpenNofSegment(const std::string &tr_str) {
    if (!InitializeEnv()) {
        return nullptr;
    }

    tr_info tr;
    int ret = ParseTransPortStr(tr_str, &tr);
    if (ret != 0) {
        return nullptr;
    }

    ctrlr_info *info = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctrlrs_mutex);
        auto it = connected_ctrlrs.find(tr.ctrlr_key);
        if (it == connected_ctrlrs.end()) {
            auto new_info = std::make_unique<ctrlr_info>();
            info = new_info.get();

            ret = ConnectController(&tr.trid, info);
            if (ret != 0) {
                return nullptr;
            }

            connected_ctrlrs[tr.ctrlr_key] = std::move(new_info);
        } else {
            info = it->second.get();
        }
    }

    nof_seg_handle *seg_handle = nullptr;
    struct spdk_nvme_qpair *qpair = nullptr;
    struct spdk_nvme_ns *ns = nullptr;
    {
        auto &ns_seg = info->ns_seg;
        std::lock_guard<std::mutex> lock(info->ns_mutex);
        auto ns_it = ns_seg.find(tr.ns);
        if (ns_it != ns_seg.end()) {
            return ns_it->second.get();
        }

        if (spdk_nvme_ctrlr_is_active_ns(info->ctrlr, tr.ns)) {
            ns = spdk_nvme_ctrlr_get_ns(info->ctrlr, tr.ns);
        } else {
            LOG(ERROR) << "spdk_nvme_ctrlr_is_active_ns failed";
            return nullptr;
        }

        qpair = spdk_nvme_ctrlr_alloc_io_qpair(info->ctrlr, nullptr, 0);
        if (!qpair) {
            LOG(ERROR) << "alloc spdk_nvme_qpair failed";
            return nullptr;
        }

        auto new_seg = std::make_unique<nof_seg_handle>();
        new_seg->qpair = qpair;
        new_seg->ns = ns;
        new_seg->ctrlr = info->ctrlr;
        new_seg->rdma_domain = nullptr;
        struct spdk_memory_domain *domains[8] = {};
        int domain_count =
            spdk_nvme_ctrlr_get_memory_domains(info->ctrlr, domains, 8);
        if (domain_count < 0 && IsGpuDmabufEnabled()) {
            LOG(WARNING) << "Failed to query SPDK controller memory domains, rc="
                         << domain_count;
        }
        for (int i = 0; i < domain_count && i < 8; ++i) {
            if (domains[i] != nullptr &&
                spdk_memory_domain_get_dma_device_type(domains[i]) ==
                    SPDK_DMA_DEVICE_TYPE_RDMA) {
                new_seg->rdma_domain = domains[i];
                break;
            }
        }
        if (IsGpuDmabufEnabled() && new_seg->rdma_domain == nullptr) {
            LOG(WARNING) << "SPDK GPU dma-buf enabled but controller has no "
                         << "RDMA memory domain";
        }
        if (new_seg->rdma_domain != nullptr && IsGpuDmabufEnabled()) {
            std::vector<std::pair<void *, size_t>> regions;
            {
                std::lock_guard<std::mutex> lock(gpu_dmabuf_mutex_);
                regions.reserve(gpu_memory_regions_.size());
                for (auto &[addr, size] : gpu_memory_regions_) {
                    regions.emplace_back(reinterpret_cast<void *>(addr), size);
                }
            }

            for (auto &[addr, size] : regions) {
                ret = RegisterGpuBuffer(new_seg.get(), addr, size);
                if (ret != 0) {
                    LOG(WARNING)
                        << "Failed to pre-register GPU buffer for SPDK NoF, ptr="
                        << addr << ", size=" << size << ", ret=" << ret;
                }
            }
        }
        seg_handle = new_seg.get();
        ns_seg[tr.ns] = std::move(new_seg);
    }

    return seg_handle;
}

uint32_t SpdkWrapper::GetBlockSize(const nof_seg_handle *seg_handle) {
    if (!seg_handle || !seg_handle->ns) {
        return INVALID_BLOCK_SIZE;
    }

    return spdk_nvme_ns_get_sector_size(seg_handle->ns);
}

int SpdkWrapper::SubmitRequest(const nof_seg_handle *seg_handle, void *ptr,
                               uint64_t lba, uint32_t lba_count, int op,
                               spdk_nvme_cmd_cb cb_fn, void *cb_ctx) {
    if (!seg_handle || !ptr || !lba_count || !seg_handle->qpair ||
        !seg_handle->ns) {
        return -1;
    }

    struct spdk_nvme_qpair *qpair = seg_handle->qpair;
    struct spdk_nvme_ns *ns = seg_handle->ns;
    if (op == kSpdkNofOpRead) {
        return spdk_nvme_ns_cmd_read(ns, qpair, ptr, lba, lba_count, cb_fn,
                                     cb_ctx, 0);
    } else if (op == kSpdkNofOpWrite) {
        return spdk_nvme_ns_cmd_write(ns, qpair, ptr, lba, lba_count, cb_fn,
                                      cb_ctx, 0);
    }
    return -1;
}

int SpdkWrapper::SubmitRequestVExt(
    const nof_seg_handle *seg_handle, uint64_t lba, uint32_t lba_count, int op,
    spdk_nvme_cmd_cb cb_fn, void *cb_ctx,
    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
    spdk_nvme_req_next_sge_cb next_sge_fn,
    SpdkNofMemoryKind memory_kind,
    struct spdk_nvme_ns_cmd_ext_io_opts *io_opts) {
    if (!seg_handle || !lba_count || !seg_handle->qpair || !seg_handle->ns ||
        !reset_sgl_fn || !next_sge_fn) {
        return -EINVAL;
    }

    struct spdk_nvme_ns_cmd_ext_io_opts *opts = nullptr;
    if (memory_kind == SpdkNofMemoryKind::GPU_DMABUF) {
        if (!IsGpuDmabufEnabled() || gpu_dmabuf_domain_ == nullptr ||
            io_opts == nullptr) {
            return -EINVAL;
        }

        uint32_t block_size = spdk_nvme_ns_get_sector_size(seg_handle->ns);
        size_t remaining = static_cast<size_t>(lba_count) * block_size;
        if (block_size == 0 || remaining / block_size != lba_count) {
            return -EINVAL;
        }

        reset_sgl_fn(cb_ctx, 0);
        while (remaining > 0) {
            void *address = nullptr;
            uint32_t length = 0;
            int rc = next_sge_fn(cb_ctx, &address, &length);
            if (rc != 0 || address == nullptr || length == 0 ||
                length > remaining ||
                !IsGpuMemoryRegionRegistered(address, length)) {
                LOG(ERROR)
                    << "SPDK GPU dma-buf vector submit uses an invalid or "
                       "unregistered segment, ptr="
                    << address << ", size=" << length;
                return -EINVAL;
            }
            remaining -= length;
        }
        reset_sgl_fn(cb_ctx, 0);

        std::memset(io_opts, 0, sizeof(*io_opts));
        io_opts->size = sizeof(*io_opts);
        io_opts->memory_domain = gpu_dmabuf_domain_;
        io_opts->memory_domain_ctx = nullptr;
        io_opts->io_flags = 0;
        opts = io_opts;
    } else if (memory_kind != SpdkNofMemoryKind::HOST_DMA) {
        return -EINVAL;
    }

    struct spdk_nvme_qpair *qpair = seg_handle->qpair;
    struct spdk_nvme_ns *ns = seg_handle->ns;
    if (op == kSpdkNofOpRead) {
        return spdk_nvme_ns_cmd_readv_ext(
            ns, qpair, lba, lba_count, cb_fn, cb_ctx, reset_sgl_fn,
            next_sge_fn, opts);
    } else if (op == kSpdkNofOpWrite) {
        return spdk_nvme_ns_cmd_writev_ext(
            ns, qpair, lba, lba_count, cb_fn, cb_ctx, reset_sgl_fn,
            next_sge_fn, opts);
    }
    return -EINVAL;
}

SpdkWrapper::ProbeBuffer *SpdkWrapper::GetOrCreateProbeBuffer(
    const std::string &tr_str, uint32_t block_size, std::string *error_reason) {
    std::lock_guard<std::mutex> lock(probe_buffers_mutex_);
    auto &probe_buffer = probe_buffers_[tr_str];
    if (!probe_buffer) {
        probe_buffer = std::make_unique<ProbeBuffer>();
    }

    if (probe_buffer->ptr != nullptr && probe_buffer->size == block_size) {
        return probe_buffer.get();
    }

    if (probe_buffer->ptr != nullptr) {
        spdk_free(probe_buffer->ptr);
        probe_buffer->ptr = nullptr;
        probe_buffer->size = 0;
    }

    probe_buffer->ptr =
        spdk_zmalloc(block_size, 0x1000, nullptr, -1, SPDK_MALLOC_DMA);
    if (!probe_buffer->ptr) {
        if (error_reason) {
            *error_reason = "alloc_fail";
        }
        return nullptr;
    }
    probe_buffer->size = block_size;
    return probe_buffer.get();
}

bool SpdkWrapper::ProbeNofSegment(const std::string &tr_str,
                                  uint32_t timeout_ms,
                                  std::string *error_reason) {
    if (!InitializeEnv()) {
        if (error_reason) {
            *error_reason = "spdk_env_init_fail";
        }
        return false;
    }

    nof_seg_handle *seg_handle = OpenNofSegment(tr_str);
    if (!seg_handle) {
        if (error_reason) {
            *error_reason = "open_fail";
        }
        return false;
    }

    uint32_t block_size = GetBlockSize(seg_handle);
    if (block_size == INVALID_BLOCK_SIZE || block_size == 0) {
        if (error_reason) {
            *error_reason = "invalid_block_size";
        }
        return false;
    }

    ProbeBuffer *probe_buffer =
        GetOrCreateProbeBuffer(tr_str, block_size, error_reason);
    if (!probe_buffer || !probe_buffer->ptr) {
        return false;
    }

    ProbeRequestContext *probe_ctx = AcquireProbeRequestContext();
    int ret = SubmitRequest(seg_handle, probe_buffer->ptr, 0, 1, kSpdkNofOpRead,
                            ProbeReadComplete, probe_ctx);
    if (ret != 0) {
        RecycleProbeRequestContext(probe_ctx);
        if (error_reason) {
            *error_reason = "submit_fail";
        }
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (!probe_ctx->done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        NvmePollProcessCompletion(seg_handle, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool ok = probe_ctx->done.load(std::memory_order_acquire) &&
              probe_ctx->success.load(std::memory_order_acquire);
    if (!ok && error_reason) {
        if (!probe_ctx->done.load(std::memory_order_acquire)) {
            *error_reason = "completion_timeout";
        } else {
            std::lock_guard<std::mutex> lock(probe_ctx->error_mutex);
            *error_reason = probe_ctx->error_reason.empty()
                                ? "completion_error"
                                : probe_ctx->error_reason;
        }
    }

    return ok;
}

}  // namespace mooncake
