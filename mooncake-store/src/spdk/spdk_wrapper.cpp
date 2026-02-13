#include <glog/logging.h>

#include <atomic>
#include "spdk/spdk_wrapper.h"

static void disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx) {

}

namespace mooncake {

struct nof_seg_handle {
    struct spdk_nvme_qpair *qpair;
    struct spdk_nvme_ns *ns;
};

struct tr_info {
    struct spdk_nvme_transport_id trid;
    std::string ctrlr_key;
    uint32_t ns;
};

struct ctrlr_info {
    struct spdk_nvme_ctrlr *ctrlr;
    std::map<uint32_t, nof_seg_handle*> ns_seg;
    std::mutex ns_mutex;
};

SpdkWrapper::SpdkWrapper() {
    initialized = false;
}

SpdkWrapper::~SpdkWrapper() {
    Cleanup();
}

SpdkWrapper& SpdkWrapper::GetInstance() {
    static SpdkWrapper ins;
    return ins;
}

bool SpdkWrapper::InitializeEnv() {
    if (initialized) {
        return true;
    }

    std::lock_guard<std::mutex> lock(init_mutex);
    if (initialized) {
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

    // 设置初始化标志
    initialized = true;
    return true;
}

void SpdkWrapper::Cleanup() {
    if (initialized) {
        spdk_env_fini();
        initialized = false;
    }
}

void* SpdkWrapper::Alloc(size_t size, size_t align, int socket_id) {
    if (!InitializeEnv()) {
        return nullptr;
    }

    // 分配内存
    void *buf = spdk_zmalloc(size, align, nullptr, socket_id, SPDK_MALLOC_DMA);
    fprintf(stderr, "spdk zmalloc: addr %p, size %lu, align %lu\n", buf, size, align);
    return buf;
}

void SpdkWrapper::Free(void *ptr) {
    if (ptr) {
        spdk_free(ptr);
    }
}

void* SpdkWrapper::NvmePollGroupCreate() {
    return spdk_nvme_poll_group_create(NULL, NULL);
}

int SpdkWrapper::NvmePollGroupDestroy(void *group) {
    return spdk_nvme_poll_group_destroy(reinterpret_cast<spdk_nvme_poll_group *>(group));
}

int SpdkWrapper::NvmePollGroupAdd(void *group, nof_seg_handle *seg) {

    return spdk_nvme_poll_group_add(reinterpret_cast<spdk_nvme_poll_group *>(group), seg->qpair);
}

int SpdkWrapper::NvmePollGroupRemove(void *group, nof_seg_handle *seg) {
    return spdk_nvme_poll_group_remove(reinterpret_cast<spdk_nvme_poll_group *>(group), seg->qpair);
}

int64_t SpdkWrapper::NvmePollGroupProcessCompletion(void *group, uint32_t complete_per_seg) {
    return spdk_nvme_poll_group_process_completions(reinterpret_cast<spdk_nvme_poll_group *>(group),
        complete_per_seg, disconnect_cb);
}

int SpdkWrapper::ParseTransPortStr(const std::string &tr_str, tr_info *info) {
    size_t pos = 0;
    size_t start = 0;

    while (pos < tr_str.size()) {
        size_t space_pos = tr_str.find(' ', pos);
        if (space_pos == std::string::npos) {
            space_pos = tr_str.length();
        }

        std::string token = tr_str,substr(pos, space_pos - pos);
        size_t colon_pos = token.find(":");
        if (colon_pos != std::string::npos) {
            std::string key = token.substr(0, colon_pos);
            std::string val = token.substr(colon_pos + 1);

            if (key == "traddr") {
                if (val.size() > SPDK_NVMF_TRADDR_MAX_LEN) {
                    LOG(ERROR) << "traddr length " << val.size() << " greater than maximum allowed " << SPDK_NVMF_TRADDR_MAX_LEN;
                    return -1;
                }
                strncpy(info->trid.traddr, val.c_str(), val.size());
                info->trid.traddr[val.size()] = '\0';
                info->ctrlr_key += val;
            } else if (key == "trsvcid") {
                if (val.size() > SPDK_NVMF_TRSVCID_MAX_LEN) {
                    LOG(ERROR) << "trsvcid length " << val.size() << " greater than maximum allowed " << SPDK_NVMF_TRSVCID_MAX_LEN;
                    return -1;
                }
                strncpy(info->trid.trsvcid, val.c_str(), val.size());
                info->trid.trsvcid[val.size()] = '\0';
            } else if (key == "subnqn") {
                if (val.size() > SPDK_NVMF_NQN_MAX_LEN) {
                    LOG(ERROR) << "subnqn length " << val.size() << " greater than maximum allowed " << SPDK_NVMF_NQN_MAX_LEN;
                    return -1;
                }
                strncpy(info->trid.subnqn, val.c_str(), val.size());
                info->trid.subnqn[val.size()] = '\0';
                info->ctrlr_key += val;
            } else if (key == "ns") {
                try {
                    info->ns = static_cast<uint32_t>(std::stoul(val));
                } catch (const std::exception& e) {
                    LOG(ERROR) << "Failed to parse ns " << e.what();
                    return -1;
                }
            }
        }

        pos = space_pos + 1;
    }

    info->trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
    info->trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    return 0;
}

int SpdkWrapper::ConnectController(const struct spdk_nvme_transport_id *trid, ctrlr_info *info) {
    auto probe_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	    struct spdk_nvme_ctrlr_opts *opts) -> bool {
        LOG(INFO) << "Attaching to " << trid->traddr << " " << trid->subnqn;
	    return true;
    };

    auto attach_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	    struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
        LOG(INFO) << "Attached to " << trid->traddr << " " << trid->subnqn;
        ctrlr_info *info = (ctrlr_info *)cb_ctx;
        info->ctrlr = ctrlr;
    };

    return spdk_nvme_probe(trid, (void*)info, probe_cb, attach_cb, NULL);
}

nof_seg_handle *SpdkWrapper::OpenNofSegment(const std::string &tr_str) {
    tr_info tr;
    int ret = ParseTransPortStr(tr_str, &tr);
    if (ret != 0) {
        return nullptr;
    }

    ctrlr_info *info = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctrlrs_mutex);
        if (connected_ctrlrs.find(tr.ctrlr_key) == connected_ctrlrs.end()) {
            info = new ctrlr_info;
            if (!info) {
                LOG(ERROR) << "alloc ctrlr_info failed";
                return nullptr;
            }

            ret = ConnectController(&tr.trid, info);
            if (ret != 0) {
                delete info;
                return nullptr;
            }

            connected_ctrlrs[tr.ctrlr_key] = info;
        } else {
            info = connected_ctrlrs[tr.ctrlr_key];
        }
    }

    nof_seg_handle *seg_handle = nullptr;
    struct spdk_nvme_qpair *qpair = nullptr;
    struct spdk_nvme_ns *ns = nullptr;
    {
        auto &ns_seg = info->ns_seg;
        std::lock_guard<std::mutex> lock(info->ns_mutex);
        if (ns_seg.find(tr.ns) != ns_seg.end()) {
            return ns_seg[tr.ns];
        }
    
        if (spdk_nvme_ctrlr_is_active_ns(info->ctrlr, tr.ns)) {
            ns = spdk_nvme_ctrlr_get_ns(info->ctrlr, tr.ns);
        } else {
            return nullptr;
        }

        qpair = spdk_nvme_ctrlr_alloc_io_qpair(info->ctrlr, nullptr, 0);
        if (!qpair) {
            LOG(ERROR) << "alloc spdk_nvme_qpair failed";
            return nullptr;
        }

        seg_handle = new nof_seg_handle;
        if (!seg_handle) {
            spdk_nvme_ctrlr_free_io_qpair(qpair);
            LOG(ERROR) << "alloc nof_seg_handle failed";
            return nullptr;
        }

        ns_seg[tr.ns] = seg_handle;
    }

    seg_handle->qpair = qpair;
    seg_handle->ns = ns;
    return seg_handle;
}

uint32_t SpdkWrapper::GetBlockSize(const nof_seg_handle *seg_handle)
{
    if (unlikely(!seg_handle || !seg_handle->ns)) {
        return INVALID_BLOCK_SIZE;
    }

    return spdk_nvme_ns_get_sector_size(seg_handle->ns);
}

int SpdkWrapper::SubmitRequest(const nof_seg_handle *seg_handle, void *ptr, uint64_t lba, uint32_t lba_count, int op, 
        spdk_nvme_cmd_cb cb_fn, void *cb_ctx) {
    if (unlikely(!seg_handle || !ptr || !lba_count || !seg_handle->qpair || !seg_handle->ns)) {
        return -1;
    }

    struct spdk_nvme_qpair *qpair = seg_handle->qpair;
    struct spdk_nvme_ns *ns = seg_handle->ns;
    if (op == 0) {
        return spdk_nvme_ns_cmd_read(ns, qpair, ptr, lba, lba_count, cb_fn, cb_ctx, 0);
    } else {
        return spdk_nvme_ns_cmd_write(ns, qpair, ptr, lba, lba_count, cb_fn, cb_ctx, 0);
    }
}

}