#include "tenant/quota.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace mooncake {
namespace {

struct RemainderShare {
    TenantId tenant_id;
    uint64_t base = 0;
    unsigned __int128 remainder = 0;
};

TenantQuotaResult AccountingMismatch() {
    return tl::make_unexpected(TenantQuotaError::kAccountingMismatch);
}

TenantQuotaChargeResult ChargeFailure(TenantQuotaError error,
                                      uint64_t deficit_bytes = 0) {
    return tl::make_unexpected(TenantQuotaChargeFailure{
        .error = error,
        .deficit_bytes = deficit_bytes,
    });
}

std::map<TenantId, uint64_t> BuildEffectiveQuotaAssignmentsImpl(
    const std::vector<TenantQuotaSnapshot>& tenants,
    uint64_t allocatable_capacity_bytes) {
    unsigned __int128 explicit_requested_sum = 0;
    std::vector<std::pair<TenantId, uint64_t>> explicit_tenants;
    std::map<TenantId, uint64_t> assigned;

    for (const auto& snapshot : tenants) {
        const TenantId& tenant_id = snapshot.tenant_id;
        assigned.emplace(tenant_id, 0);
        if (snapshot.has_explicit_policy) {
            explicit_requested_sum += snapshot.requested_quota_bytes;
            explicit_tenants.emplace_back(tenant_id,
                                          snapshot.requested_quota_bytes);
        }
    }

    if (explicit_requested_sum <= allocatable_capacity_bytes) {
        for (const auto& [tenant_id, requested_quota_bytes] :
             explicit_tenants) {
            assigned[tenant_id] = requested_quota_bytes;
        }
        return assigned;
    }

    std::vector<RemainderShare> shares;
    shares.reserve(explicit_tenants.size());
    uint64_t base_assigned = 0;
    for (const auto& [tenant_id, requested_quota_bytes] : explicit_tenants) {
        const unsigned __int128 product =
            static_cast<unsigned __int128>(allocatable_capacity_bytes) *
            requested_quota_bytes;
        const uint64_t base =
            static_cast<uint64_t>(product / explicit_requested_sum);
        shares.push_back({.tenant_id = tenant_id,
                          .base = base,
                          .remainder = product % explicit_requested_sum});
        base_assigned += base;
    }

    std::sort(shares.begin(), shares.end(),
              [](const RemainderShare& lhs, const RemainderShare& rhs) {
                  if (lhs.remainder != rhs.remainder) {
                      return lhs.remainder > rhs.remainder;
                  }
                  return lhs.tenant_id < rhs.tenant_id;
              });

    uint64_t remaining = allocatable_capacity_bytes - base_assigned;
    for (auto& share : shares) {
        if (remaining > 0) {
            ++share.base;
            --remaining;
        }
        assigned[share.tenant_id] = share.base;
    }
    return assigned;
}

}  // namespace

TenantQuotaChargeResult TenantQuotaAccount::TryCharge(uint64_t bytes) {
    if (bytes > kMaxChargedBytes) {
        return ChargeFailure(TenantQuotaError::kInvalidArgument);
    }

    // A tier without an explicit policy (the NoF tier before a NoF quota is
    // configured) only tracks usage: charge the bytes and admit the write.
    if (!EnforceQuota()) {
        ChargeUnchecked(bytes);
        return {};
    }

    for (;;) {
        const uint64_t sequence_before =
            policy_sequence_.load(std::memory_order_acquire);
        if (sequence_before & 1) {
            continue;
        }

        uint64_t expected = charged_state_.load(std::memory_order_acquire);
        if (expected & kAdmissionClosed) {
            const uint64_t sequence_after =
                policy_sequence_.load(std::memory_order_acquire);
            if (sequence_before == sequence_after && !(sequence_after & 1)) {
                return ChargeFailure(TenantQuotaError::kTenantNotRegistered);
            }
            continue;
        }

        const uint64_t charged_bytes = expected & kChargedBytesMask;
        const uint64_t effective_quota_bytes =
            effective_quota_bytes_.load(std::memory_order_acquire);
        if (bytes != 0 && (charged_bytes > effective_quota_bytes ||
                           bytes > effective_quota_bytes - charged_bytes)) {
            const uint64_t sequence_after =
                policy_sequence_.load(std::memory_order_acquire);
            if (sequence_before == sequence_after && !(sequence_after & 1)) {
                const unsigned __int128 demand =
                    static_cast<unsigned __int128>(charged_bytes) + bytes;
                const unsigned __int128 deficit =
                    demand - effective_quota_bytes;
                return ChargeFailure(
                    TenantQuotaError::kQuotaExceeded,
                    deficit > std::numeric_limits<uint64_t>::max()
                        ? std::numeric_limits<uint64_t>::max()
                        : static_cast<uint64_t>(deficit));
            }
            continue;
        }

        if (bytes != 0) {
            const uint64_t desired = charged_bytes + bytes;
            if (!charged_state_.compare_exchange_weak(
                    expected, desired, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                continue;
            }
        }

        const uint64_t state_after =
            charged_state_.load(std::memory_order_acquire);
        const uint64_t sequence_after =
            policy_sequence_.load(std::memory_order_acquire);
        if (sequence_before == sequence_after && !(sequence_after & 1) &&
            !(state_after & kAdmissionClosed)) {
            return {};
        }

        if (bytes != 0) {
            auto release_result = Release(bytes);
            if (!release_result) {
                return ChargeFailure(release_result.error());
            }
        }
    }
}

void TenantQuotaAccount::ChargeUnchecked(uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    uint64_t expected = charged_state_.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t charged_bytes = expected & kChargedBytesMask;
        const uint64_t next = bytes > kMaxChargedBytes - charged_bytes
                                  ? kMaxChargedBytes
                                  : charged_bytes + bytes;
        const uint64_t desired = (expected & kAdmissionClosed) | next;
        if (charged_state_.compare_exchange_weak(expected, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            return;
        }
    }
}

TenantQuotaResult TenantQuotaAccount::Release(uint64_t bytes) {
    if (bytes == 0) {
        return {};
    }

    uint64_t expected = charged_state_.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t charged_bytes = expected & kChargedBytesMask;
        if (bytes > charged_bytes) {
            return AccountingMismatch();
        }

        const uint64_t desired =
            (expected & kAdmissionClosed) | (charged_bytes - bytes);
        if (charged_state_.compare_exchange_weak(expected, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            return {};
        }
    }
}

uint64_t TenantQuotaAccount::ChargedBytes() const {
    return charged_state_.load(std::memory_order_acquire) & kChargedBytesMask;
}

uint64_t TenantQuotaAccount::EffectiveQuotaBytes() const {
    return effective_quota_bytes_.load(std::memory_order_acquire);
}

bool TenantQuotaAccount::AdmissionClosed() const {
    return charged_state_.load(std::memory_order_acquire) & kAdmissionClosed;
}

void TenantQuotaAccount::BeginPolicyUpdate() {
    const uint64_t previous =
        policy_sequence_.fetch_add(1, std::memory_order_acq_rel);
    assert((previous & 1) == 0);
    (void)previous;
}

void TenantQuotaAccount::EndPolicyUpdate() {
    const uint64_t previous =
        policy_sequence_.fetch_add(1, std::memory_order_release);
    assert((previous & 1) != 0);
    (void)previous;
}

void TenantQuotaAccount::SetAdmissionClosed(bool closed) {
    uint64_t expected = charged_state_.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t desired =
            closed ? expected | kAdmissionClosed : expected & kChargedBytesMask;
        if (charged_state_.compare_exchange_weak(expected, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            return;
        }
    }
}

void TenantQuotaAccount::ApplyEffectiveQuota(uint64_t effective_quota_bytes) {
    effective_quota_bytes_.store(effective_quota_bytes,
                                 std::memory_order_release);
    SetAdmissionClosed(!has_explicit_policy_ && EnforceQuota());
}

void TenantQuotaAccount::SetEnforceQuota(bool enforce) {
    enforce_quota_.store(enforce, std::memory_order_release);
}

bool TenantQuotaAccount::EnforceQuota() const {
    return enforce_quota_.load(std::memory_order_acquire);
}

void TenantQuotaAccount::SetChargedBytesForRebuild(uint64_t charged_bytes) {
    assert(charged_bytes <= kMaxChargedBytes);
    charged_state_.store(charged_bytes | kAdmissionClosed,
                         std::memory_order_release);
}

std::map<TenantId, uint64_t> TenantQuotaTable::BuildEffectiveQuotaAssignments(
    const std::vector<TenantQuotaSnapshot>& tenants,
    uint64_t allocatable_capacity_bytes) {
    return BuildEffectiveQuotaAssignmentsImpl(tenants,
                                              allocatable_capacity_bytes);
}

TenantQuotaResult TenantQuotaTable::UpsertTenantPolicy(
    const TenantId& tenant_id, const TenantQuotaLimits& policy) {
    if (policy.memory_quota_bytes == 0 ||
        policy.memory_quota_bytes > TenantQuotaAccount::kMaxChargedBytes ||
        policy.nof_quota_bytes > TenantQuotaAccount::kMaxChargedBytes) {
        return tl::make_unexpected(TenantQuotaError::kInvalidArgument);
    }

    // Memory tier is authoritative for registration.
    auto& memory_account = GetOrCreateAccount(accounts_, tenant_id, true);
    memory_account.BeginPolicyUpdate();
    memory_account.SetAdmissionClosed(true);
    memory_account.requested_quota_bytes_ = policy.memory_quota_bytes;
    memory_account.has_explicit_policy_ = true;
    memory_account.EndPolicyUpdate();

    // NoF tier is opt-in: a zero quota clears any pre-existing NoF policy and
    // returns the tier to tracking-only, so NoF writes are admitted again.
    if (policy.nof_quota_bytes == 0) {
        auto nof_it = nof_accounts_.find(tenant_id);
        if (nof_it != nof_accounts_.end()) {
            auto& nof_account = *nof_it->second;
            nof_account.BeginPolicyUpdate();
            nof_account.SetAdmissionClosed(true);
            nof_account.SetEnforceQuota(false);
            nof_account.requested_quota_bytes_ = 0;
            nof_account.effective_quota_bytes_.store(0,
                                                     std::memory_order_release);
            nof_account.has_explicit_policy_ = false;
            nof_account.EndPolicyUpdate();
        }
    } else {
        auto& nof_account = GetOrCreateAccount(nof_accounts_, tenant_id, true);
        nof_account.BeginPolicyUpdate();
        nof_account.SetAdmissionClosed(true);
        nof_account.SetEnforceQuota(true);
        nof_account.requested_quota_bytes_ = policy.nof_quota_bytes;
        nof_account.has_explicit_policy_ = true;
        nof_account.EndPolicyUpdate();
    }
    return {};
}

TenantQuotaResult TenantQuotaTable::DisableTenantPolicyIfEmpty(
    const TenantId& tenant_id) {
    // Each entry pairs the account with the enforcement to leave it in once its
    // policy is gone: the memory tier keeps enforcing, the NoF tier does not.
    std::vector<std::pair<TenantQuotaAccount*, bool>> accounts;
    auto it = accounts_.find(tenant_id);
    if (it != accounts_.end() && it->second->has_explicit_policy_) {
        accounts.emplace_back(it->second.get(), true);
    }
    auto nof_it = nof_accounts_.find(tenant_id);
    if (nof_it != nof_accounts_.end() && nof_it->second->has_explicit_policy_) {
        accounts.emplace_back(nof_it->second.get(), false);
    }
    if (accounts.empty()) {
        return tl::make_unexpected(TenantQuotaError::kTenantNotFound);
    }

    for (auto& [account, _] : accounts) {
        account->BeginPolicyUpdate();
        account->SetAdmissionClosed(true);
    }
    const bool any_nonempty = std::any_of(
        accounts.begin(), accounts.end(),
        [](const auto& entry) { return entry.first->ChargedBytes() != 0; });
    if (any_nonempty) {
        for (auto& [account, _] : accounts) {
            account->SetAdmissionClosed(false);
            account->EndPolicyUpdate();
        }
        return tl::make_unexpected(TenantQuotaError::kTenantNotEmpty);
    }

    for (auto& [account, enforce_after] : accounts) {
        account->requested_quota_bytes_ = 0;
        account->effective_quota_bytes_.store(0, std::memory_order_release);
        account->has_explicit_policy_ = false;
        account->SetEnforceQuota(enforce_after);
        account->EndPolicyUpdate();
    }
    return {};
}

TenantQuotaResult TenantQuotaTable::ApplyTenantPolicies(
    const TenantQuotaPolicyMap& policies) {
    for (const auto& [_, policy] : policies) {
        if (policy.memory_quota_bytes == 0 ||
            policy.memory_quota_bytes > TenantQuotaAccount::kMaxChargedBytes ||
            policy.nof_quota_bytes > TenantQuotaAccount::kMaxChargedBytes) {
            return tl::make_unexpected(TenantQuotaError::kInvalidArgument);
        }
    }

    std::map<TenantId, uint64_t> memory_quotas;
    std::map<TenantId, uint64_t> nof_quotas;
    for (const auto& [tenant_id, policy] : policies) {
        memory_quotas.emplace(tenant_id, policy.memory_quota_bytes);
        if (policy.nof_quota_bytes != 0) {
            nof_quotas.emplace(tenant_id, policy.nof_quota_bytes);
        }
    }
    ApplySingleTierPolicies(accounts_, memory_quotas, true);
    ApplySingleTierPolicies(nof_accounts_, nof_quotas, false);
    return {};
}

void TenantQuotaTable::ApplySingleTierPolicies(
    AccountMap& accounts, const std::map<TenantId, uint64_t>& tier_quotas,
    bool enforce_without_policy) {
    for (auto& [tenant_id, account_ptr] : accounts) {
        auto& account = *account_ptr;
        auto quota_it = tier_quotas.find(tenant_id);
        account.BeginPolicyUpdate();
        account.SetAdmissionClosed(true);
        if (quota_it == tier_quotas.end()) {
            account.SetEnforceQuota(enforce_without_policy);
            account.requested_quota_bytes_ = 0;
            account.effective_quota_bytes_.store(0, std::memory_order_release);
            account.has_explicit_policy_ = false;
        } else {
            account.SetEnforceQuota(true);
            account.requested_quota_bytes_ = quota_it->second;
            account.has_explicit_policy_ = true;
        }
        account.EndPolicyUpdate();
    }

    for (const auto& [tenant_id, quota_bytes] : tier_quotas) {
        auto& account = GetOrCreateAccount(accounts, tenant_id, true);
        if (account.has_explicit_policy_) {
            continue;
        }
        account.BeginPolicyUpdate();
        account.SetAdmissionClosed(true);
        account.SetEnforceQuota(true);
        account.requested_quota_bytes_ = quota_bytes;
        account.has_explicit_policy_ = true;
        account.EndPolicyUpdate();
    }
}

TenantQuotaPolicyMap TenantQuotaTable::GetTenantPolicies() const {
    TenantQuotaPolicyMap policies;
    for (const auto& [tenant_id, account] : accounts_) {
        if (account->has_explicit_policy_) {
            policies[tenant_id].memory_quota_bytes =
                account->requested_quota_bytes_;
        }
    }
    for (const auto& [tenant_id, account] : nof_accounts_) {
        if (account->has_explicit_policy_) {
            policies[tenant_id].nof_quota_bytes =
                account->requested_quota_bytes_;
        }
    }
    return policies;
}

void TenantQuotaTable::RecomputeEffectiveQuotas(uint64_t memory_capacity_bytes,
                                                uint64_t nof_capacity_bytes) {
    ApplyEffectiveQuotas(accounts_,
                         BuildEffectiveQuotaAssignments(ListTenantSnapshots(),
                                                        memory_capacity_bytes));
    ApplyEffectiveQuotas(
        nof_accounts_, BuildEffectiveQuotaAssignments(ListNoFTenantSnapshots(),
                                                      nof_capacity_bytes));
}

bool TenantQuotaTable::IsTenantRegistered(const TenantId& tenant_id) const {
    auto it = accounts_.find(tenant_id);
    if (it != accounts_.end() && it->second->has_explicit_policy_) {
        return true;
    }
    auto nof_it = nof_accounts_.find(tenant_id);
    return nof_it != nof_accounts_.end() &&
           nof_it->second->has_explicit_policy_;
}

TenantQuotaHandle TenantQuotaTable::GetOrCreateTenantHandle(
    const TenantId& tenant_id) {
    return &GetOrCreateAccount(accounts_, tenant_id, true);
}

TenantQuotaHandle TenantQuotaTable::GetOrCreateNoFTenantHandle(
    const TenantId& tenant_id) {
    // A tenant that never configured a NoF quota keeps tracking-only NoF
    // accounting: its writes are admitted until a NoF policy is set.
    return &GetOrCreateAccount(nof_accounts_, tenant_id, false);
}

std::optional<TenantQuotaSnapshot> TenantQuotaTable::GetTenantSnapshot(
    const TenantId& tenant_id) const {
    return GetSnapshot(accounts_, tenant_id);
}

std::optional<TenantQuotaSnapshot> TenantQuotaTable::GetNoFTenantSnapshot(
    const TenantId& tenant_id) const {
    return GetSnapshot(nof_accounts_, tenant_id);
}

std::vector<TenantQuotaSnapshot> TenantQuotaTable::ListTenantSnapshots() const {
    return ListSnapshots(accounts_);
}

std::vector<TenantQuotaSnapshot> TenantQuotaTable::ListNoFTenantSnapshots()
    const {
    return ListSnapshots(nof_accounts_);
}

std::optional<TenantQuotaSnapshot> TenantQuotaTable::GetSnapshot(
    const AccountMap& accounts, const TenantId& tenant_id) const {
    auto it = accounts.find(tenant_id);
    if (it == accounts.end() || (!it->second->has_explicit_policy_ &&
                                 it->second->ChargedBytes() == 0)) {
        return std::nullopt;
    }
    return MakeSnapshot(it->first, *it->second);
}

std::vector<TenantQuotaSnapshot> TenantQuotaTable::ListSnapshots(
    const AccountMap& accounts) const {
    std::vector<TenantQuotaSnapshot> snapshots;
    snapshots.reserve(accounts.size());
    for (const auto& [tenant_id, account] : accounts) {
        if (!account->has_explicit_policy_ && account->ChargedBytes() == 0) {
            continue;
        }
        snapshots.push_back(MakeSnapshot(tenant_id, *account));
    }
    return snapshots;
}

TenantQuotaResult TenantQuotaTable::RebuildUsage(
    const TenantQuotaUsageMap& usage) {
    return RebuildUsageInto(accounts_, usage, true);
}

TenantQuotaResult TenantQuotaTable::RebuildNoFUsage(
    const TenantQuotaUsageMap& usage) {
    return RebuildUsageInto(nof_accounts_, usage, false);
}

TenantQuotaResult TenantQuotaTable::RebuildUsageInto(
    AccountMap& accounts, const TenantQuotaUsageMap& usage,
    bool enforce_quota) {
    for (const auto& [_, charged_bytes] : usage) {
        if (charged_bytes > TenantQuotaAccount::kMaxChargedBytes) {
            return tl::make_unexpected(TenantQuotaError::kInvalidArgument);
        }
    }

    for (auto& [_, account] : accounts) {
        account->BeginPolicyUpdate();
        account->SetChargedBytesForRebuild(0);
        account->EndPolicyUpdate();
    }

    for (const auto& [tenant_id, charged_bytes] : usage) {
        auto& account = GetOrCreateAccount(accounts, tenant_id, enforce_quota);
        if (!account.has_explicit_policy_) {
            account.requested_quota_bytes_ = 0;
            account.effective_quota_bytes_.store(0, std::memory_order_release);
        }
        account.BeginPolicyUpdate();
        account.SetChargedBytesForRebuild(charged_bytes);
        account.EndPolicyUpdate();
    }
    return {};
}

TenantQuotaAccount& TenantQuotaTable::GetOrCreateAccount(
    AccountMap& accounts, const TenantId& tenant_id, bool enforce_quota) {
    auto [it, inserted] = accounts.try_emplace(tenant_id);
    if (inserted) {
        it->second = std::make_unique<TenantQuotaAccount>();
        it->second->SetEnforceQuota(enforce_quota);
        if (!enforce_quota) {
            // Tracking-only accounts are never gated on admission.
            it->second->SetAdmissionClosed(false);
        }
    }
    return *it->second;
}

TenantQuotaSnapshot TenantQuotaTable::MakeSnapshot(
    const TenantId& tenant_id, const TenantQuotaAccount& account) const {
    const uint64_t charged_bytes = account.ChargedBytes();
    const uint64_t effective_quota_bytes = account.EffectiveQuotaBytes();
    return TenantQuotaSnapshot{
        .tenant_id = tenant_id,
        .requested_quota_bytes = account.requested_quota_bytes_,
        .effective_quota_bytes = effective_quota_bytes,
        .charged_bytes = charged_bytes,
        .admission_closed = account.AdmissionClosed(),
        .has_explicit_policy = account.has_explicit_policy_,
        .over_quota =
            account.EnforceQuota() && charged_bytes > effective_quota_bytes,
    };
}

void TenantQuotaTable::ApplyEffectiveQuotas(
    AccountMap& accounts,
    const std::map<TenantId, uint64_t>& effective_quotas) {
    for (auto& [tenant_id, account] : accounts) {
        auto it = effective_quotas.find(tenant_id);
        const uint64_t effective_quota_bytes =
            it == effective_quotas.end() ? 0 : it->second;
        account->BeginPolicyUpdate();
        account->ApplyEffectiveQuota(effective_quota_bytes);
        account->EndPolicyUpdate();
    }
}

}  // namespace mooncake
