#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stack>
#include <string>
#include <vector>
#include <spdk/dma.h>
#include <spdk/env.h>
#include <spdk/nvme.h>

namespace mooncake {

#define INVALID_BLOCK_SIZE 0xFFFFFFFF

constexpr int kSpdkNofOpRead = 0;
constexpr int kSpdkNofOpWrite = 1;
constexpr int kSpdkNofOpNum = 2;

enum class SpdkNofMemoryKind : uint8_t {
    HOST_DMA = 0,
    GPU_DMABUF = 1,
};

struct nof_seg_handle;
struct tr_info;
struct ctrlr_info;

class SpdkWrapper {
   public:
    SpdkWrapper(const SpdkWrapper &) = delete;
    SpdkWrapper &operator=(const SpdkWrapper &) = delete;

    static SpdkWrapper &GetInstance();

    bool InitializeEnv();

    void Cleanup();

    void *Alloc(size_t size, size_t align, int socket_id = -1);

    void Free(void *ptr);

    int64_t NvmePollProcessCompletion(nof_seg_handle *seg,
                                      uint32_t complete_per_seg);

    bool IsGpuDmabufEnabled() const;

    int InitializeGpuDmabufDomain(int cuda_device_id = -1);

    int RegisterGpuBuffer(const nof_seg_handle *seg_handle, void *ptr,
                          size_t size);

    int RegisterGpuMemoryRegion(void *ptr, size_t size);

    void UnregisterGpuMemoryRegion(void *ptr);

    void InvalidateGpuBuffer(void *ptr, size_t size);

    /**
     * Classify a complete buffer range for task-level memory-domain selection.
     * Returns 0 for a host range or a fully registered GPU range. A range that
     * only partially overlaps a registered GPU region is rejected.
     */
    int ClassifyMemoryRegion(void *ptr, size_t size,
                             SpdkNofMemoryKind *memory_kind);

    /**
     * Acquire a preallocated host DMA tail buffer. Write padding shares the
     * immutable zero block; read padding receives an exclusive discard slot.
     */
    int AcquireHostScratch(size_t size, bool for_write, void **ptr,
                           std::shared_ptr<void> *owner);

    int AcquireGpuScratch(size_t size, bool for_write, void **ptr,
                          std::shared_ptr<void> *owner);

    /** @brief Open a NoF segment. */
    nof_seg_handle *OpenNofSegment(const std::string &tr_str);

    uint32_t GetBlockSize(const nof_seg_handle *seg_handle);

    int SubmitRequest(const nof_seg_handle *seg_handle, void *ptr, uint64_t lba,
                      uint32_t lba_count, int op, spdk_nvme_cmd_cb cb_fn,
                      void *cb_ctx);

    int SubmitRequestVExt(
        const nof_seg_handle *seg_handle, uint64_t lba, uint32_t lba_count,
        int op, spdk_nvme_cmd_cb cb_fn, void *cb_ctx,
        spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
        spdk_nvme_req_next_sge_cb next_sge_fn,
        SpdkNofMemoryKind memory_kind,
        struct spdk_nvme_ns_cmd_ext_io_opts *io_opts);

    bool ProbeNofSegment(const std::string &tr_str, uint32_t timeout_ms,
                         std::string *error_reason = nullptr);

   private:
    struct ProbeBuffer {
        void *ptr{nullptr};
        uint32_t size{0};

        ProbeBuffer() = default;
        ProbeBuffer(const ProbeBuffer &) = delete;
        ProbeBuffer &operator=(const ProbeBuffer &) = delete;
        ProbeBuffer(ProbeBuffer &&) = delete;
        ProbeBuffer &operator=(ProbeBuffer &&) = delete;
    };

    struct ProbeRequestContext {
        std::atomic<bool> done{false};
        std::atomic<bool> success{false};
        std::mutex error_mutex;
        std::string error_reason;
        SpdkWrapper *owner{nullptr};

        void Reset(SpdkWrapper *wrapper) {
            std::lock_guard<std::mutex> lock(error_mutex);
            owner = wrapper;
            done.store(false, std::memory_order_release);
            success.store(false, std::memory_order_release);
            error_reason.clear();
        }
    };

    explicit SpdkWrapper();
    ~SpdkWrapper();

    int ParseTransPortStr(const std::string &tr_str, tr_info *info);
    int ConnectController(const struct spdk_nvme_transport_id *trid,
                          ctrlr_info *info);
    int CreateGpuDmabufDomain(int cuda_device_id);
    bool IsGpuMemoryRegionRegistered(void *ptr, size_t size);
    ProbeBuffer *GetOrCreateProbeBuffer(const std::string &tr_str,
                                        uint32_t block_size,
                                        std::string *error_reason);
    ProbeRequestContext *AcquireProbeRequestContext();
    void RecycleProbeRequestContext(ProbeRequestContext *ctx);
    void ReplenishProbeRequestContextPoolLocked(size_t count);
    int InitializeHostScratch();
    int InitializeGpuScratch(int cuda_device_id);
    void CleanupGpuScratch();
    void ReleaseHostScratchSlot(size_t slot_index);
    void ReleaseGpuScratchSlot(size_t slot_index);
    static void ProbeReadComplete(void *ctx, const struct spdk_nvme_cpl *cpl);

    std::atomic<bool> initialized{false};
    std::mutex init_mutex;
    std::map<std::string, std::unique_ptr<ctrlr_info>> connected_ctrlrs;
    std::mutex ctrlrs_mutex;
    std::map<std::string, std::unique_ptr<ProbeBuffer>> probe_buffers_;
    std::mutex probe_buffers_mutex_;
    std::vector<std::unique_ptr<ProbeRequestContext>> probe_request_contexts_;
    std::stack<ProbeRequestContext *> probe_request_context_pool_;
    std::mutex probe_request_context_pool_mutex_;
    std::atomic<bool> gpu_dmabuf_enabled_{false};
    struct spdk_memory_domain *gpu_dmabuf_domain_{nullptr};
    std::atomic<int> gpu_cuda_device_id_{-1};
    std::map<uintptr_t, size_t> gpu_memory_regions_;
    std::mutex gpu_dmabuf_mutex_;
    void *host_scratch_slab_{nullptr};
    size_t host_scratch_slot_size_{0};
    size_t host_scratch_read_slot_count_{0};
    std::stack<size_t> host_scratch_free_slots_;
    std::mutex host_scratch_mutex_;
    void *gpu_scratch_slab_{nullptr};
    size_t gpu_scratch_slot_size_{0};
    size_t gpu_scratch_read_slot_count_{0};
    std::stack<size_t> gpu_scratch_free_slots_;
    std::mutex gpu_scratch_mutex_;
};

}  // namespace mooncake
