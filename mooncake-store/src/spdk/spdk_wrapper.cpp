#include <glog/logging.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <set>
#include <thread>
#include "spdk/spdk_wrapper.h"

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
              << ", io_queue_requests="
              << opts->io_queue_requests
              << ", keep_alive_timeout_ms=" << opts->keep_alive_timeout_ms
              << ", transport_ack_timeout="
              << static_cast<int>(opts->transport_ack_timeout)
              << ", admin_queue_size=" << opts->admin_queue_size
              << ", fabrics_connect_timeout_us="
              << opts->fabrics_connect_timeout_us
              << ", header_digest=" << opts->header_digest
              << ", data_digest=" << opts->data_digest;
}

const char *AsyncEventTypeToString(uint32_t type) {
    switch (type) {
        case SPDK_NVME_ASYNC_EVENT_TYPE_ERROR:
            return "error";
        case SPDK_NVME_ASYNC_EVENT_TYPE_SMART:
            return "smart_health";
        case SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE:
            return "notice";
        case SPDK_NVME_ASYNC_EVENT_TYPE_IO:
            return "io";
        case SPDK_NVME_ASYNC_EVENT_TYPE_VENDOR:
            return "vendor";
        default:
            return "unknown";
    }
}

const char *NoticeEventInfoToString(uint32_t info) {
    switch (info) {
        case SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED:
            return "namespace_attribute_changed";
        case SPDK_NVME_ASYNC_EVENT_FW_ACTIVATION_START:
            return "firmware_activation_start";
        case SPDK_NVME_ASYNC_EVENT_TELEMETRY_LOG_CHANGED:
            return "telemetry_log_changed";
        case SPDK_NVME_ASYNC_EVENT_ANA_CHANGE:
            return "ana_change";
        case SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE:
            return "discovery_log_change";
        default:
            return "notice_unknown";
    }
}

const char *SmartEventInfoToString(uint32_t info) {
    switch (info) {
        case SPDK_NVME_ASYNC_EVENT_SUBSYSTEM_RELIABILITY:
            return "subsystem_reliability";
        case SPDK_NVME_ASYNC_EVENT_TEMPERATURE_THRESHOLD:
            return "temperature_threshold";
        case SPDK_NVME_ASYNC_EVENT_SPARE_BELOW_THRESHOLD:
            return "spare_below_threshold";
        default:
            return "smart_unknown";
    }
}

const char *AsyncEventInfoToString(uint32_t type, uint32_t info) {
    if (type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) {
        return NoticeEventInfoToString(info);
    }
    if (type == SPDK_NVME_ASYNC_EVENT_TYPE_SMART) {
        return SmartEventInfoToString(info);
    }
    return "unknown";
}

void LogActiveNamespaces(struct spdk_nvme_ctrlr *ctrlr) {
    std::string active_ns;
    for (uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
         nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        if (!active_ns.empty()) {
            active_ns += ",";
        }
        active_ns += std::to_string(nsid);
    }

    const struct spdk_nvme_transport_id *trid =
        spdk_nvme_ctrlr_get_transport_id(ctrlr);
    LOG(INFO) << "spdk_nof_admin_active_namespaces traddr="
              << (trid ? trid->traddr : "") << " subnqn="
              << (trid ? trid->subnqn : "") << " active_ns=["
              << active_ns << "]";
}

std::string BuildControllerKey(const struct spdk_nvme_transport_id *trid) {
    if (!trid) {
        return "";
    }
    return std::string(trid->traddr) + "|" + std::string(trid->trsvcid) +
           "|" + std::string(trid->subnqn) + "|" +
           std::to_string(static_cast<int>(trid->trtype));
}

void NofAerCallback(void *cb_arg, const struct spdk_nvme_cpl *cpl) {
    auto *ctrlr = reinterpret_cast<struct spdk_nvme_ctrlr *>(cb_arg);
    const struct spdk_nvme_transport_id *trid =
        ctrlr ? spdk_nvme_ctrlr_get_transport_id(ctrlr) : nullptr;

    if (spdk_nvme_cpl_is_error(cpl)) {
        LOG(WARNING) << "spdk_nof_admin_aer_error traddr="
                     << (trid ? trid->traddr : "") << " subnqn="
                     << (trid ? trid->subnqn : "") << " status="
                     << spdk_nvme_cpl_get_status_string(&cpl->status);
        return;
    }

    union spdk_nvme_async_event_completion event;
    event.raw = cpl->cdw0;

    LOG(INFO) << "spdk_nof_admin_aer traddr=" << (trid ? trid->traddr : "")
              << " subnqn=" << (trid ? trid->subnqn : "")
              << " type=" << AsyncEventTypeToString(event.bits.async_event_type)
              << " info="
              << AsyncEventInfoToString(event.bits.async_event_type,
                                        event.bits.async_event_info)
              << " raw_type=" << event.bits.async_event_type
              << " raw_info=" << event.bits.async_event_info
              << " log_page=" << event.bits.log_page_identifier;

    if (ctrlr && event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE &&
        event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED) {
        SpdkWrapper::GetInstance().RecordNamespaceAttributeChanged(ctrlr);
        LogActiveNamespaces(ctrlr);
    }
}

uint64_t GetAdminPollIntervalMs() {
    static const uint64_t interval_ms = [] {
        uint64_t value = 100;
        ParseEnvU64("MC_SPDK_NOF_ADMIN_POLL_INTERVAL_MS", &value);
        return value;
    }();
    return interval_ms;
}

}  // namespace

struct nof_seg_handle {
    struct spdk_nvme_qpair *qpair{nullptr};
    struct spdk_nvme_ns *ns{nullptr};
};

struct tr_info {
    struct spdk_nvme_transport_id trid;
    std::string ctrlr_key;
    uint32_t ns;
};

struct ctrlr_info {
    struct spdk_nvme_ctrlr *ctrlr{nullptr};
    std::map<uint32_t, nof_seg_handle*> ns_seg;
    std::vector<struct spdk_nvme_qpair *> retired_qpairs;
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
        {
            std::lock_guard<std::mutex> lock(ctrlrs_mutex);
            for (auto &[_, info] : connected_ctrlrs) {
                if (!info) {
                    continue;
                }
                {
                    std::lock_guard<std::mutex> ns_lock(info->ns_mutex);
                    for (auto &[_, seg] : info->ns_seg) {
                        if (!seg) {
                            continue;
                        }
                        if (seg->qpair) {
                            spdk_nvme_ctrlr_free_io_qpair(seg->qpair);
                            seg->qpair = nullptr;
                        }
                        seg->ns = nullptr;
                    }
                    info->ns_seg.clear();
                }
                if (info->ctrlr) {
                    spdk_nvme_detach(info->ctrlr);
                    info->ctrlr = nullptr;
                }
                delete info;
            }
            connected_ctrlrs.clear();
            for (auto &[_, stale_infos] : stale_ctrlrs_) {
                for (auto *info : stale_infos) {
                    if (!info) {
                        continue;
                    }
                    for (auto *qpair : info->retired_qpairs) {
                        if (qpair) {
                            spdk_nvme_ctrlr_free_io_qpair(qpair);
                        }
                    }
                    info->retired_qpairs.clear();
                    if (info->ctrlr) {
                        spdk_nvme_detach(info->ctrlr);
                        info->ctrlr = nullptr;
                    }
                    delete info;
                }
            }
            stale_ctrlrs_.clear();
        }
        {
            std::lock_guard<std::mutex> admin_lock(admin_poll_mutex_);
            last_admin_poll_.clear();
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

int64_t SpdkWrapper::NvmePollProcessCompletion(nof_seg_handle *seg, uint32_t complete_per_seg) {
    if (!seg || !seg->qpair) {
        return -ENXIO;
    }
    return spdk_nvme_qpair_process_completions(seg->qpair, complete_per_seg);
}

bool SpdkWrapper::IsConnectionError(int32_t ret) const {
    if (ret >= 0) {
        return false;
    }

    int err = -ret;
    return err == ENXIO || err == EIO || err == ECONNRESET ||
           err == ENODEV || err == EPIPE || err == ETIMEDOUT;
}

std::string SpdkWrapper::GetControllerKey(const std::string &tr_str) {
    tr_info tr;
    if (ParseTransPortStr(tr_str, &tr) != 0) {
        return "";
    }
    return tr.ctrlr_key;
}

void SpdkWrapper::RecordNamespaceAttributeChanged(
    struct spdk_nvme_ctrlr *ctrlr) {
    const struct spdk_nvme_transport_id *trid =
        ctrlr ? spdk_nvme_ctrlr_get_transport_id(ctrlr) : nullptr;
    std::string ctrlr_key = BuildControllerKey(trid);
    if (ctrlr_key.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(namespace_events_mutex_);
    namespace_changed_ctrlrs_.insert(ctrlr_key);
}

std::vector<std::string>
SpdkWrapper::ConsumeNamespaceAttributeChangedControllers() {
    std::lock_guard<std::mutex> lock(namespace_events_mutex_);
    std::vector<std::string> changed_ctrlrs(namespace_changed_ctrlrs_.begin(),
                                            namespace_changed_ctrlrs_.end());
    namespace_changed_ctrlrs_.clear();
    return changed_ctrlrs;
}

std::vector<uint32_t> SpdkWrapper::GetActiveNamespaces(
    const std::string &tr_str) {
    tr_info tr;
    if (ParseTransPortStr(tr_str, &tr) != 0) {
        return {};
    }

    std::lock_guard<std::mutex> lock(ctrlrs_mutex);
    auto it = connected_ctrlrs.find(tr.ctrlr_key);
    if (it == connected_ctrlrs.end() || !it->second || !it->second->ctrlr ||
        spdk_nvme_ctrlr_is_failed(it->second->ctrlr)) {
        return {};
    }

    std::vector<uint32_t> active_namespaces;
    for (uint32_t nsid =
             spdk_nvme_ctrlr_get_first_active_ns(it->second->ctrlr);
         nsid != 0;
         nsid = spdk_nvme_ctrlr_get_next_active_ns(it->second->ctrlr, nsid)) {
        active_namespaces.push_back(nsid);
    }
    return active_namespaces;
}

int32_t SpdkWrapper::NvmePollAdminCompletions(nof_seg_handle *seg) {
    if (!seg || !seg->ns) {
        return -EINVAL;
    }

    struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(seg->ns);
    if (!ctrlr) {
        return -ENXIO;
    }

    auto now = std::chrono::steady_clock::now();
    auto interval = std::chrono::milliseconds(GetAdminPollIntervalMs());

    std::lock_guard<std::mutex> lock(admin_poll_mutex_);
    auto it = last_admin_poll_.find(ctrlr);
    if (it != last_admin_poll_.end() && now - it->second < interval) {
        return 0;
    }

    last_admin_poll_[ctrlr] = now;
    int32_t ret = spdk_nvme_ctrlr_process_admin_completions(ctrlr);
    if (ret > 0) {
        const struct spdk_nvme_transport_id *trid =
            spdk_nvme_ctrlr_get_transport_id(ctrlr);
        LOG(INFO) << "spdk_nof_admin_poll_completions traddr="
                  << (trid ? trid->traddr : "") << " subnqn="
                  << (trid ? trid->subnqn : "") << " completions=" << ret;
    }
    return ret;
}

void SpdkWrapper::DropNofControllerLocked(const std::string &ctrlr_key,
                                          bool detach) {
    auto it = connected_ctrlrs.find(ctrlr_key);
    if (it == connected_ctrlrs.end()) {
        return;
    }

    ctrlr_info *info = it->second;
    if (!info) {
        connected_ctrlrs.erase(it);
        return;
    }

    struct spdk_nvme_ctrlr *ctrlr = info->ctrlr;
    {
        std::lock_guard<std::mutex> ns_lock(info->ns_mutex);
        for (auto &[nsid, seg] : info->ns_seg) {
            if (!seg) {
                continue;
            }
            if (detach && seg->qpair) {
                spdk_nvme_ctrlr_free_io_qpair(seg->qpair);
            } else if (!detach && seg->qpair) {
                info->retired_qpairs.push_back(seg->qpair);
            }
            seg->qpair = nullptr;
            seg->ns = nullptr;
            LOG(WARNING) << "invalidate nof segment handle"
                         << ", ctrlr_key=" << ctrlr_key
                         << ", nsid=" << nsid;
        }
        info->ns_seg.clear();
    }

    {
        std::lock_guard<std::mutex> admin_lock(admin_poll_mutex_);
        last_admin_poll_.erase(ctrlr);
    }

    if (detach && ctrlr) {
        for (auto *qpair : info->retired_qpairs) {
            if (qpair) {
                spdk_nvme_ctrlr_free_io_qpair(qpair);
            }
        }
        info->retired_qpairs.clear();
        spdk_nvme_detach(ctrlr);
        info->ctrlr = nullptr;
        delete info;
    } else if (!detach) {
        stale_ctrlrs_[ctrlr_key].push_back(info);
    }

    connected_ctrlrs.erase(it);
    LOG(WARNING) << "invalidate nof controller"
                 << ", ctrlr_key=" << ctrlr_key
                 << ", action="
                 << (detach ? "detach_after_open_failure"
                            : "drop_cache_without_detach");
}

void SpdkWrapper::CleanupStaleNofControllersLocked(
    const std::string &ctrlr_key) {
    auto it = stale_ctrlrs_.find(ctrlr_key);
    if (it == stale_ctrlrs_.end()) {
        return;
    }

    std::vector<ctrlr_info *> stale_infos;
    stale_infos.swap(it->second);
    stale_ctrlrs_.erase(it);

    for (auto *info : stale_infos) {
        if (!info) {
            continue;
        }
        for (auto *qpair : info->retired_qpairs) {
            if (qpair) {
                spdk_nvme_ctrlr_free_io_qpair(qpair);
            }
        }
        info->retired_qpairs.clear();

        if (info->ctrlr) {
            spdk_nvme_detach(info->ctrlr);
            info->ctrlr = nullptr;
        }
        delete info;
    }

    LOG(WARNING) << "cleanup stale nof controllers"
                 << ", ctrlr_key=" << ctrlr_key
                 << ", count=" << stale_infos.size();
}

void SpdkWrapper::InvalidateNofController(const std::string &tr_str) {
    tr_info tr;
    int ret = ParseTransPortStr(tr_str, &tr);
    if (ret != 0) {
        LOG(ERROR) << "invalidate nof controller: parse transport failed"
                   << ", endpoint=" << tr_str;
        return;
    }

    std::lock_guard<std::mutex> lock(ctrlrs_mutex);
    DropNofControllerLocked(tr.ctrlr_key, false);
}

void SpdkWrapper::InvalidateNofController(nof_seg_handle *seg) {
    if (!seg || !seg->ns) {
        return;
    }

    struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(seg->ns);
    if (!ctrlr) {
        seg->qpair = nullptr;
        seg->ns = nullptr;
        return;
    }

    std::lock_guard<std::mutex> lock(ctrlrs_mutex);
    for (auto it = connected_ctrlrs.begin(); it != connected_ctrlrs.end(); ++it) {
        ctrlr_info *info = it->second;
        if (!info || info->ctrlr != ctrlr) {
            continue;
        }

        DropNofControllerLocked(it->first, false);
        return;
    }

    seg->qpair = nullptr;
    seg->ns = nullptr;
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
           LOG(ERROR) << "Failed to parse ns value: " << ns_str << ", error: " << e.what();
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
              << "trsvcid:" << info->trid.trsvcid
              << "ns:" << info->ns
              << "subnqn:" << info->trid.subnqn
              << "trtype:" << info->trid.trtype;
    
    return 0;
}

int SpdkWrapper::ConnectController(const struct spdk_nvme_transport_id *trid, ctrlr_info *info) {
    auto probe_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	    struct spdk_nvme_ctrlr_opts *opts) -> bool {
        ApplyCtrlrOptsFromEnv(opts);
        LOG(INFO) << "Attaching to " << trid->traddr << " " << trid->subnqn;
	    return true;
    };
    auto attach_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	    struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
        LOG(INFO) << "Attached to " << trid->traddr << " " << trid->subnqn;
        ctrlr_info *info = (ctrlr_info *)cb_ctx;
        info->ctrlr = ctrlr;
        spdk_nvme_ctrlr_register_aer_callback(ctrlr, NofAerCallback, ctrlr);
    };

    return spdk_nvme_probe(trid, (void*)info, probe_cb, attach_cb, NULL);
}

nof_seg_handle *SpdkWrapper::OpenNofSegment(const std::string &tr_str) {
    tr_info tr;
    int ret = ParseTransPortStr(tr_str, &tr);
    if (ret != 0) {
        return nullptr;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        ctrlr_info *info = nullptr;
        {
            std::lock_guard<std::mutex> lock(ctrlrs_mutex);
            CleanupStaleNofControllersLocked(tr.ctrlr_key);
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

            if (!info || !info->ctrlr ||
                spdk_nvme_ctrlr_is_failed(info->ctrlr)) {
                LOG(ERROR) << "nof controller is failed before opening segment"
                           << ", ctrlr_key=" << tr.ctrlr_key
                           << ", attempt=" << attempt;
                DropNofControllerLocked(tr.ctrlr_key, true);
                continue;
            }
        }

        nof_seg_handle *seg_handle = nullptr;
        struct spdk_nvme_qpair *qpair = nullptr;
        struct spdk_nvme_ns *ns = nullptr;
        bool should_retry = false;
        {
            auto &ns_seg = info->ns_seg;
            std::lock_guard<std::mutex> lock(info->ns_mutex);
            if (ns_seg.find(tr.ns) != ns_seg.end()) {
                return ns_seg[tr.ns];
            }

            if (spdk_nvme_ctrlr_is_active_ns(info->ctrlr, tr.ns)) {
                ns = spdk_nvme_ctrlr_get_ns(info->ctrlr, tr.ns);
            } else {
                LOG(ERROR) << "spdk_nvme_ctrlr_is_active_ns failed";
                return nullptr;
            }

            qpair = spdk_nvme_ctrlr_alloc_io_qpair(info->ctrlr, nullptr, 0);
            if (!qpair) {
                LOG(ERROR) << "alloc spdk_nvme_qpair failed"
                           << ", ctrlr_key=" << tr.ctrlr_key
                           << ", attempt=" << attempt;
                should_retry = true;
            } else {
                seg_handle = new nof_seg_handle;
                if (!seg_handle) {
                    spdk_nvme_ctrlr_free_io_qpair(qpair);
                    LOG(ERROR) << "alloc nof_seg_handle failed";
                    return nullptr;
                }

                seg_handle->qpair = qpair;
                seg_handle->ns = ns;
                ns_seg[tr.ns] = seg_handle;
                return seg_handle;
            }
        }

        if (should_retry) {
            std::lock_guard<std::mutex> lock(ctrlrs_mutex);
            DropNofControllerLocked(tr.ctrlr_key, true);
        }
    }

    return nullptr;
}

void SpdkWrapper::CloseNofSegment(const std::string &tr_str) {
    tr_info tr;
    if (ParseTransPortStr(tr_str, &tr) != 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(ctrlrs_mutex);
    auto ctrlr_it = connected_ctrlrs.find(tr.ctrlr_key);
    if (ctrlr_it == connected_ctrlrs.end() || !ctrlr_it->second) {
        return;
    }

    ctrlr_info *info = ctrlr_it->second;
    std::lock_guard<std::mutex> ns_lock(info->ns_mutex);
    auto seg_it = info->ns_seg.find(tr.ns);
    if (seg_it == info->ns_seg.end()) {
        return;
    }

    nof_seg_handle *seg = seg_it->second;
    if (seg) {
        if (seg->qpair) {
            spdk_nvme_ctrlr_free_io_qpair(seg->qpair);
            seg->qpair = nullptr;
        }
        seg->ns = nullptr;
        delete seg;
    }
    info->ns_seg.erase(seg_it);
    LOG(INFO) << "close nof segment handle"
              << ", ctrlr_key=" << tr.ctrlr_key
              << ", nsid=" << tr.ns;
}

uint32_t SpdkWrapper::GetBlockSize(const nof_seg_handle *seg_handle)
{
    if (!seg_handle || !seg_handle->ns) {
        return INVALID_BLOCK_SIZE;
    }

    return spdk_nvme_ns_get_sector_size(seg_handle->ns);
}

uint64_t SpdkWrapper::GetNamespaceCapacityBytes(
    const nof_seg_handle *seg_handle) {
    if (!seg_handle || !seg_handle->ns) {
        return 0;
    }
    return spdk_nvme_ns_get_size(seg_handle->ns);
}

int SpdkWrapper::SubmitRequest(const nof_seg_handle *seg_handle, void *ptr, uint64_t lba, uint32_t lba_count, int op, 
        spdk_nvme_cmd_cb cb_fn, void *cb_ctx) {
    if (!seg_handle || !ptr || !lba_count || !seg_handle->qpair || !seg_handle->ns) {
        return -ENXIO;
    }

    struct spdk_nvme_qpair *qpair = seg_handle->qpair;
    struct spdk_nvme_ns *ns = seg_handle->ns;
    if (op == 0) {
        return spdk_nvme_ns_cmd_read(ns, qpair, ptr, lba, lba_count, cb_fn, cb_ctx, 0);
    } else {
        return spdk_nvme_ns_cmd_write(ns, qpair, ptr, lba, lba_count, cb_fn, cb_ctx, 0);
    }
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

    probe_buffer->ptr = spdk_zmalloc(block_size, 0x1000, nullptr, -1,
                                     SPDK_MALLOC_DMA);
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
    int ret = SubmitRequest(seg_handle, probe_buffer->ptr, 0, 1, 0,
                            ProbeReadComplete, probe_ctx);
    if (ret != 0) {
        RecycleProbeRequestContext(probe_ctx);
        if (IsConnectionError(ret)) {
            InvalidateNofController(seg_handle);
        }
        if (error_reason) {
            *error_reason = IsConnectionError(ret)
                                ? "controller_error:" + std::to_string(ret)
                                : "submit_fail";
        }
        return false;
    }

    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!probe_ctx->done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        int32_t admin_ret = NvmePollAdminCompletions(seg_handle);
        if (admin_ret < 0) {
            LOG(ERROR) << "poll admin completion error during probe: ret "
                       << admin_ret;
            if (IsConnectionError(admin_ret)) {
                InvalidateNofController(seg_handle);
                if (error_reason) {
                    *error_reason =
                        "controller_error:" + std::to_string(admin_ret);
                }
                return false;
            }
        }
        int64_t poll_ret = NvmePollProcessCompletion(seg_handle, 0);
        if (poll_ret < 0 && IsConnectionError(static_cast<int32_t>(poll_ret))) {
            InvalidateNofController(seg_handle);
            if (error_reason) {
                *error_reason =
                    "controller_error:" + std::to_string(poll_ret);
            }
            return false;
        }
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
