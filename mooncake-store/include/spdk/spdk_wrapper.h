#pragma once
#include <cstddef>
#include <mutex>
#include <string>
#include <map>
#include <spdk/env.h>
#include <spdk/nvme.h>

namespace mooncake {

#define INVALID_BLOCK_SIZE 0xFFFFFFFF

struct nof_seg_handle;
struct tr_info;
struct ctrlr_info;

class SpdkWrapper {
public:

    SpdkWrapper(const SpdkWrapper&) = delete;
    SpdkWrapper& operator=(const SpdkWrapper&) = delete;

    static SpdkWrapper& GetInstance();

    bool InitializeEnv();

    void Cleanup();

    void* Alloc(size_t size, size_t align, int socket_id = -1);

    void Free(void *ptr);

    void* NvmePollGroupCreate();

    int NvmePollGroupDestroy(void *group);

    int NvmePollGroupAdd(void *group, nof_seg_handle *seg);

    int NvmePollGroupRemove(void *group, nof_seg_handle *seg);

    int64_t NvmePollGroupProcessCompletion(void *group, uint32_t complete_per_seg);

    /**
     * @brief 获取传输通道
     *
     * 连接控制器
     * 
     * @param tr_str transport description
     * @return nof_seg handle the async operation, or nullptr
     * failure
     */
    nof_seg_handle *OpenNofSegment(const std::string &tr_str);

    uint32_t GetBlockSize(const nof_seg_handle *seg_handle);

    int SubmitRequest(const nof_seg_handle *seg_handle, void *ptr, uint64_t lba, uint32_t lba_count, 
        int op, spdk_nvme_cmd_cb cb_fn, void *cb_ctx);

private:
    explicit SpdkWrapper();
    ~SpdkWrapper();

    int ParseTransPortStr(const std::string &tr_str, tr_info *info);
    int ConnectController(const struct spdk_nvme_transport_id *trid, ctrlr_info *info);

    bool initialized;
    std::mutex init_mutex;
    std::map<std::string, ctrlr_info*> connected_ctrlrs;
    std::mutex ctrlrs_mutex;
};

}