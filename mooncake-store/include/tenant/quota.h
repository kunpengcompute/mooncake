#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tenant_id.h"

#include <ylt/util/tl/expected.hpp>

namespace mooncake {

struct TenantQuotaSnapshot {
    TenantId tenant_id;
    uint64_t requested_quota_bytes = 0;
    uint64_t effective_quota_bytes = 0;
    uint64_t charged_bytes = 0;
    bool admission_closed = true;
    bool has_explicit_policy = false;
    bool over_quota = false;
};

// Dual-tier tenant quota policy: one memory quota plus an optional NoF quota.
// nof_quota_bytes == 0 means "NoF quota not configured" (memory tier remains
// authoritative for registration; NoF tier is opt-in: without a NoF quota its
// writes are admitted and only their usage is tracked).
struct TenantQuotaLimits {
    uint64_t memory_quota_bytes{0};
    uint64_t nof_quota_bytes{0};

    bool operator==(const TenantQuotaLimits& other) const {
        return memory_quota_bytes == other.memory_quota_bytes &&
               nof_quota_bytes == other.nof_quota_bytes;
    }
};

using TenantQuotaPolicyMap = std::map<TenantId, TenantQuotaLimits>;
using TenantQuotaUsageMap =
    std::unordered_map<TenantId, uint64_t, TenantIdHash>;

enum class TenantQuotaError {
    kQuotaExceeded,
    kInvalidArgument,
    kAccountingMismatch,
    kTenantNotRegistered,
    kTenantNotEmpty,
    kTenantNotFound,
};

struct TenantQuotaChargeFailure {
    TenantQuotaError error;
    uint64_t deficit_bytes = 0;
};

using TenantQuotaResult = tl::expected<void, TenantQuotaError>;
using TenantQuotaChargeResult = tl::expected<void, TenantQuotaChargeFailure>;

class TenantQuotaAccount {
   public:
    static constexpr uint64_t kAdmissionClosed = 1ULL << 63;
    static constexpr uint64_t kChargedBytesMask = kAdmissionClosed - 1;
    static constexpr uint64_t kMaxChargedBytes = kChargedBytesMask;

    TenantQuotaChargeResult TryCharge(uint64_t bytes);
    // Charges `bytes` whatever the quota and the admission state, saturating
    // at kMaxChargedBytes: for memory already in use that was not charged up
    // front.
    void ChargeUnchecked(uint64_t bytes);
    TenantQuotaResult Release(uint64_t bytes);

    uint64_t ChargedBytes() const;
    uint64_t EffectiveQuotaBytes() const;
    bool AdmissionClosed() const;

   private:
    friend class TenantQuotaTable;

    void BeginPolicyUpdate();
    void EndPolicyUpdate();
    void SetAdmissionClosed(bool closed);
    void ApplyEffectiveQuota(uint64_t effective_quota_bytes);
    void SetChargedBytesForRebuild(uint64_t charged_bytes);
    void SetEnforceQuota(bool enforce);
    bool EnforceQuota() const;

    // Bit 63 closes admission; bits 0..62 contain charged bytes.
    alignas(64) std::atomic<uint64_t> charged_state_{kAdmissionClosed};

    // Keep control-plane writes off the charged-state cache line.
    alignas(64) std::atomic<uint64_t> effective_quota_bytes_{0};
    std::atomic<uint64_t> policy_sequence_{0};

    // Whether charges are gated by admission and quota. The memory tier always
    // is; the NoF tier only while it has an explicit policy, so NoF writes pass
    // while no NoF quota is configured.
    std::atomic<bool> enforce_quota_{true};

    // Accessed only under external control-plane synchronization.
    uint64_t requested_quota_bytes_{0};
    bool has_explicit_policy_{false};
};

using TenantQuotaHandle = TenantQuotaAccount*;

template <size_t NumShards>
class ShardedTenantQuotaTable;

// Control-plane registry for stable quota accounts. Callers must provide
// external synchronization. Charge and release bypass this table and operate
// directly on TenantQuotaHandle.
class TenantQuotaTable {
   public:
    TenantQuotaResult UpsertTenantPolicy(const TenantId& tenant_id,
                                         const TenantQuotaLimits& policy);
    TenantQuotaResult DisableTenantPolicyIfEmpty(const TenantId& tenant_id);
    TenantQuotaResult ApplyTenantPolicies(const TenantQuotaPolicyMap& policies);
    TenantQuotaPolicyMap GetTenantPolicies() const;

    void RecomputeEffectiveQuotas(uint64_t memory_capacity_bytes,
                                  uint64_t nof_capacity_bytes);

    bool IsTenantRegistered(const TenantId& tenant_id) const;
    // May create a stable closed tombstone for a previously unseen tenant.
    TenantQuotaHandle GetOrCreateTenantHandle(const TenantId& tenant_id);
    std::optional<TenantQuotaSnapshot> GetTenantSnapshot(
        const TenantId& tenant_id) const;
    std::vector<TenantQuotaSnapshot> ListTenantSnapshots() const;

    // NoF tier symmetric entry points: same TenantQuotaAccount /
    // TenantQuotaSnapshot semantics, backed by nof_accounts_ instead of
    // accounts_. Tier difference is carried by which map is addressed.
    TenantQuotaHandle GetOrCreateNoFTenantHandle(const TenantId& tenant_id);
    std::optional<TenantQuotaSnapshot> GetNoFTenantSnapshot(
        const TenantId& tenant_id) const;
    std::vector<TenantQuotaSnapshot> ListNoFTenantSnapshots() const;

    // Rebuild overwrites runtime accounting and must only run while data-plane
    // charge/release operations are quiescent.
    TenantQuotaResult RebuildUsage(const TenantQuotaUsageMap& usage);
    TenantQuotaResult RebuildNoFUsage(const TenantQuotaUsageMap& usage);

   private:
    template <size_t>
    friend class ShardedTenantQuotaTable;

    using AccountMap = std::map<TenantId, std::unique_ptr<TenantQuotaAccount>>;

    TenantQuotaAccount& GetOrCreateAccount(AccountMap& accounts,
                                           const TenantId& tenant_id,
                                           bool enforce_quota);
    TenantQuotaSnapshot MakeSnapshot(const TenantId& tenant_id,
                                     const TenantQuotaAccount& account) const;
    std::vector<TenantQuotaSnapshot> ListSnapshots(
        const AccountMap& accounts) const;
    std::optional<TenantQuotaSnapshot> GetSnapshot(
        const AccountMap& accounts, const TenantId& tenant_id) const;
    TenantQuotaResult RebuildUsageInto(AccountMap& accounts,
                                       const TenantQuotaUsageMap& usage,
                                       bool enforce_quota);
    // Apply one tier's non-zero quota map to the given account table. The map
    // contains only tenants that should have an explicit policy for this tier;
    // tenants absent from the map are cleared. `enforce_without_policy` is true
    // for the memory tier, whose accounts gate admission even unregistered, and
    // false for the NoF tier, which admits writes until a policy is set.
    void ApplySingleTierPolicies(
        AccountMap& accounts, const std::map<TenantId, uint64_t>& tier_quotas,
        bool enforce_without_policy);
    static std::map<TenantId, uint64_t> BuildEffectiveQuotaAssignments(
        const std::vector<TenantQuotaSnapshot>& tenants,
        uint64_t allocatable_capacity_bytes);
    void ApplyEffectiveQuotas(
        AccountMap& accounts,
        const std::map<TenantId, uint64_t>& effective_quotas);

    AccountMap accounts_;
    AccountMap nof_accounts_;
};

}  // namespace mooncake
