// Copyright 2024 KVCache.AI
// Licensed under the Apache License, Version 2.0 (the "License");

#include "transport/rdma_transport/ascend_parameter_plane.h"

#include <glog/logging.h>
#include "config.h"
#include <json/json.h>

#include <arpa/inet.h>
#include <ifaddrs.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

#if defined(USE_ASCEND_NDR)
#include <acl/acl.h>

extern "C" int halMemRegUbSegment(uint32_t devid, uint64_t va, uint64_t size);
extern "C" int halMemUnRegUbSegment(uint32_t devid, uint64_t va);
#endif

namespace mooncake {
namespace {

bool RangeContains(uintptr_t base, size_t length, uintptr_t addr,
                   size_t request_length) {
    return addr >= base && request_length <= length &&
           addr - base <= length - request_length;
}

#if defined(USE_ASCEND_NDR)
bool RangesOverlap(uintptr_t a, size_t a_length, uintptr_t b, size_t b_length) {
    return a < b + b_length && b < a + a_length;
}

#endif

}  // namespace

AscendParameterPlane &AscendParameterPlane::GetInstance() {
    static AscendParameterPlane instance;
    return instance;
}

AscendParameterPlane::~AscendParameterPlane() { Shutdown(); }

int AscendParameterPlane::InitializeFromEnvironment() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!initialization_attempted_) {
        initialization_attempted_ = true;
        initialization_result_ = InitializeLocked();
    }
    return initialization_result_;
}

int AscendParameterPlane::InitializeLocked() {
    auto &config = AscendStorageConfig::GetInstance();
    const int rc = config.InitializeFromEnvironment();
    if (rc != 0) return rc;
    if (config.Mode() != AscendTransferMode::ParameterDirect) return 0;
    device_ = config.Device();
    enabled_ = true;
    return 0;
}

bool AscendParameterPlane::IsEnabled() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return enabled_;
}

const AscendParameterPlaneDevice &AscendParameterPlane::Device() const {
    return device_;
}

bool AscendParameterPlane::IsDeviceMemory(void *addr, int *device_id) const {
#if !defined(USE_ASCEND_NDR)
    (void)addr;
    (void)device_id;
    return false;
#else
    if (addr == nullptr) return false;
    aclrtPtrAttributes attributes{};
    const aclError rc = aclrtPointerGetAttributes(addr, &attributes);
    if (rc != ACL_SUCCESS ||
        attributes.location.type != ACL_MEM_LOCATION_TYPE_DEVICE) {
        return false;
    }
    if (device_id != nullptr) {
        *device_id = static_cast<int>(attributes.location.id);
    }
    return true;
#endif
}

AscendParameterPlane::Region *AscendParameterPlane::FindRegionLocked(
    void *addr, size_t length) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(addr);
    auto upper = regions_.upper_bound(address);
    if (upper == regions_.begin()) return nullptr;
    auto candidate = std::prev(upper);
    return RangeContains(candidate->first, candidate->second.length, address,
                         length)
               ? &candidate->second
               : nullptr;
}

const AscendParameterPlane::Region *AscendParameterPlane::FindRegionLocked(
    void *addr, size_t length) const {
    const uintptr_t address = reinterpret_cast<uintptr_t>(addr);
    auto upper = regions_.upper_bound(address);
    if (upper == regions_.begin()) return nullptr;
    auto candidate = std::prev(upper);
    return RangeContains(candidate->first, candidate->second.length, address,
                         length)
               ? &candidate->second
               : nullptr;
}

int AscendParameterPlane::RegisterRegion(void *addr, size_t length) {
#if !defined(USE_ASCEND_NDR)
    (void)addr;
    (void)length;
    return -ENOTSUP;
#else
    if (InitializeFromEnvironment() != 0 || !IsEnabled()) return -ENOTSUP;
    int pointer_device = -1;
    if (!IsDeviceMemory(addr, &pointer_device) || length == 0) return -EINVAL;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (pointer_device != device_.logical_device_id) return -EXDEV;
    const uintptr_t base = reinterpret_cast<uintptr_t>(addr);
    if (length > std::numeric_limits<uintptr_t>::max() - base) {
        return -EOVERFLOW;
    }
    for (auto &[other_base, region] : regions_) {
        if (!RangesOverlap(base, length, other_base, region.length)) continue;
        if (base == other_base && length == region.length) {
            ++region.references;
            return 0;
        }
        return -EADDRINUSE;
    }

    const int rc = halMemRegUbSegment(
        static_cast<uint32_t>(device_.logical_device_id),
        static_cast<uint64_t>(base), static_cast<uint64_t>(length));
    if (rc != 0) {
        LOG(ERROR) << "halMemRegUbSegment failed: rc=" << rc
                   << ", addr=" << addr << ", length=" << length;
        return -EIO;
    }
    Region region;
    region.base = addr;
    region.length = length;
    region.references = 1;
    regions_.emplace(base, std::move(region));
    return 0;
#endif
}

int AscendParameterPlane::UnregisterRegion(void *addr) {
#if !defined(USE_ASCEND_NDR)
    (void)addr;
    return -ENOTSUP;
#else
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = regions_.find(reinterpret_cast<uintptr_t>(addr));
    if (it == regions_.end()) return -ENOENT;
    if (--it->second.references != 0) return 0;

    bool deregistration_failed = false;
    for (auto mr_it = it->second.mrs.begin(); mr_it != it->second.mrs.end();) {
        if (ibv_dereg_mr(mr_it->second) != 0) {
            deregistration_failed = true;
            ++mr_it;
        } else {
            mr_it = it->second.mrs.erase(mr_it);
        }
    }
    if (deregistration_failed) {
        ++it->second.references;
        return -EIO;
    }
    const int rc =
        halMemUnRegUbSegment(static_cast<uint32_t>(device_.logical_device_id),
                             static_cast<uint64_t>(it->first));
    if (rc != 0) {
        ++it->second.references;
        return -EIO;
    }
    regions_.erase(it);
    return 0;
#endif
}

bool AscendParameterPlane::Contains(void *addr, size_t length) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return FindRegionLocked(addr, length) != nullptr;
}

bool AscendParameterPlane::GetMrRange(uintptr_t base, size_t capacity,
                                      uintptr_t address, size_t length,
                                      size_t limit, uintptr_t &mr_base,
                                      size_t &mr_length) {
    if (!limit || !length || length > limit ||
        !RangeContains(base, capacity, address, length))
        return false;
    const size_t offset = address - base;
    const size_t window = (offset / limit) * limit;
    mr_base = base + window;
    mr_length = std::min(limit, capacity - window);
    if (length > mr_length || offset - window > mr_length - length) {
        // A callback can cross a window boundary. Register that requested
        // span separately; never return an MR that covers only part of it.
        mr_base = address;
        mr_length = length;
    }
    return true;
}

int AscendParameterPlane::GetOrCreateMr(struct ibv_pd *pd, void *addr,
                                        size_t length, int access,
                                        struct ibv_mr **mr) {
#if !defined(USE_ASCEND_NDR)
    (void)pd;
    (void)addr;
    (void)length;
    (void)access;
    (void)mr;
    return -ENOTSUP;
#else
    if (pd == nullptr || mr == nullptr || length == 0) return -EINVAL;
    auto make_key = [&](const Region &region, size_t limit) {
        uintptr_t base = 0;
        size_t size = 0;
        GetMrRange(reinterpret_cast<uintptr_t>(region.base), region.length,
                   reinterpret_cast<uintptr_t>(addr), length, limit, base,
                   size);
        return Region::MrKey{pd, base, size, access};
    };
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const Region *region = FindRegionLocked(addr, length);
        if (!region) return -ENOENT;
        auto limit = pd_mr_limits_.find(pd);
        if (limit != pd_mr_limits_.end()) {
            auto existing = region->mrs.find(make_key(*region, limit->second));
            if (existing != region->mrs.end()) {
                *mr = existing->second;
                return 0;
            }
        }
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Region *region = FindRegionLocked(addr, length);
    if (!region) return -ENOENT;
    auto limit = pd_mr_limits_.find(pd);
    if (limit == pd_mr_limits_.end()) {
        ibv_device_attr attributes{};
        if (ibv_query_device(pd->context, &attributes) != 0) return -EIO;
        size_t max_length = std::min<uint64_t>(attributes.max_mr_size,
                                               globalConfig().max_mr_size);
        if (!max_length) return -EINVAL;
        limit = pd_mr_limits_.emplace(pd, max_length).first;
    }
    auto key = make_key(*region, limit->second);
    if (std::get<2>(key) == 0) return -E2BIG;
    auto existing = region->mrs.find(key);
    if (existing != region->mrs.end()) {
        *mr = existing->second;
        return 0;
    }
    auto *new_mr = ibv_reg_mr(pd, reinterpret_cast<void *>(std::get<1>(key)),
                              std::get<2>(key), access);
    if (!new_mr) return errno ? -errno : -EIO;
    region->mrs.emplace(key, new_mr);
    *mr = new_mr;
    return 0;
#endif
}

void AscendParameterPlane::InvalidatePd(struct ibv_pd *pd) {
#if defined(USE_ASCEND_NDR)
    if (pd == nullptr) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    pd_mr_limits_.erase(pd);
    for (auto &[_, region] : regions_) {
        for (auto it = region.mrs.begin(); it != region.mrs.end();) {
            if (std::get<0>(it->first) != pd) {
                ++it;
                continue;
            }
            if (ibv_dereg_mr(it->second) != 0) {
                ++it;
                continue;
            }
            it = region.mrs.erase(it);
        }
    }
#else
    (void)pd;
#endif
}

void AscendParameterPlane::Shutdown() {
#if defined(USE_ASCEND_NDR)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto &[base, region] : regions_) {
        for (auto &[pd, mr] : region.mrs) {
            (void)pd;
            (void)ibv_dereg_mr(mr);
        }
        (void)halMemUnRegUbSegment(
            static_cast<uint32_t>(device_.logical_device_id),
            static_cast<uint64_t>(base));
    }
    regions_.clear();
    pd_mr_limits_.clear();
    enabled_ = false;
#endif
}

}  // namespace mooncake
