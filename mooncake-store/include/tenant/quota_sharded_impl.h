#pragma once

#include <algorithm>

#include "tenant/quota_sharded.h"

namespace mooncake {

template <size_t NumShards>
TenantQuotaResult ShardedTenantQuotaTable<NumShards>::UpsertTenantPolicy(
    const TenantId& tenant_id, const TenantQuotaLimits& policy,
    uint64_t memory_capacity_bytes, uint64_t nof_capacity_bytes) {
    std::lock_guard<std::mutex> recompute_lock(recompute_mutex_);
    auto& shard = GetShard(tenant_id);
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto result = shard.table.UpsertTenantPolicy(tenant_id, policy);
        if (!result) {
            return result;
        }
    }
    RecomputeMemoryEffectiveQuotasLocked(memory_capacity_bytes);
    RecomputeNoFEffectiveQuotasLocked(nof_capacity_bytes);
    return {};
}

template <size_t NumShards>
TenantQuotaResult
ShardedTenantQuotaTable<NumShards>::DisableTenantPolicyIfEmpty(
    const TenantId& tenant_id) {
    std::lock_guard<std::mutex> recompute_lock(recompute_mutex_);
    auto& shard = GetShard(tenant_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.table.DisableTenantPolicyIfEmpty(tenant_id);
}

template <size_t NumShards>
TenantQuotaResult ShardedTenantQuotaTable<NumShards>::ApplyTenantPolicies(
    const TenantQuotaPolicyMap& policies, uint64_t memory_capacity_bytes,
    uint64_t nof_capacity_bytes) {
    for (const auto& [_, policy] : policies) {
        if (policy.memory_quota_bytes == 0 ||
            policy.memory_quota_bytes > TenantQuotaAccount::kMaxChargedBytes ||
            policy.nof_quota_bytes > TenantQuotaAccount::kMaxChargedBytes) {
            return tl::make_unexpected(TenantQuotaError::kInvalidArgument);
        }
    }

    std::lock_guard<std::mutex> recompute_lock(recompute_mutex_);
    std::array<TenantQuotaPolicyMap, kNumShards> grouped_policies;
    for (const auto& [tenant_id, policy] : policies) {
        grouped_policies[GetShardIndex(tenant_id)].emplace(tenant_id, policy);
    }

    for (size_t i = 0; i < kNumShards; ++i) {
        auto& shard = shards_[i];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto result = shard.table.ApplyTenantPolicies(grouped_policies[i]);
        if (!result) {
            return result;
        }
    }
    RecomputeMemoryEffectiveQuotasLocked(memory_capacity_bytes);
    RecomputeNoFEffectiveQuotasLocked(nof_capacity_bytes);
    return {};
}

template <size_t NumShards>
TenantQuotaPolicyMap ShardedTenantQuotaTable<NumShards>::GetTenantPolicies()
    const {
    TenantQuotaPolicyMap policies;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto shard_policies = shard.table.GetTenantPolicies();
        policies.insert(shard_policies.begin(), shard_policies.end());
    }
    return policies;
}

template <size_t NumShards>
void ShardedTenantQuotaTable<NumShards>::RecomputeEffectiveQuotas(
    uint64_t memory_capacity_bytes, uint64_t nof_capacity_bytes) {
    std::lock_guard<std::mutex> recompute_lock(recompute_mutex_);
    RecomputeMemoryEffectiveQuotasLocked(memory_capacity_bytes);
    RecomputeNoFEffectiveQuotasLocked(nof_capacity_bytes);
}

template <size_t NumShards>
bool ShardedTenantQuotaTable<NumShards>::IsTenantRegistered(
    const TenantId& tenant_id) const {
    const auto& shard = GetShard(tenant_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.table.IsTenantRegistered(tenant_id);
}

template <size_t NumShards>
TenantQuotaHandle ShardedTenantQuotaTable<NumShards>::GetOrCreateTenantHandle(
    const TenantId& tenant_id) {
    auto& shard = GetShard(tenant_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.table.GetOrCreateTenantHandle(tenant_id);
}

template <size_t NumShards>
TenantQuotaHandle
ShardedTenantQuotaTable<NumShards>::GetOrCreateNoFTenantHandle(
    const TenantId& tenant_id) {
    auto& shard = GetShard(tenant_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.table.GetOrCreateNoFTenantHandle(tenant_id);
}

template <size_t NumShards>
std::optional<TenantQuotaSnapshot>
ShardedTenantQuotaTable<NumShards>::GetTenantSnapshot(
    const TenantId& tenant_id) const {
    const auto& shard = GetShard(tenant_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.table.GetTenantSnapshot(tenant_id);
}

template <size_t NumShards>
std::optional<TenantQuotaSnapshot>
ShardedTenantQuotaTable<NumShards>::GetNoFTenantSnapshot(
    const TenantId& tenant_id) const {
    const auto& shard = GetShard(tenant_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.table.GetNoFTenantSnapshot(tenant_id);
}

template <size_t NumShards>
std::vector<TenantQuotaSnapshot>
ShardedTenantQuotaTable<NumShards>::ListTenantSnapshots() const {
    std::vector<TenantQuotaSnapshot> snapshots;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto shard_snapshots = shard.table.ListTenantSnapshots();
        snapshots.insert(snapshots.end(), shard_snapshots.begin(),
                         shard_snapshots.end());
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.tenant_id < rhs.tenant_id;
              });
    return snapshots;
}

template <size_t NumShards>
std::vector<TenantQuotaSnapshot>
ShardedTenantQuotaTable<NumShards>::ListNoFTenantSnapshots() const {
    std::vector<TenantQuotaSnapshot> snapshots;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto shard_snapshots = shard.table.ListNoFTenantSnapshots();
        snapshots.insert(snapshots.end(), shard_snapshots.begin(),
                         shard_snapshots.end());
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.tenant_id < rhs.tenant_id;
              });
    return snapshots;
}

template <size_t NumShards>
TenantQuotaResult ShardedTenantQuotaTable<NumShards>::RebuildUsage(
    const TenantQuotaUsageMap& usage, uint64_t memory_capacity_bytes) {
    for (const auto& [_, charged_bytes] : usage) {
        if (charged_bytes > TenantQuotaAccount::kMaxChargedBytes) {
            return tl::make_unexpected(TenantQuotaError::kInvalidArgument);
        }
    }

    std::lock_guard<std::mutex> recompute_lock(recompute_mutex_);
    std::array<TenantQuotaUsageMap, kNumShards> grouped_usage;
    for (const auto& [tenant_id, charged_bytes] : usage) {
        grouped_usage[GetShardIndex(tenant_id)].emplace(tenant_id,
                                                        charged_bytes);
    }

    for (size_t i = 0; i < kNumShards; ++i) {
        auto& shard = shards_[i];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto result = shard.table.RebuildUsage(grouped_usage[i]);
        if (!result) {
            return result;
        }
    }
    RecomputeMemoryEffectiveQuotasLocked(memory_capacity_bytes);
    return {};
}

template <size_t NumShards>
TenantQuotaResult ShardedTenantQuotaTable<NumShards>::RebuildNoFUsage(
    const TenantQuotaUsageMap& usage, uint64_t nof_capacity_bytes) {
    for (const auto& [_, charged_bytes] : usage) {
        if (charged_bytes > TenantQuotaAccount::kMaxChargedBytes) {
            return tl::make_unexpected(TenantQuotaError::kInvalidArgument);
        }
    }

    std::lock_guard<std::mutex> recompute_lock(recompute_mutex_);
    std::array<TenantQuotaUsageMap, kNumShards> grouped_usage;
    for (const auto& [tenant_id, charged_bytes] : usage) {
        grouped_usage[GetShardIndex(tenant_id)].emplace(tenant_id,
                                                        charged_bytes);
    }

    for (size_t i = 0; i < kNumShards; ++i) {
        auto& shard = shards_[i];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto result = shard.table.RebuildNoFUsage(grouped_usage[i]);
        if (!result) {
            return result;
        }
    }
    RecomputeNoFEffectiveQuotasLocked(nof_capacity_bytes);
    return {};
}

template <size_t NumShards>
size_t ShardedTenantQuotaTable<NumShards>::GetShardIndex(
    const TenantId& tenant_id) const {
    return TenantIdHash{}(tenant_id) % kNumShards;
}

template <size_t NumShards>
typename ShardedTenantQuotaTable<NumShards>::Shard&
ShardedTenantQuotaTable<NumShards>::GetShard(const TenantId& tenant_id) {
    return shards_[GetShardIndex(tenant_id)];
}

template <size_t NumShards>
const typename ShardedTenantQuotaTable<NumShards>::Shard&
ShardedTenantQuotaTable<NumShards>::GetShard(const TenantId& tenant_id) const {
    return shards_[GetShardIndex(tenant_id)];
}

template <size_t NumShards>
void ShardedTenantQuotaTable<NumShards>::RecomputeMemoryEffectiveQuotasLocked(
    uint64_t memory_capacity_bytes) {
    std::vector<TenantQuotaSnapshot> snapshots;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto shard_snapshots = shard.table.ListTenantSnapshots();
        snapshots.insert(snapshots.end(), shard_snapshots.begin(),
                         shard_snapshots.end());
    }

    auto effective_quotas = TenantQuotaTable::BuildEffectiveQuotaAssignments(
        snapshots, memory_capacity_bytes);
    std::array<std::map<TenantId, uint64_t>, kNumShards> grouped_quotas;
    for (const auto& [tenant_id, effective_quota_bytes] : effective_quotas) {
        grouped_quotas[GetShardIndex(tenant_id)].emplace(tenant_id,
                                                         effective_quota_bytes);
    }

    for (size_t i = 0; i < kNumShards; ++i) {
        auto& shard = shards_[i];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.table.ApplyEffectiveQuotas(shard.table.accounts_,
                                         grouped_quotas[i]);
    }
}

template <size_t NumShards>
void ShardedTenantQuotaTable<NumShards>::RecomputeNoFEffectiveQuotasLocked(
    uint64_t nof_capacity_bytes) {
    std::vector<TenantQuotaSnapshot> snapshots;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto shard_snapshots = shard.table.ListNoFTenantSnapshots();
        snapshots.insert(snapshots.end(), shard_snapshots.begin(),
                         shard_snapshots.end());
    }

    auto effective_quotas = TenantQuotaTable::BuildEffectiveQuotaAssignments(
        snapshots, nof_capacity_bytes);
    std::array<std::map<TenantId, uint64_t>, kNumShards> grouped_quotas;
    for (const auto& [tenant_id, effective_quota_bytes] : effective_quotas) {
        grouped_quotas[GetShardIndex(tenant_id)].emplace(tenant_id,
                                                         effective_quota_bytes);
    }

    for (size_t i = 0; i < kNumShards; ++i) {
        auto& shard = shards_[i];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.table.ApplyEffectiveQuotas(shard.table.nof_accounts_,
                                         grouped_quotas[i]);
    }
}

}  // namespace mooncake
