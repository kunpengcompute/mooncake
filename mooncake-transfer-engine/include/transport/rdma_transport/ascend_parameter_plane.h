// Copyright 2024 KVCache.AI
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <infiniband/verbs.h>
#include "transport/rdma_transport/ascend_storage_config.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>

namespace mooncake {

using AscendParameterPlaneDevice = AscendStorageDevice;

/**
 * Process-wide registry for Ascend HBM exposed through the A5 parameter plane.
 *
 * UB registration is shared by RdmaTransport and the SPDK memory-domain
 * adapter. RDMA MRs are cached per protection domain because lkey/rkey values
 * are valid only in the PD in which they were created.
 */
class AscendParameterPlane {
   public:
    AscendParameterPlane(const AscendParameterPlane &) = delete;
    AscendParameterPlane &operator=(const AscendParameterPlane &) = delete;

    static AscendParameterPlane &GetInstance();

    int InitializeFromEnvironment();
    bool IsEnabled() const;
    const AscendParameterPlaneDevice &Device() const;

    bool IsDeviceMemory(void *addr, int *device_id = nullptr) const;
    int RegisterRegion(void *addr, size_t length);
    int UnregisterRegion(void *addr);
    bool Contains(void *addr, size_t length) const;

    int GetOrCreateMr(struct ibv_pd *pd, void *addr, size_t length, int access,
                      struct ibv_mr **mr);
    // Select a reusable registration window without exceeding the RNIC limit.
    static bool GetMrRange(uintptr_t base, size_t capacity, uintptr_t address,
                           size_t length, size_t limit, uintptr_t &mr_base,
                           size_t &mr_length);
    void InvalidatePd(struct ibv_pd *pd);
    void Shutdown();

   private:
    friend class AscendParameterPlaneTestPeer;
    AscendParameterPlane() = default;
    ~AscendParameterPlane();

    struct Region {
        void *base{nullptr};
        size_t length{0};
        size_t references{0};
        using MrKey = std::tuple<struct ibv_pd *, uintptr_t, size_t, int>;
        std::map<MrKey, struct ibv_mr *> mrs;
    };

    int InitializeLocked();
    Region *FindRegionLocked(void *addr, size_t length);
    const Region *FindRegionLocked(void *addr, size_t length) const;

    mutable std::shared_mutex mutex_;
    bool initialization_attempted_{false};
    int initialization_result_{0};
    bool enabled_{false};
    AscendParameterPlaneDevice device_;
    std::map<uintptr_t, Region> regions_;
    std::map<ibv_pd *, size_t> pd_mr_limits_;
};

}  // namespace mooncake
