#define _GNU_SOURCE

#include "nof/spdk_initiator.h"

#include <glog/logging.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include <thread>
#include <utility>
#include <vector>

#include <spdk/dma.h>
#include <spdk/env.h>
#include <list>
#include <limits>
#include <cstdio>
#include <infiniband/verbs.h>
#if defined(USE_ASCEND_NDR) || defined(USE_ASCEND_STAGING)
#include "transport/rdma_transport/ascend_storage_config.h"
#endif
#ifdef USE_SPDK_NPU_UB
#include "transport/rdma_transport/ascend_parameter_plane.h"
#endif
#include <spdk/nvme.h>

#include "config/nof_debug_config.h"
#include "config/spdk_controller_config.h"
#include "environ.h"
#include "nof/page_registry.h"
#include "nof/probe_context_pool.h"
#include "nof/padding_buffer_pool.h"
#include "nof/namespace_refresh.h"

namespace mooncake {
namespace {

bool IsControllerConnectionError(int64_t ret) {
    return ret == -ENXIO || ret == -EIO || ret == -ECONNRESET ||
           ret == -ENODEV || ret == -EPIPE || ret == -ETIMEDOUT;
}

#ifdef USE_SPDK_NPU_UB
ibv_pd* GetRdmaPd(spdk_memory_domain* domain) {
    if (!domain || spdk_memory_domain_get_dma_device_type(domain) !=
                       SPDK_DMA_DEVICE_TYPE_RDMA)
        return nullptr;
    auto* ctx = spdk_memory_domain_get_context(domain);
    if (!ctx || ctx->size < sizeof(*ctx) || !ctx->user_ctx) return nullptr;
    auto* rdma = static_cast<spdk_memory_domain_rdma_ctx*>(ctx->user_ctx);
    return rdma->size >= sizeof(*rdma) ? static_cast<ibv_pd*>(rdma->ibv_pd)
                                       : nullptr;
}
std::shared_ptr<ibv_pd> RetainSpdkPd(ibv_pd* pd) {
    struct Owners {
        std::mutex mutex;
        std::map<ibv_pd*, size_t> counts;
    };
    static auto* owners = new Owners;
    auto* registry = owners;
    std::lock_guard<std::mutex> lock(registry->mutex);
    ++registry->counts[pd];
    return std::shared_ptr<ibv_pd>(pd, [registry](ibv_pd* value) {
        std::lock_guard<std::mutex> lock(registry->mutex);
        auto it = registry->counts.find(value);
        if (--it->second == 0) {
            AscendParameterPlane::GetInstance().InvalidatePd(value);
            registry->counts.erase(it);
        }
    });
}
#endif

struct VectorIo : NofSglCursor {
    NofIOAdaptor adaptor{};
    spdk_nvme_ns_cmd_ext_io_opts opts{};
    spdk_nvme_cpl completion{};
    bool completed{false};
    bool reads_into_hbm{false};
};
void ResetVector(void* arg, uint32_t offset) {
    static_cast<VectorIo*>(arg)->Reset(offset);
}
int NextVector(void* arg, void** address, uint32_t* length) {
    return static_cast<VectorIo*>(arg)->Next(address, length);
}
void VectorComplete(void* arg, const spdk_nvme_cpl* cpl) {
    auto& io = *static_cast<VectorIo*>(arg);
    io.completion = *cpl;
    io.completed = true;
}

// Newer SPDK releases require opts_size; older headers do not have it.
template <typename T>
void SetEnvOptsSize(T& opts) {
    if constexpr (requires { opts.opts_size; }) opts.opts_size = sizeof(opts);
}

#ifdef USE_SPDK_NPU_UB
template <typename T>
int ConfigureNpuWriteCapsule(T& opts) {
    static const bool disable_in_capsule = Environ::Process().GetTypedOr(
        EnvironmentVariable<bool>{"MC_NOF_DISABLE_HBM_IN_CAPSULE"}, false);
    if (!disable_in_capsule) return 0;

    if constexpr (requires { opts.rdma_disable_in_capsule_data; }) {
        opts.rdma_disable_in_capsule_data = true;
        LOG_FIRST_N(INFO, 1)
            << "MC_NOF_DISABLE_HBM_IN_CAPSULE enabled: disabling in-capsule "
               "data for NVMe/RDMA writes from HBM";
        return 0;
    } else {
        LOG_FIRST_N(ERROR, 1)
            << "MC_NOF_DISABLE_HBM_IN_CAPSULE requires SPDK support for "
               "rdma_disable_in_capsule_data";
        return -ENOTSUP;
    }
}
#endif

void ApplyCtrlrOptsFromEnv(struct spdk_nvme_ctrlr_opts* opts) {
    opts->keep_alive_timeout_ms = 0;
    const auto config = SpdkControllerConfig::FromEnvironment();
    if (config.num_io_queues.has_value()) {
        opts->num_io_queues = *config.num_io_queues;
    }
    if (config.io_queue_size.has_value()) {
        opts->io_queue_size = *config.io_queue_size;
    }
    if (config.io_queue_requests.has_value()) {
        opts->io_queue_requests = *config.io_queue_requests;
    }
    if (config.transport_ack_timeout.has_value()) {
        opts->transport_ack_timeout = *config.transport_ack_timeout;
    }
    if (config.admin_queue_size.has_value()) {
        opts->admin_queue_size = *config.admin_queue_size;
    }
    if (config.fabrics_connect_timeout_us.has_value()) {
        opts->fabrics_connect_timeout_us = *config.fabrics_connect_timeout_us;
    }
    if (config.header_digest.has_value()) {
        opts->header_digest = *config.header_digest;
    }
    if (config.data_digest.has_value()) {
        opts->data_digest = *config.data_digest;
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

// spdk_nvme_probe/spdk_nvme_detach mutate the NVMe driver's process-global
// controller state and are not thread-safe against each other. The
// pre-refactor SpdkWrapper was a process singleton whose ctrlrs_mutex
// serialized them; with per-instance initiators a process-wide lock is
// needed instead. Held only around the SPDK calls; the lock order is
// ctrlrs_mutex_ < NvmeCtrlrStateMutex (the destructor takes this lock while
// holding ctrlrs_mutex_, and no path takes ctrlrs_mutex_ while holding this
// one), so a slow probe still cannot stall cache lookups. Leaked to avoid
// any static destruction-order hazard.
std::mutex& NvmeCtrlrStateMutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

// SPDK can return the same controller to multiple initiator instances.
// Each controller has one asynchronous event request callback slot, so fan out
// notices to every live instance.
struct NamespaceNotices {
    std::mutex mutex;
    std::map<spdk_nvme_ctrlr*, std::set<std::atomic<bool>*>> subscribers;
};
NamespaceNotices& Notices() {
    static auto* notices = new NamespaceNotices();
    return *notices;
}
void SubscribeNamespaceNotices(spdk_nvme_ctrlr* ctrlr,
                               std::atomic<bool>* changed) {
    auto& notices = Notices();
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(notices.mutex);
        auto& subscribers = notices.subscribers[ctrlr];
        first = subscribers.empty();
        subscribers.insert(changed);
    }
    if (!first) return;
    spdk_nvme_ctrlr_register_aer_callback(
        ctrlr,
        [](void* ctx, const spdk_nvme_cpl* cpl) {
            if (spdk_nvme_cpl_is_error(cpl)) return;
            union spdk_nvme_async_event_completion event;
            event.raw = cpl->cdw0;
            if (event.bits.async_event_type !=
                    SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE ||
                event.bits.async_event_info !=
                    SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED)
                return;
            auto& notices = Notices();
            std::lock_guard<std::mutex> lock(notices.mutex);
            auto it =
                notices.subscribers.find(static_cast<spdk_nvme_ctrlr*>(ctx));
            if (it != notices.subscribers.end()) {
                for (auto* changed : it->second) changed->store(true);
            }
        },
        ctrlr);
}
void UnsubscribeNamespaceNotices(spdk_nvme_ctrlr* ctrlr,
                                 std::atomic<bool>* changed) {
    auto& notices = Notices();
    std::lock_guard<std::mutex> lock(notices.mutex);
    auto it = notices.subscribers.find(ctrlr);
    if (it == notices.subscribers.end()) return;
    it->second.erase(changed);
    if (it->second.empty()) notices.subscribers.erase(it);
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared, refcounted SPDK environment guard.
//
// DPDK EAL init is process-global. The env is acquired lazily on first use
// and, once initialized, pinned until process exit: SPDK v23.01.1 rejects a
// non-NULL-opts spdk_env_init after spdk_env_fini with -EINVAL, so a
// mid-process fini would break every later Acquire() in this process.
// ---------------------------------------------------------------------------

class SpdkEnvGuard {
   public:
    static std::shared_ptr<SpdkEnvGuard> Acquire() {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        if (auto existing = Registry().lock()) {
            return existing;
        }
        // Raw pointer so an init failure can be plain-deleted under the lock
        // (the Deleter locks RegistryMutex; destroying a shared_ptr while
        // holding it would deadlock).
        auto* raw = new SpdkEnvGuard();
        if (!raw->ok_) {
            delete raw;
            return nullptr;
        }
        std::shared_ptr<SpdkEnvGuard> guard(raw, Deleter());
        Registry() = guard;
        ProcessPin() = guard;  // pinned until process exit (class comment)
        return guard;
    }

    SpdkEnvGuard(const SpdkEnvGuard&) = delete;
    SpdkEnvGuard& operator=(const SpdkEnvGuard&) = delete;

   private:
    SpdkEnvGuard() {
        // rte_eal_init may restrict the calling thread's CPU affinity, and
        // Linux threads inherit the creator's mask — without a restore,
        // threads the host app spawns later would be pinned to one core.
        // Save/restore the affinity around spdk_env_init (glibc-only APIs,
        // hence Linux-gated).
#if defined(__linux__)
        cpu_set_t orig_cpuset;
        CPU_ZERO(&orig_cpuset);
        const bool affinity_saved =
            pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t),
                                   &orig_cpuset) == 0;
#endif

        struct spdk_env_opts opts{};
        SetEnvOptsSize(opts);
        spdk_env_opts_init(&opts);
        opts.name = "mooncake";
        // Containers without physical-address access (restricted
        // /proc/self/pagemap) cannot run IOVA-as-PA; MC_NOF_IOVA_MODE lets
        // operators opt into "va" there. When unset, SPDK keeps its
        // PA-preferred default (matching baseline SpdkWrapper).
        const char* iova_mode = std::getenv("MC_NOF_IOVA_MODE");
        if (iova_mode && *iova_mode != '\0') {
            opts.iova_mode = iova_mode;
        }
        ok_ = (spdk_env_init(&opts) == 0);

        // Best-effort restore of the caller's original affinity.
#if defined(__linux__)
        if (affinity_saved &&
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
                                   &orig_cpuset) != 0) {
            LOG(WARNING) << "Failed to restore calling thread CPU affinity "
                            "after SPDK env init";
        }
#endif

        if (!ok_) {
            LOG(ERROR) << "spdk_env_init failed"
                       << (iova_mode && *iova_mode != '\0'
                               ? ""
                               : " (in a container without "
                                 "physical-address access, set "
                                 "MC_NOF_IOVA_MODE=va)");
        }
    }

    // Runs when the last reference dies. In practice that never happens:
    // ProcessPin is deliberately leaked (see below), so one reference is
    // always alive and spdk_env_fini never runs — the OS reclaims DPDK
    // resources at process exit, and SPDK v23.01.1 could not be
    // re-initialized after fini anyway. spdk_env_fini is serialized with
    // Acquire via RegistryMutex: a concurrent Acquire holds the same mutex
    // while running spdk_env_init, so DPDK teardown and re-init can never
    // race.
    struct Deleter {
        void operator()(SpdkEnvGuard* p) const {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            if (p->ok_) {
                spdk_env_fini();
            }
            delete p;
        }
    };

    static std::mutex& RegistryMutex() {
        // Deliberately leaked (like the ShmHelper singleton): the Deleter
        // can run after this mutex during static destruction when a guard's
        // last reference is held by an earlier-constructed function static.
        static std::mutex* m = new std::mutex();
        return *m;
    }
    static std::weak_ptr<SpdkEnvGuard>& Registry() {
        // Deliberately leaked (like RegistryMutex): a probe-only initiator
        // never registers memory, so the NofRegistryBundle below is first
        // constructed by its ~Impl backstop during static destruction —
        // after function-local static variables like this one have already been
        // destroyed. The bundle's constructor calls Acquire(), which must
        // not touch a destroyed static. Leaking also means the pin below
        // never releases, so spdk_env_fini never runs at all; the OS
        // reclaims DPDK resources at exit.
        static std::weak_ptr<SpdkEnvGuard>* w =
            new std::weak_ptr<SpdkEnvGuard>();
        return *w;
    }
    // Process-lifetime pin: keeps one reference from the first successful
    // Acquire until process exit, so the env is never finalized mid-process
    // (SPDK v23.01.1 cannot be re-initialized after fini). Leaked like
    // Registry() for the same static-destruction-order reason.
    static std::shared_ptr<SpdkEnvGuard>& ProcessPin() {
        static std::shared_ptr<SpdkEnvGuard>* pin =
            new std::shared_ptr<SpdkEnvGuard>();
        return *pin;
    }

    bool ok_{false};
};

// ---------------------------------------------------------------------------
// Process-global page registry singleton (#3131).
//
// SPDK's translation table is process-global, so the bookkeeping must be
// too: per-instance refcounts would let one initiator instance unmap pages
// another still uses. Each SpdkInitiator::Impl registers under its own
// `this` as the owner token.
//
// The bundle also pins the SPDK env from first use until process exit, so an
// UnregisterMemory can never run after spdk_env_fini.
// ---------------------------------------------------------------------------

#ifdef HAVE_SPDK_MEM_REGISTER
struct NofRegistryBundle {
    std::shared_ptr<SpdkEnvGuard> env;
    NofPageRegistry registry;

    NofRegistryBundle()
        : env(SpdkEnvGuard::Acquire()),
          registry(&spdk_mem_register, &spdk_mem_unregister) {}
};

NofRegistryBundle& GetNofRegistryBundle() {
    // Deliberately leaked (like RegistryMutex and the ShmHelper singleton):
    // avoids static-destruction-order issues with probe initiators' backstop
    // UnregisterAll. A probe-only process first constructs the bundle inside
    // that backstop during static destruction, so its constructor's Acquire()
    // runs late too — safe because Registry()/ProcessPin() are likewise
    // leaked and never destroyed.
    static NofRegistryBundle* bundle = new NofRegistryBundle();
    return *bundle;
}
#endif  // HAVE_SPDK_MEM_REGISTER

// ---------------------------------------------------------------------------
// Opaque handle definition — visible only inside this file.
// ---------------------------------------------------------------------------

class NofSegmentHandle {
   public:
    std::atomic<struct spdk_nvme_qpair*> qpair{nullptr};
    struct spdk_nvme_ns* ns{nullptr};
    std::atomic<bool> failed{false};
    std::string ctrlr_key;
    uint32_t nsid{0};
    uint32_t block_size{0};
    uint64_t capacity{0};
    int64_t last_poll_error{0};
    bool is_rdma{false};
    spdk_memory_domain* rdma_domain{nullptr};
#ifdef USE_SPDK_NPU_UB
    std::shared_ptr<ibv_pd> pd_lease;
#endif
    std::list<std::unique_ptr<VectorIo>> vector_ios;
};

namespace {

struct tr_info {
    struct spdk_nvme_transport_id trid;
    std::string ctrlr_key;
    std::string src_addr;
    uint32_t ns;
};

struct ctrlr_info {
    struct spdk_nvme_ctrlr* ctrlr{nullptr};
    std::atomic<bool> failed{false};
    std::atomic<bool> namespace_changed{false};
    std::string key;
    std::chrono::steady_clock::time_point last_admin_poll{};
    std::mutex admin_mutex;
    std::vector<std::unique_ptr<NofSegmentHandle>> retired_segments;
    std::string src_addr;
    std::map<uint32_t, std::unique_ptr<NofSegmentHandle>> ns_seg;
    std::mutex ns_mutex;
};

// Process-unique owner tokens for the page registry: unlike an Impl's heap
// address, a monotonically increasing id is never recycled (a new Impl could
// otherwise inherit a dead one's leftover registration records).
std::atomic<uint64_t> g_nof_owner_token{1};
std::atomic<uint64_t> g_nof_probe_id{1};

}  // namespace

// ---------------------------------------------------------------------------
// SpdkInitiator::Impl
// ---------------------------------------------------------------------------

class SpdkInitiator::Impl {
   public:
    explicit Impl(std::string source_ip) : source_ip_(std::move(source_ip)) {}

    ~Impl() {
        if (!env_guard_) {
            return;
        }
        // Give timed-out probe commands a bounded chance to complete before
        // the qpairs are freed below.
        DrainQuarantinedProbes();
        {
            std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
            std::lock_guard<std::mutex> nvme_lock(NvmeCtrlrStateMutex());
            for (auto& [_, info] : connected_ctrlrs_) {
                if (info) DestroyController(*info);
            }
            for (auto& info : retired_ctrlrs_) {
                DestroyController(*info);
            }
            connected_ctrlrs_.clear();
        }
#ifdef USE_SPDK_NPU_UB
        for (auto& [ptr, size] : device_buffers_) {
            if (AscendParameterPlane::GetInstance().UnregisterRegion(ptr))
                LOG(ERROR) << "Could not release NoF UB registration at "
                           << ptr;
        }
        if (npu_domain_) spdk_memory_domain_destroy(npu_domain_);
#endif
#ifdef HAVE_SPDK_MEM_REGISTER
        // Backstop, after all I/O teardown above: release every page
        // registration this instance still holds, in case the real-client
        // teardown path missed any.
        if (GetNofRegistryBundle().registry.UnregisterAll(owner_token_) !=
            ErrorCode::OK) {
            LOG(ERROR) << "NoF initiator teardown could not release all page "
                          "registrations; they stay registered until process "
                          "exit";
        }
#endif
        // env_guard_ is released after all qpair/ctrlr teardown above. That
        // only drops this instance's reference: in HAVE_SPDK_MEM_REGISTER
        // builds the leaked NofRegistryBundle keeps the env pinned for the
        // whole process, so spdk_env_fini (in ~SpdkEnvGuard) never runs here.
    }

    NofSegmentHandle* OpenSegment(const std::string& tr_str) {
        if (!EnsureEnv()) {
            return nullptr;
        }
        tr_info tr;
        if (ParseTransPortStr(tr_str, &tr) != 0) {
            return nullptr;
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            bool should_retry = false;
            if (auto* handle = OpenSegmentOnce(tr, &should_retry))
                return handle;
            if (!should_retry) return nullptr;
            // OpenSegmentOnce marks the exact failed generation. The next
            // attempt uses the existing retirement path without detaching
            // a controller while another worker still owns I/O.
        }
        return nullptr;
    }

    NofSegmentHandle* OpenSegmentOnce(const tr_info& tr, bool* should_retry) {
        ctrlr_info* info = nullptr;
        {
            std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
            auto it = connected_ctrlrs_.find(tr.ctrlr_key);
            if (it != connected_ctrlrs_.end() &&
                (it->second->failed ||
                 spdk_nvme_ctrlr_is_failed(it->second->ctrlr))) {
                MarkFailed(*it->second);
                retired_ctrlrs_.push_back(std::move(it->second));
                connected_ctrlrs_.erase(it);
                it = connected_ctrlrs_.end();
            }
            // No callback storage or handle is released here.
            for (auto& retired : retired_ctrlrs_) {
                if (!retired->ctrlr) continue;
                bool busy = false;
                for (auto& [_, seg] : retired->ns_seg)
                    busy |= seg->qpair != nullptr;
                for (auto& seg : retired->retired_segments)
                    busy |= seg->qpair != nullptr;
                if (!busy) {
                    std::lock_guard<std::mutex> nvme_lock(
                        NvmeCtrlrStateMutex());
                    UnsubscribeNamespaceNotices(retired->ctrlr,
                                                &retired->namespace_changed);
                    spdk_nvme_detach(retired->ctrlr);
                    retired->ctrlr = nullptr;
                } else if (tr.ctrlr_key == retired->key) {
                    return nullptr;
                }
            }
            if (it != connected_ctrlrs_.end()) info = it->second.get();
        }
        if (!info) {
            // Connect OUTSIDE the mutex: spdk_nvme_probe may block for
            // seconds and would stall first-time opens of other endpoints.
            auto new_info = std::make_unique<ctrlr_info>();
            new_info->src_addr = tr.src_addr;
            const int rc = ConnectController(&tr.trid, new_info.get());
            if (rc != 0 || !new_info->ctrlr) {
                LOG(ERROR) << "NoF controller connect failed: endpoint="
                           << tr.ctrlr_key << ", src_addr=" << tr.src_addr
                           << ", probe_rc=" << rc
                           << ", attached=" << (new_info->ctrlr != nullptr);
                return nullptr;
            }
            new_info->key = tr.ctrlr_key;
            bool detach_duplicate = false;
            {
                std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
                auto it = connected_ctrlrs_.find(tr.ctrlr_key);
                if (it != connected_ctrlrs_.end()) {
                    // Lost a concurrent first-open race: use the winner;
                    // detach our duplicate outside the mutex (can block).
                    info = it->second.get();
                    detach_duplicate = true;
                } else {
                    info = new_info.get();
                    connected_ctrlrs_[tr.ctrlr_key] = std::move(new_info);
                }
            }
            if (detach_duplicate) {
                std::lock_guard<std::mutex> nvme_lock(NvmeCtrlrStateMutex());
                UnsubscribeNamespaceNotices(new_info->ctrlr,
                                            &new_info->namespace_changed);
                spdk_nvme_detach(new_info->ctrlr);
            }
        }

        NofSegmentHandle* seg_handle = nullptr;
        {
            auto& ns_seg = info->ns_seg;
            std::lock_guard<std::mutex> lock(info->ns_mutex);
            if (info->failed) return nullptr;
            auto ns_it = ns_seg.find(tr.ns);
            if (ns_it != ns_seg.end()) {
                auto* handle = ns_it->second.get();
                if (!handle->failed) {
                    std::lock_guard<std::mutex> admin_lock(info->admin_mutex);
                    if (handle->ns &&
                        spdk_nvme_ns_get_sector_size(handle->ns) != 0 &&
                        spdk_nvme_ns_get_size(handle->ns) != 0) {
                        return handle;
                    }
                    LOG(WARNING)
                        << "cached nof namespace has invalid geometry"
                        << ", ctrlr_key=" << tr.ctrlr_key << ", nsid=" << tr.ns
                        << ", action=reconnect_controller";
                    *should_retry = true;
                    MarkFailedNamespacesLocked(*info);
                    return nullptr;
                }
                info->retired_segments.push_back(std::move(ns_it->second));
                ns_seg.erase(ns_it);
            }
            int32_t namespace_refresh_error = 0;
            if (!WaitForActiveNamespace(*info, tr.ns,
                                        &namespace_refresh_error)) {
                LOG(ERROR) << "spdk_nvme_ctrlr_is_active_ns failed"
                           << ", ctrlr_key=" << tr.ctrlr_key
                           << ", nsid=" << tr.ns
                           << ", admin_error=" << namespace_refresh_error;
                *should_retry =
                    IsControllerConnectionError(namespace_refresh_error);
                if (*should_retry) MarkFailedNamespacesLocked(*info);
                return nullptr;
            }
            std::lock_guard<std::mutex> admin_lock(info->admin_mutex);
            if (info->failed) return nullptr;
            struct spdk_nvme_ns* ns =
                spdk_nvme_ctrlr_get_ns(info->ctrlr, tr.ns);
            if (!ns) {
                LOG(ERROR) << "spdk_nvme_ctrlr_get_ns failed"
                           << ", ctrlr_key=" << tr.ctrlr_key
                           << ", nsid=" << tr.ns;
                return nullptr;
            }

            const auto block_size = spdk_nvme_ns_get_sector_size(ns);
            const auto capacity = spdk_nvme_ns_get_size(ns);
            if (block_size == 0 || capacity == 0) {
                LOG(WARNING)
                    << "nof namespace geometry is not ready"
                    << ", ctrlr_key=" << tr.ctrlr_key << ", nsid=" << tr.ns
                    << ", block_size=" << block_size
                    << ", capacity=" << capacity
                    << ", action=reconnect_controller";
                *should_retry = true;
                MarkFailedNamespacesLocked(*info);
                return nullptr;
            }

            struct spdk_nvme_qpair* qpair =
                spdk_nvme_ctrlr_alloc_io_qpair(info->ctrlr, nullptr, 0);
            if (!qpair) {
                LOG(ERROR) << "alloc spdk_nvme_qpair failed"
                           << ", ctrlr_key=" << tr.ctrlr_key
                           << ", nsid=" << tr.ns;
                *should_retry = true;
                MarkFailedNamespacesLocked(*info);
                return nullptr;
            }

            auto new_seg = std::make_unique<NofSegmentHandle>();
            new_seg->ctrlr_key = tr.ctrlr_key;
            new_seg->nsid = tr.ns;
            new_seg->qpair = qpair;
            new_seg->ns = ns;
            new_seg->block_size = block_size;
            new_seg->capacity = capacity;
            new_seg->is_rdma = tr.trid.trtype == SPDK_NVME_TRANSPORT_RDMA;
#if defined(USE_ASCEND_NDR) || defined(USE_ASCEND_STAGING)
            auto& route = AscendStorageConfig::GetInstance();
            if (route.HasRoute()) {
                const int count =
                    spdk_nvme_ctrlr_get_memory_domains(info->ctrlr, nullptr, 0);
                std::vector<spdk_memory_domain*> domains(std::max(count, 0));
                if (count > 0)
                    spdk_nvme_ctrlr_get_memory_domains(info->ctrlr,
                                                       domains.data(), count);
                for (auto* domain : domains) {
                    if (spdk_memory_domain_get_dma_device_type(domain) !=
                        SPDK_DMA_DEVICE_TYPE_RDMA)
                        continue;
                    auto* ctx = spdk_memory_domain_get_context(domain);
                    if (!ctx || !ctx->user_ctx || ctx->size < sizeof(*ctx))
                        continue;
                    auto* rdma = static_cast<spdk_memory_domain_rdma_ctx*>(
                        ctx->user_ctx);
                    if (rdma->size < sizeof(*rdma)) continue;
                    auto* pd = static_cast<ibv_pd*>(rdma->ibv_pd);
                    if (pd && pd->context &&
                        route.Device().rdma_device ==
                            ibv_get_device_name(pd->context->device)) {
                        new_seg->rdma_domain = domain;
#ifdef USE_SPDK_NPU_UB
                        new_seg->pd_lease = RetainSpdkPd(pd);
#endif
                        break;
                    }
                }
                if (!new_seg->rdma_domain) {
                    LOG(ERROR) << "NoF PD does not match worker RDMA device "
                               << route.Device().rdma_device;
                    spdk_nvme_ctrlr_free_io_qpair(qpair);
                    return nullptr;
                }
            }
#endif
            seg_handle = new_seg.get();
            ns_seg[tr.ns] = std::move(new_seg);
            if (NoFDebugConfig::IsEnabledAtFirstUse()) {
                LOG(INFO) << "NoF I/O qpair opened: qpair=" << qpair
                          << ", ctrlr_key=" << tr.ctrlr_key
                          << ", nsid=" << tr.ns
                          << ", src_addr=" << info->src_addr;
            }
        }
        return seg_handle;
    }

    uint32_t GetBlockSize(const NofSegmentHandle* handle) {
        if (!handle || handle->failed || !handle->ns) {
            return kInvalidBlockSize;
        }
        return handle->block_size;
    }

    int SubmitIO(NofSegmentHandle* handle, void* buffer, uint64_t byte_offset,
                 uint64_t byte_length, NofIOOp op, NofIOAdaptor* adaptor) {
        if (!handle || !buffer || byte_length == 0 || !handle->qpair ||
            !handle->ns || !adaptor || !adaptor->cb) {
            return -EINVAL;
        }
        if (handle->failed) return -ENXIO;
        const uint32_t block_size = handle->block_size;
        // Interface precondition, enforced at submit time.
        if (block_size == 0 || byte_offset % block_size != 0 ||
            byte_length % block_size != 0) {
            LOG(ERROR) << "SubmitIO alignment violation: offset=" << byte_offset
                       << " length=" << byte_length
                       << " block_size=" << block_size;
            return -EINVAL;
        }

#ifdef USE_SPDK_NPU_UB
        if (AscendParameterPlane::GetInstance().Contains(buffer, byte_length))
            return SubmitIOV(handle,
                             {{buffer, static_cast<size_t>(byte_length)}},
                             byte_offset, byte_length, op, adaptor, 0);
#endif
        const uint64_t lba = byte_offset / block_size;
        const uint32_t lba_count =
            static_cast<uint32_t>(byte_length / block_size);
        if (op != NofIOOp::kRead && op != NofIOOp::kWrite) return -EINVAL;
        if (op == NofIOOp::kWrite && NoFDebugConfig::IsEnabledAtFirstUse()) {
            LOG_FIRST_N(INFO, 1)
                << "NoF first host write: qpair=" << handle->qpair.load()
                << ", buffer=" << buffer << ", ssd_offset=" << byte_offset
                << ", bytes=" << byte_length;
        }
        // SPDK's contiguous RDMA command carries one rkey. When a host
        // payload spans registration pages, submit an SGL so each page can
        // use its own rkey without splitting the NVMe block range.
        const size_t page_remaining = NofBytesToRegistrationPageEnd(buffer);
        if (handle->is_rdma && byte_length > page_remaining) {
            if (NoFDebugConfig::IsEnabledAtFirstUse()) {
                LOG_FIRST_N(INFO, 1)
                    << "NoF host RDMA request spans registration pages: qpair="
                    << handle->qpair.load() << ", buffer=" << buffer
                    << ", bytes=" << byte_length
                    << ", first_segment_bytes=" << page_remaining;
            }
            return SubmitIOV(handle,
                             {{buffer, static_cast<size_t>(byte_length)}},
                             byte_offset, byte_length, op, adaptor, 0);
        }
        const int rc = op == NofIOOp::kRead
                           ? spdk_nvme_ns_cmd_read(
                                 handle->ns, handle->qpair, buffer, lba,
                                 lba_count, IOCompleteTrampoline, adaptor, 0)
                           : spdk_nvme_ns_cmd_write(
                                 handle->ns, handle->qpair, buffer, lba,
                                 lba_count, IOCompleteTrampoline, adaptor, 0);
        if (rc != 0) {
            LogSubmitError(handle, op, "host", buffer, byte_offset, byte_length,
                           rc);
        }
        return rc;
    }

    int SubmitIOV(NofSegmentHandle* handle, uint64_t byte_offset,
                  uint64_t byte_length, NofIOOp op, NofSglIOAdaptor* adaptor) {
        if (!handle || !handle->qpair || !handle->ns || !adaptor ||
            !adaptor->cb || !adaptor->reset || !adaptor->next || !byte_length)
            return -EINVAL;
        if (handle->failed) return -ENXIO;
        const uint32_t block_size = handle->block_size;
        if (!block_size || block_size == kInvalidBlockSize ||
            byte_offset % block_size || byte_length % block_size ||
            byte_length / block_size > UINT32_MAX)
            return -EINVAL;
        const uint64_t lba = byte_offset / block_size;
        const auto blocks = static_cast<uint32_t>(byte_length / block_size);
        adaptor->split_at_registration_page = handle->is_rdma;
        if (op == NofIOOp::kRead) {
            return spdk_nvme_ns_cmd_readv(handle->ns, handle->qpair, lba,
                                          blocks, SglCompleteTrampoline,
                                          adaptor, 0, ResetSgl, NextSge);
        }
        if (op == NofIOOp::kWrite) {
            return spdk_nvme_ns_cmd_writev(handle->ns, handle->qpair, lba,
                                           blocks, SglCompleteTrampoline,
                                           adaptor, 0, ResetSgl, NextSge);
        }
        return -EINVAL;
    }

    int AcquirePadding(size_t size, bool for_write, void** ptr,
                       std::shared_ptr<void>* owner) {
        return padding_pool_->Acquire(size, for_write, ptr, owner);
    }

    int64_t PollCompletion(NofSegmentHandle* handle, uint32_t max_completions) {
        if (!handle || !handle->qpair) {
            return -EINVAL;
        }
        if (handle->failed) return -ENXIO;
        // max_completions == 0 means "all pending" (SPDK dialect, kept as an
        // explicit interface contract).
        const int64_t rc =
            spdk_nvme_qpair_process_completions(handle->qpair, max_completions);
        if (rc < 0 && rc != handle->last_poll_error) {
            LOG(ERROR) << "NoF SPDK qpair poll failed: qpair="
                       << handle->qpair.load() << ", rc=" << rc
                       << ", failure_reason="
                       << spdk_nvme_qpair_get_failure_reason(handle->qpair)
                       << ", tracked_vector_ios=" << handle->vector_ios.size();
        }
        handle->last_poll_error = rc < 0 ? rc : 0;
        // Even synchronous -EFAULT completions are delivered only after the
        // worker has accounted for the successfully submitted I/O.
        DrainVectorCompletions(handle);
        return rc;
    }

    static void DrainVectorCompletions(NofSegmentHandle* handle) {
        for (auto it = handle->vector_ios.begin();
             it != handle->vector_ios.end();) {
            if (!(*it)->completed) {
                ++it;
                continue;
            }
            auto io = std::move(*it);
            it = handle->vector_ios.erase(it);
            if (io->reads_into_hbm &&
                !spdk_nvme_cpl_is_error(&io->completion) &&
                NoFDebugConfig::IsEnabledAtFirstUse()) {
                LOG_FIRST_N(INFO, 4)
                    << "NoF HBM read completed: qpair=" << handle->qpair
                    << ", bytes=" << io->length;
            }
            IOCompleteTrampoline(&io->adaptor, &io->completion);
        }
    }

    static void MarkFailed(ctrlr_info& info) {
        info.failed = true;
        std::lock_guard<std::mutex> lock(info.ns_mutex);
        MarkFailedNamespacesLocked(info);
    }

    // Caller holds ns_mutex. Mark the exact controller generation so a late
    // open failure cannot invalidate a newly connected controller by key.
    static void MarkFailedNamespacesLocked(ctrlr_info& info) {
        info.failed = true;
        for (auto& [_, seg] : info.ns_seg) seg->failed = true;
    }

    static void DestroyController(ctrlr_info& info) {
        auto destroy_segment = [](NofSegmentHandle* segment) {
            if (segment->qpair) spdk_nvme_ctrlr_free_io_qpair(segment->qpair);
#ifdef USE_SPDK_NPU_UB
            segment->pd_lease.reset();
#endif
        };
        for (auto& [_, seg] : info.ns_seg) {
            destroy_segment(seg.get());
        }
        for (auto& seg : info.retired_segments) {
            destroy_segment(seg.get());
        }
        if (info.ctrlr) {
            UnsubscribeNamespaceNotices(info.ctrlr, &info.namespace_changed);
            spdk_nvme_detach(info.ctrlr);
        }
    }

    std::string GetControllerKey(const std::string& endpoint) {
        tr_info tr;
        return ParseTransPortStr(endpoint, &tr) == 0 ? tr.ctrlr_key : "";
    }

    std::vector<uint32_t> GetActiveNamespaces(const std::string& endpoint) {
        std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
        auto it = connected_ctrlrs_.find(GetControllerKey(endpoint));
        if (it == connected_ctrlrs_.end() || it->second->failed) return {};
        auto& info = *it->second;
        std::lock_guard<std::mutex> admin_lock(info.admin_mutex);
        std::vector<uint32_t> result;
        for (uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(info.ctrlr);
             nsid != 0;
             nsid = spdk_nvme_ctrlr_get_next_active_ns(info.ctrlr, nsid)) {
            result.push_back(nsid);
        }
        return result;
    }

    uint64_t GetNamespaceCapacityBytes(const NofSegmentHandle* handle) {
        return handle && !handle->failed && handle->ns ? handle->capacity : 0;
    }

    std::vector<std::string> ConsumeNamespaceAttributeChangedControllers() {
        std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
        std::vector<std::string> result;
        for (auto& [key, info] : connected_ctrlrs_) {
            if (info->namespace_changed.exchange(false)) result.push_back(key);
        }
        return result;
    }

    void InvalidateController(const std::string& endpoint) {
        InvalidateControllerKey(GetControllerKey(endpoint));
    }

    void InvalidateControllerKey(const std::string& key) {
        std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
        auto it = connected_ctrlrs_.find(key);
        if (it != connected_ctrlrs_.end()) MarkFailed(*it->second);
    }

    void InvalidateController(NofSegmentHandle* handle) {
        if (!handle) return;
        std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
        auto it = connected_ctrlrs_.find(handle->ctrlr_key);
        if (it == connected_ctrlrs_.end()) return;
        bool current = false;
        {
            std::lock_guard<std::mutex> ns_lock(it->second->ns_mutex);
            auto ns = it->second->ns_seg.find(handle->nsid);
            current =
                ns != it->second->ns_seg.end() && ns->second.get() == handle;
        }
        // A late failure on a retired generation must not invalidate a new
        // connection to the same endpoint.
        if (current) MarkFailed(*it->second);
    }

    void CloseSegment(const std::string& endpoint) {
        tr_info tr;
        if (ParseTransPortStr(endpoint, &tr) != 0) return;
        std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
        auto close = [&](ctrlr_info& info) {
            std::lock_guard<std::mutex> ns_lock(info.ns_mutex);
            auto ns = info.ns_seg.find(tr.ns);
            if (ns != info.ns_seg.end()) AbortSegment(ns->second.get());
        };
        auto it = connected_ctrlrs_.find(tr.ctrlr_key);
        if (it != connected_ctrlrs_.end()) close(*it->second);
        for (auto& info : retired_ctrlrs_) {
            if (info->key == tr.ctrlr_key) close(*info);
        }
    }

    int32_t PollAdminCompletions(NofSegmentHandle* handle) {
        if (!handle || handle->failed) return -ENXIO;
        std::lock_guard<std::mutex> lock(ctrlrs_mutex_);
        auto it = connected_ctrlrs_.find(handle->ctrlr_key);
        if (it == connected_ctrlrs_.end() || it->second->failed) return -ENXIO;
        auto& info = *it->second;
        return PollAdminCompletions(info, false);
    }

    int32_t PollAdminCompletions(ctrlr_info& info, bool force) {
        std::lock_guard<std::mutex> admin_lock(info.admin_mutex);
        const auto now = std::chrono::steady_clock::now();
        static const auto interval = std::chrono::milliseconds(
            std::min<uint64_t>(SpdkControllerConfig::FromEnvironment()
                                   .nof_admin_poll_interval_ms,
                               static_cast<uint64_t>(
                                   std::chrono::milliseconds::max().count())));
        if (info.failed || !info.ctrlr) return -ENXIO;
        if (!force && now - info.last_admin_poll < interval) return 0;
        info.last_admin_poll = now;
        return spdk_nvme_ctrlr_process_admin_completions(info.ctrlr);
    }

    bool WaitForActiveNamespace(ctrlr_info& info, uint32_t nsid,
                                int32_t* admin_error) {
        static const auto config = SpdkControllerConfig::FromEnvironment();
        const auto milliseconds = [](uint64_t value) {
            return std::chrono::milliseconds(std::min<uint64_t>(
                value, static_cast<uint64_t>(
                           std::chrono::milliseconds::max().count())));
        };
        const auto result = RefreshNofNamespace(
            [&] {
                std::lock_guard<std::mutex> lock(info.admin_mutex);
                return !info.failed && info.ctrlr &&
                       spdk_nvme_ctrlr_is_active_ns(info.ctrlr, nsid);
            },
            [&] { return PollAdminCompletions(info, true); },
            milliseconds(config.nof_namespace_refresh_timeout_ms),
            milliseconds(config.nof_namespace_refresh_poll_interval_ms));
        *admin_error = result.admin_error;
        if (result.admin_error < 0) {
            LOG(ERROR) << "refresh nof namespaces failed"
                       << ", ctrlr_key=" << info.key << ", nsid=" << nsid
                       << ", admin_ret=" << result.admin_error
                       << ", poll_count=" << result.poll_count;
        } else if (!result.active) {
            LOG(WARNING) << "nof namespace remains inactive after admin refresh"
                         << ", ctrlr_key=" << info.key << ", nsid=" << nsid
                         << ", timeout_ms="
                         << config.nof_namespace_refresh_timeout_ms
                         << ", poll_count=" << result.poll_count;
        } else if (result.poll_count > 0) {
            LOG(INFO) << "nof namespace became active after admin refresh"
                      << ", ctrlr_key=" << info.key << ", nsid=" << nsid
                      << ", poll_count=" << result.poll_count;
        }
        return result.active;
    }

    bool AbortSegment(NofSegmentHandle* handle) {
        if (!handle) return true;
        handle->failed = true;
        // Owner thread only, outside a completion callback: freeing the qpair
        // disconnects it and synchronously aborts its outstanding requests.
        auto* qpair = handle->qpair.load();
        if (qpair && spdk_nvme_ctrlr_free_io_qpair(qpair) != 0) return false;
        DrainVectorCompletions(handle);
#ifdef USE_SPDK_NPU_UB
        handle->pd_lease.reset();
#endif
        handle->qpair = nullptr;
        return true;
    }

    ErrorCode RegisterMemory(void* ptr, size_t size, bool host_pool) {
        if (!ptr || size == 0) {
            return ErrorCode::INVALID_PARAMS;
        }
        if (!EnsureEnv()) {
            return ErrorCode::INTERNAL_ERROR;
        }
#ifdef HAVE_SPDK_MEM_REGISTER
        // A non-aligned ptr is floored to its 2MB page, which assumes the
        // whole containing page is mapped and hugepage-backed — warn loudly
        // rather than fail silently later inside SPDK.
        if (reinterpret_cast<uintptr_t>(ptr) % kNofRegistrationPageSize != 0) {
            LOG(WARNING) << "RegisterMemory: ptr " << ptr
                         << " is not 2MB-aligned; registering the whole "
                            "containing 2MB pages, which must be mapped "
                            "and hugepage-backed";
        }
        // Registrations are charged per initiator instance via owner_token_.
        auto& registry = GetNofRegistryBundle().registry;
        size_t span = kNofRegistrationPageSize;
        if (host_pool) {
            constexpr size_t mib = 1ULL << 20;
            static const auto span_mib = Environ::Process().GetTypedOr(
                EnvironmentVariable<size_t>{"MC_NOF_HOST_MR_SIZE_MB"},
                NofPageRegistry::kDefaultPoolSpan / mib);
            if (span_mib < 2 || span_mib > 256 || span_mib % 2) {
                LOG(ERROR) << "MC_NOF_HOST_MR_SIZE_MB must be an even number "
                              "from 2 through 256";
                return ErrorCode::INVALID_PARAMS;
            }
            span = span_mib * mib;
        }
        size_t new_spans = 0;
        const auto rc =
            registry.RegisterPool(owner_token_, ptr, size, span, &new_spans);
        if (rc != ErrorCode::OK) {
            LOG(ERROR) << "NoF SPDK host registration failed: buffer=" << ptr
                       << ", bytes=" << size << ", result=" << toString(rc);
        } else if (NoFDebugConfig::IsEnabledAtFirstUse()) {
            LOG(INFO) << "NoF SPDK host registration succeeded: buffer=" << ptr
                      << ", bytes=" << size
                      << ", max_registration_bytes=" << span
                      << ", new_spdk_spans=" << new_spans;
        }
        return rc;
#else
        if (host_pool) {
            LOG(ERROR) << "NoF host pool requires spdk_mem_register";
            return ErrorCode::INVALID_PARAMS;
        }
        LOG(WARNING) << "SPDK build lacks spdk_mem_register; NoF over RDMA "
                        "will fail for unregistered user buffers (#3131)";
        return ErrorCode::OK;
#endif
    }

    ErrorCode RegisterDeviceMemory(void* ptr, size_t size) {
#ifdef USE_SPDK_NPU_UB
        if (!ptr || !size || !EnsureEnv()) return ErrorCode::INVALID_PARAMS;
        std::lock_guard<std::mutex> lock(device_mutex_);
        auto it = device_buffers_.find(ptr);
        if (it != device_buffers_.end())
            return it->second == size ? ErrorCode::OK
                                      : ErrorCode::INVALID_PARAMS;
        if (!npu_domain_) {
            if (spdk_memory_domain_create(
                    &npu_domain_, SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START,
                    nullptr, "ascend-ub") != 0)
                return ErrorCode::INTERNAL_ERROR;
            spdk_memory_domain_set_translation(npu_domain_,
                                               TranslateNpuUbMemory);
        }
        if (AscendParameterPlane::GetInstance().RegisterRegion(ptr, size) != 0)
            return ErrorCode::INVALID_PARAMS;
        device_buffers_.emplace(ptr, size);
        return ErrorCode::OK;
#else
        return ErrorCode::INVALID_PARAMS;
#endif
    }

    int SubmitIOV(NofSegmentHandle* handle,
                  const std::vector<NofBuffer>& buffers, uint64_t byte_offset,
                  uint64_t byte_length, NofIOOp op, NofIOAdaptor* adaptor,
                  size_t buffer_offset) {
        if (!handle || !handle->ns || !handle->qpair || !adaptor ||
            !adaptor->cb || !byte_length ||
            (op != NofIOOp::kRead && op != NofIOOp::kWrite))
            return -EINVAL;
        if (handle->failed) return -ENXIO;
        const uint32_t block_size = GetBlockSize(handle);
        if (!block_size || block_size == kInvalidBlockSize ||
            byte_offset % block_size || byte_length % block_size ||
            byte_length / block_size > UINT32_MAX)
            return -EINVAL;
        size_t total = 0;
        bool device = false, host = false;
        for (const auto& buffer : buffers) {
            if (!buffer.data || !buffer.size || buffer.size > SIZE_MAX - total)
                return -EINVAL;
            total += buffer.size;
#ifdef USE_SPDK_NPU_UB
            if (AscendParameterPlane::GetInstance().Contains(buffer.data,
                                                             buffer.size))
                device = true;
            else
#endif
                host = true;
        }
        if (buffer_offset > total || byte_length > total - buffer_offset ||
            (device && host))
            return -EINVAL;
        auto io = std::unique_ptr<VectorIo>(new (std::nothrow) VectorIo);
        if (!io) return -ENOMEM;
        io->buffers = buffers;
        io->start = buffer_offset;
        io->length = byte_length;
        io->split_at_registration_page = handle->is_rdma && host;
        io->adaptor = *adaptor;
        io->opts.size = sizeof(io->opts);
#ifdef USE_SPDK_NPU_UB
        if (device) {
            if (!handle->rdma_domain || !npu_domain_) return -ENOTSUP;
            io->opts.memory_domain = npu_domain_;
            io->reads_into_hbm = op == NofIOOp::kRead;
            if (op == NofIOOp::kWrite) {
                const int rc = ConfigureNpuWriteCapsule(io->opts);
                if (rc != 0) return rc;
            }
        }
#endif
        auto* request = io.get();
        handle->vector_ios.push_back(std::move(io));
        auto entry = std::prev(handle->vector_ios.end());
        const uint64_t lba = byte_offset / block_size;
        const uint32_t count = byte_length / block_size;
        const int rc =
            op == NofIOOp::kRead
                ? spdk_nvme_ns_cmd_readv_ext(
                      handle->ns, handle->qpair, lba, count, VectorComplete,
                      request, ResetVector, NextVector, &request->opts)
                : spdk_nvme_ns_cmd_writev_ext(
                      handle->ns, handle->qpair, lba, count, VectorComplete,
                      request, ResetVector, NextVector, &request->opts);
        if (rc != 0) {
            LogSubmitError(handle, op, device ? "hbm-sgl" : "host-sgl",
                           buffers.front().data, byte_offset, byte_length, rc);
        }
        if (rc == -EFAULT) return 0;
        if (rc != 0) handle->vector_ios.erase(entry);
        return rc;
    }

    ErrorCode UnregisterMemory(void* ptr) {
#ifdef USE_SPDK_NPU_UB
        {
            std::lock_guard<std::mutex> lock(device_mutex_);
            auto it = device_buffers_.find(ptr);
            if (it != device_buffers_.end()) {
                if (AscendParameterPlane::GetInstance().UnregisterRegion(ptr))
                    return ErrorCode::INTERNAL_ERROR;
                device_buffers_.erase(it);
                return ErrorCode::OK;
            }
        }
#endif
#ifdef HAVE_SPDK_MEM_REGISTER
        return GetNofRegistryBundle().registry.Unregister(owner_token_, ptr);
#else
        (void)ptr;
        return ErrorCode::OK;
#endif
    }

    bool ProbeSegment(const std::string& tr_str, uint32_t timeout_ms,
                      std::string* error_reason) {
        if (!EnsureEnv()) {
            if (error_reason) {
                *error_reason = "spdk_env_init_fail";
            }
            return false;
        }

        NofSegmentHandle* seg_handle = OpenSegment(tr_str);
        if (!seg_handle) {
            if (error_reason) {
                *error_reason = "open_fail";
            }
            return false;
        }

        uint32_t block_size = GetBlockSize(seg_handle);
        if (block_size == kInvalidBlockSize || block_size == 0) {
            if (error_reason) {
                *error_reason = "invalid_block_size";
            }
            return false;
        }

        void* buffer =
            spdk_zmalloc(block_size, 0x1000, nullptr, -1, SPDK_MALLOC_DMA);
        if (!buffer) {
            if (error_reason) *error_reason = "alloc_fail";
            return false;
        }
        NofProbeContext* probe_ctx = probe_pool_.Acquire();
        probe_ctx->adaptor = {&ProbeReadComplete, probe_ctx};
        probe_ctx->seg = seg_handle;
        probe_ctx->probe_id =
            g_nof_probe_id.fetch_add(1, std::memory_order_relaxed);
        probe_ctx->submitted_at = std::chrono::steady_clock::now();
        probe_ctx->timeout = std::chrono::milliseconds(timeout_ms);
        auto* qpair = seg_handle->qpair.load();
        errno = 0;
        const int submit_rc = SubmitIO(seg_handle, buffer, 0, block_size,
                                       NofIOOp::kRead, &probe_ctx->adaptor);
        const int submit_errno = errno;
        if (submit_rc != 0) {
            LOG(WARNING)
                << "NoF heartbeat read submission failed: endpoint=" << tr_str
                << ", probe_id=" << probe_ctx->probe_id
                << ", qid=" << (qpair ? spdk_nvme_qpair_get_id(qpair) : 0)
                << ", qpair=" << qpair << ", lba=0, bytes=" << block_size
                << ", submit_rc=" << submit_rc
                << ", submit_errno=" << submit_errno << ", connected="
                << (qpair && spdk_nvme_qpair_is_connected(qpair))
                << ", qpair_failure_reason="
                << (qpair ? spdk_nvme_qpair_get_failure_reason(qpair) : 0);
            probe_pool_.Recycle(probe_ctx);
            if (error_reason) {
                *error_reason =
                    IsControllerConnectionError(submit_rc)
                        ? "controller_error:" + std::to_string(submit_rc)
                        : "submit_fail";
            }
            if (IsControllerConnectionError(submit_rc)) {
                InvalidateController(seg_handle);
            }
            AbortSegment(seg_handle);
            spdk_free(buffer);
            return false;
        }

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        int64_t last_poll_rc = 0;
        int64_t first_poll_error = 0;
        int first_poll_errno = 0;
        int64_t connection_error = 0;
        uint64_t poll_count = 0;
        uint64_t completions_seen = 0;
        while (!probe_ctx->done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            const int admin_ret = PollAdminCompletions(seg_handle);
            errno = 0;
            last_poll_rc = PollCompletion(seg_handle, 0);
            const int poll_errno = errno;
            ++poll_count;
            if (last_poll_rc > 0) {
                completions_seen += static_cast<uint64_t>(last_poll_rc);
            } else if (last_poll_rc < 0 && first_poll_error == 0) {
                first_poll_error = last_poll_rc;
                first_poll_errno = poll_errno;
            }
            if (IsControllerConnectionError(admin_ret) ||
                IsControllerConnectionError(last_poll_rc)) {
                connection_error = IsControllerConnectionError(admin_ret)
                                       ? admin_ret
                                       : last_poll_rc;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const bool done = probe_ctx->done.load(std::memory_order_acquire);
        const bool ok = connection_error == 0 && done &&
                        probe_ctx->success.load(std::memory_order_acquire);
        if (!ok) {
            std::string reason =
                done ? "completion_error" : "completion_timeout";
            if (connection_error != 0) {
                reason = "controller_error:" + std::to_string(connection_error);
            } else if (done) {
                std::lock_guard<std::mutex> lock(probe_ctx->error_mutex);
                if (!probe_ctx->error_reason.empty()) {
                    reason = probe_ctx->error_reason;
                }
            }
            if (error_reason) *error_reason = reason;
            auto* ctrlr = spdk_nvme_ns_get_ctrlr(seg_handle->ns);
            const auto* opts = spdk_nvme_ctrlr_get_opts(ctrlr);
            LOG(WARNING)
                << "NoF heartbeat read failed: endpoint=" << tr_str
                << ", probe_id=" << probe_ctx->probe_id
                << ", cntlid=" << spdk_nvme_ctrlr_get_id(ctrlr)
                << ", qid=" << (qpair ? spdk_nvme_qpair_get_id(qpair) : 0)
                << ", hostnqn=" << opts->hostnqn << ", src_addr="
                << (opts->src_addr[0] ? opts->src_addr : "auto")
                << ", qpair=" << qpair << ", lba=0, bytes=" << block_size
                << ", timeout_ms=" << timeout_ms << ", submit_rc=" << submit_rc
                << ", poll_count=" << poll_count
                << ", completions_seen=" << completions_seen
                << ", first_poll_error=" << first_poll_error
                << ", first_poll_errno=" << first_poll_errno
                << ", last_poll_rc=" << last_poll_rc << ", connected="
                << (qpair && spdk_nvme_qpair_is_connected(qpair))
                << ", qpair_failure_reason="
                << (qpair ? spdk_nvme_qpair_get_failure_reason(qpair) : 0)
                << ", reason=" << reason;
        }

        if (connection_error != 0 || !done) {
            InvalidateController(seg_handle);
            AbortSegment(seg_handle);
        }
        // An unsuccessful abort can leave SPDK holding both the probe
        // context and its DMA buffer until the controller is drained.
        if (probe_ctx->done.load(std::memory_order_acquire)) {
            probe_pool_.Recycle(probe_ctx);
            spdk_free(buffer);
        } else {
            probe_pool_.Quarantine(probe_ctx);
            if (!seg_handle->qpair) spdk_free(buffer);
        }
        return ok;
    }

   private:
    static void LogSubmitError(NofSegmentHandle* handle, NofIOOp op,
                               const char* memory_type, const void* buffer,
                               uint64_t byte_offset, uint64_t byte_length,
                               int rc) {
        LOG_EVERY_N(ERROR, 128)
            << "NoF SPDK submit failed: qpair=" << handle->qpair.load()
            << ", op=" << (op == NofIOOp::kWrite ? "write" : "read")
            << ", memory=" << memory_type << ", buffer=" << buffer
            << ", ssd_offset=" << byte_offset << ", bytes=" << byte_length
            << ", rc=" << rc;
    }

    bool EnsureEnv() {
        // Locked check-then-set: an unlocked read of a shared_ptr member is
        // a torn-read UB. Cold path, one mutex acquisition is negligible.
        std::lock_guard<std::mutex> lock(env_mutex_);
        if (!env_guard_) {
            env_guard_ = SpdkEnvGuard::Acquire();
        }
        return env_guard_ != nullptr;
    }

    static void ResetSgl(void* ctx, uint32_t offset) {
        auto* adaptor = static_cast<NofSglIOAdaptor*>(ctx);
        adaptor->reset(adaptor->ctx, offset);
    }
    static int NextSge(void* ctx, void** address, uint32_t* length) {
        auto* adaptor = static_cast<NofSglIOAdaptor*>(ctx);
        return adaptor->next(adaptor->ctx, address, length);
    }
    static void SglCompleteTrampoline(void* ctx, const spdk_nvme_cpl* cpl) {
        auto* adaptor = static_cast<NofSglIOAdaptor*>(ctx);
        IOCompleteTrampoline(static_cast<NofIOAdaptor*>(adaptor), cpl);
    }

#ifdef USE_SPDK_NPU_UB
    static int TranslateNpuUbMemory(
        spdk_memory_domain*, void*, spdk_memory_domain* destination,
        spdk_memory_domain_translation_ctx*, void* addr, size_t length,
        spdk_memory_domain_translation_result* result) {
        auto* pd = GetRdmaPd(destination);
        if (!pd || !result || !addr || !length) return -EINVAL;
        ibv_mr* mr = nullptr;
        const int rc = AscendParameterPlane::GetInstance().GetOrCreateMr(
            pd, addr, length,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                IBV_ACCESS_REMOTE_WRITE,
            &mr);
        if (rc) {
            LOG(ERROR) << "NoF HBM memory translation failed: pd=" << pd
                       << ", addr=" << addr << ", bytes=" << length
                       << ", rc=" << rc;
            return rc;
        }
        result->size = sizeof(*result);
        result->iov_count = 1;
        result->iov = {addr, length};
        result->iovs = nullptr;
        result->dst_domain = destination;
        result->rdma.lkey = mr->lkey;
        result->rdma.rkey = mr->rkey;
        return 0;
    }
#endif

    static void IOCompleteTrampoline(void* cb_arg,
                                     const struct spdk_nvme_cpl* cpl) {
        auto* adaptor = static_cast<NofIOAdaptor*>(cb_arg);
        NofIOCompletion completion;
        if (spdk_nvme_cpl_is_error(cpl)) {
            completion.success = false;
            completion.sc = cpl->status.sc;
            completion.sct = cpl->status.sct;
            const bool namespace_failure =
                cpl->status.sct == SPDK_NVME_SCT_MEDIA_ERROR ||
                (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
                 (cpl->status.sc == SPDK_NVME_SC_INTERNAL_DEVICE_ERROR ||
                  cpl->status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT));
            completion.error_string =
                std::string(namespace_failure ? "namespace_io_error:"
                                              : "completion_error:") +
                spdk_nvme_cpl_get_status_string(&cpl->status);
        } else {
            completion.success = true;
        }
        adaptor->cb(adaptor->ctx, completion);
    }

    static void ProbeReadComplete(void* ctx, const NofIOCompletion& c) {
        auto* probe_ctx = reinterpret_cast<NofProbeContext*>(ctx);
        const auto elapsed =
            std::chrono::steady_clock::now() - probe_ctx->submitted_at;
        if (elapsed >= probe_ctx->timeout) {
            auto* seg = probe_ctx->seg;
            auto* ctrlr = spdk_nvme_ns_get_ctrlr(seg->ns);
            LOG(WARNING)
                << "NoF heartbeat read completed after timeout budget: "
                << "probe_id=" << probe_ctx->probe_id
                << ", cntlid=" << spdk_nvme_ctrlr_get_id(ctrlr)
                << ", qid=" << spdk_nvme_qpair_get_id(seg->qpair)
                << ", nsid=" << spdk_nvme_ns_get_id(seg->ns) << ", elapsed_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       elapsed)
                       .count()
                << ", success=" << c.success << ", sc=" << c.sc
                << ", sct=" << c.sct << ", status=" << c.error_string;
        }
        if (!c.success) {
            {
                std::lock_guard<std::mutex> lock(probe_ctx->error_mutex);
                probe_ctx->error_reason =
                    (c.error_string.empty() ? "completion_error"
                                            : c.error_string) +
                    ", sc=" + std::to_string(c.sc) +
                    ", sct=" + std::to_string(c.sct);
            }
            probe_ctx->success.store(false, std::memory_order_release);
        } else {
            probe_ctx->success.store(true, std::memory_order_release);
        }
        probe_ctx->done.store(true, std::memory_order_release);
        // No recycle here — see ProbeSegment.
    }

    int ParseTransPortStr(const std::string& tr_str, tr_info* info) {
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
            } catch (const std::exception& e) {
                LOG(ERROR) << "Failed to parse ns value: " << ns_str
                           << ", error: " << e.what();
                return -1;
            }
        } else {
            // ns: is optional (both forms are accepted, e.g. from Python);
            // info->ns keeps its default of 1.
            LOG(WARNING) << "No ns field found in transport string, "
                            "defaulting to ns=1";
        }

        info->src_addr = source_ip_;
#if defined(USE_ASCEND_NDR) || defined(USE_ASCEND_STAGING)
        auto& route = AscendStorageConfig::GetInstance();
        if (route.HasRoute()) {
            if (info->trid.trtype != SPDK_NVME_TRANSPORT_RDMA) return -EINVAL;
            if (info->src_addr.empty()) info->src_addr = route.Device().rdma_ip;
        }
#endif
        info->ctrlr_key = std::string(info->trid.traddr) + "|" +
                          std::string(info->trid.trsvcid) + "|" +
                          std::string(info->trid.subnqn) + "|" +
                          std::to_string(static_cast<int>(info->trid.trtype)) +
                          "|" + info->src_addr;

        VLOG(1) << "traddr:" << info->trid.traddr
                << " trsvcid:" << info->trid.trsvcid << " ns:" << info->ns
                << " subnqn:" << info->trid.subnqn
                << " trtype:" << info->trid.trtype;

        return 0;
    }

    int ConnectController(const struct spdk_nvme_transport_id* trid,
                          ctrlr_info* info) {
        auto probe_cb = [](void* cb_ctx,
                           const struct spdk_nvme_transport_id* trid,
                           struct spdk_nvme_ctrlr_opts* opts) -> bool {
            ApplyCtrlrOptsFromEnv(opts);
            auto* info = static_cast<ctrlr_info*>(cb_ctx);
            if (!info->src_addr.empty())
                std::snprintf(opts->src_addr, sizeof(opts->src_addr), "%s",
                              info->src_addr.c_str());
            LOG(INFO) << "Attaching to " << trid->traddr << " " << trid->subnqn
                      << ", src_addr="
                      << (info->src_addr.empty() ? "auto" : info->src_addr);
            return true;
        };

        auto attach_cb = [](void* cb_ctx,
                            const struct spdk_nvme_transport_id* trid,
                            struct spdk_nvme_ctrlr* ctrlr,
                            const struct spdk_nvme_ctrlr_opts* opts) {
            LOG(INFO) << "Attached to " << trid->traddr << " " << trid->subnqn;
            ctrlr_info* info = (ctrlr_info*)cb_ctx;
            info->ctrlr = ctrlr;
            SubscribeNamespaceNotices(ctrlr, &info->namespace_changed);
        };

        // Serialize probes process-wide (see NvmeCtrlrStateMutex): the
        // callbacks only set info->ctrlr and log, so nothing here can
        // re-enter a lock.
        std::lock_guard<std::mutex> nvme_lock(NvmeCtrlrStateMutex());
        return spdk_nvme_probe(trid, (void*)info, probe_cb, attach_cb, NULL);
    }

    // Bounded grace period for timed-out probes at teardown: SPDK requires a
    // qpair to have no outstanding I/O when freed, and completions are only
    // processed by polling.
    void DrainQuarantinedProbes() {
        const auto stale = probe_pool_.QuarantinedSnapshot();
        if (stale.empty()) {
            return;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        bool pending = true;
        while (pending && std::chrono::steady_clock::now() < deadline) {
            pending = false;
            for (auto* ctx : stale) {
                if (ctx->done.load(std::memory_order_acquire) || !ctx->seg) {
                    continue;
                }
                PollCompletion(ctx->seg, 0);
                pending = pending || !ctx->done.load(std::memory_order_acquire);
            }
            if (pending) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (pending) {
            LOG(WARNING) << "Timed-out probe command(s) still outstanding at "
                            "initiator teardown; dropped with the qpair";
        }
    }

    // Member order matters: env_guard_ is declared FIRST so it is destroyed
    // LAST — all qpairs/ctrlrs are detached before spdk_env_fini runs.
    std::shared_ptr<SpdkEnvGuard> env_guard_;
    const std::string source_ip_;
#ifdef USE_SPDK_NPU_UB
    std::mutex device_mutex_;
    std::map<void*, size_t> device_buffers_;
    spdk_memory_domain* npu_domain_{nullptr};
#endif
    // Process-unique registry owner token (see g_nof_owner_token).
    const uint64_t owner_token_ =
        g_nof_owner_token.fetch_add(1, std::memory_order_relaxed);
    std::mutex env_mutex_;
    std::map<std::string, std::unique_ptr<ctrlr_info>> connected_ctrlrs_;
    std::mutex ctrlrs_mutex_;
    std::vector<std::unique_ptr<ctrlr_info>> retired_ctrlrs_;
    NofProbeContextPool probe_pool_;
    std::shared_ptr<NofPaddingPool> padding_pool_ =
        std::make_shared<NofPaddingPool>(std::make_shared<SpdkDmaAllocator>());
};

// ---------------------------------------------------------------------------
// SpdkInitiator forwarding
// ---------------------------------------------------------------------------

SpdkInitiator::SpdkInitiator(std::string source_ip)
    : impl_(std::make_unique<Impl>(std::move(source_ip))) {}
SpdkInitiator::~SpdkInitiator() = default;

NofSegmentHandle* SpdkInitiator::OpenSegment(const std::string& tr_str) {
    return impl_->OpenSegment(tr_str);
}

bool SpdkInitiator::ProbeSegment(const std::string& tr_str, uint32_t timeout_ms,
                                 std::string* error_reason) {
    return impl_->ProbeSegment(tr_str, timeout_ms, error_reason);
}

std::string SpdkInitiator::GetControllerKey(const std::string& endpoint) {
    return impl_->GetControllerKey(endpoint);
}

std::vector<uint32_t> SpdkInitiator::GetActiveNamespaces(
    const std::string& endpoint) {
    return impl_->GetActiveNamespaces(endpoint);
}

uint64_t SpdkInitiator::GetNamespaceCapacityBytes(
    const NofSegmentHandle* handle) {
    return impl_->GetNamespaceCapacityBytes(handle);
}

std::vector<std::string>
SpdkInitiator::ConsumeNamespaceAttributeChangedControllers() {
    return impl_->ConsumeNamespaceAttributeChangedControllers();
}

void SpdkInitiator::InvalidateController(const std::string& endpoint) {
    return impl_->InvalidateController(endpoint);
}

void SpdkInitiator::InvalidateController(NofSegmentHandle* handle) {
    return impl_->InvalidateController(handle);
}

void SpdkInitiator::CloseSegment(const std::string& endpoint) {
    return impl_->CloseSegment(endpoint);
}

int32_t SpdkInitiator::PollAdminCompletions(NofSegmentHandle* handle) {
    return impl_->PollAdminCompletions(handle);
}

bool SpdkInitiator::AbortSegment(NofSegmentHandle* handle) {
    return impl_->AbortSegment(handle);
}

uint32_t SpdkInitiator::GetBlockSize(const NofSegmentHandle* handle) {
    return impl_->GetBlockSize(handle);
}

int SpdkInitiator::SubmitIO(NofSegmentHandle* handle, void* buffer,
                            uint64_t byte_offset, uint64_t byte_length,
                            NofIOOp op, NofIOAdaptor* adaptor) {
    return impl_->SubmitIO(handle, buffer, byte_offset, byte_length, op,
                           adaptor);
}

int SpdkInitiator::SubmitIOV(NofSegmentHandle* handle, uint64_t byte_offset,
                             uint64_t byte_length, NofIOOp op,
                             NofSglIOAdaptor* adaptor) {
    return impl_->SubmitIOV(handle, byte_offset, byte_length, op, adaptor);
}

int SpdkInitiator::AcquirePadding(size_t size, bool for_write, void** ptr,
                                  std::shared_ptr<void>* owner) {
    return impl_->AcquirePadding(size, for_write, ptr, owner);
}

int SpdkInitiator::SubmitIOV(NofSegmentHandle* handle,
                             const std::vector<NofBuffer>& buffers,
                             uint64_t byte_offset, uint64_t byte_length,
                             NofIOOp op, NofIOAdaptor* adaptor,
                             size_t buffer_offset) {
    return impl_->SubmitIOV(handle, buffers, byte_offset, byte_length, op,
                            adaptor, buffer_offset);
}
ErrorCode SpdkInitiator::RegisterDeviceMemory(void* ptr, size_t size) {
    return impl_->RegisterDeviceMemory(ptr, size);
}

int64_t SpdkInitiator::PollCompletion(NofSegmentHandle* handle,
                                      uint32_t max_completions) {
    return impl_->PollCompletion(handle, max_completions);
}

ErrorCode SpdkInitiator::RegisterMemory(void* ptr, size_t size) {
    return impl_->RegisterMemory(ptr, size, false);
}

ErrorCode SpdkInitiator::RegisterHostPool(void* ptr, size_t size) {
    return impl_->RegisterMemory(ptr, size, true);
}

ErrorCode SpdkInitiator::UnregisterMemory(void* ptr) {
    return impl_->UnregisterMemory(ptr);
}

NofCapabilities SpdkInitiator::GetCapabilities() const {
    NofCapabilities caps;
    caps.supports_sgl = true;
    caps.dma_alignment = 4;
    // Upper bound: registration capability exists in this build;
    // not transport-specific. RegisterMemory on TCP NVMe-oF is harmless,
    // callers must not skip it based on this flag.
#ifdef HAVE_SPDK_MEM_REGISTER
    caps.requires_memory_registration = true;
#else
    caps.requires_memory_registration = false;
#endif
    return caps;
}

// ---------------------------------------------------------------------------
// SpdkDmaAllocator
// ---------------------------------------------------------------------------

SpdkDmaAllocator::SpdkDmaAllocator() = default;
SpdkDmaAllocator::~SpdkDmaAllocator() = default;

void* SpdkDmaAllocator::Alloc(size_t size, size_t align, int socket_id) {
    {
        // Locked check-then-set: an unlocked read of a shared_ptr member is
        // a torn-read UB.
        std::lock_guard<std::mutex> lock(env_mutex_);
        if (!env_guard_) {
            env_guard_ = SpdkEnvGuard::Acquire();
            if (!env_guard_) {
                return nullptr;
            }
        }
    }
    return spdk_zmalloc(size, align, nullptr, socket_id, SPDK_MALLOC_DMA);
}

void SpdkDmaAllocator::Free(void* ptr) {
    if (ptr) {
        spdk_free(ptr);
    }
}

}  // namespace mooncake
