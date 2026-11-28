#include "nof/page_registry.h"

#include <glog/logging.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <vector>

namespace mooncake {

namespace {
constexpr uintptr_t kPageMask = kNofRegistrationPageSize - 1;
uintptr_t AlignUpToPage(uintptr_t v) { return (v + kPageMask) & ~kPageMask; }
}  // namespace

NofPageRegistry::NofPageRegistry(MemRegisterFn register_fn,
                                 MemUnregisterFn unregister_fn)
    : register_fn_(register_fn), unregister_fn_(unregister_fn) {}

ErrorCode NofPageRegistry::Register(uint64_t owner, void* ptr, size_t size) {
    return RegisterPool(owner, ptr, size, kPageSize);
}

NofPageRegistry::SpanMap::iterator NofPageRegistry::SpanInteriorLocked(
    uintptr_t address) {
    auto next = spans_.lower_bound(address);
    if (next == spans_.begin()) return spans_.end();
    auto prev = std::prev(next);
    return address - prev->first < prev->second.length ? prev : spans_.end();
}

ErrorCode NofPageRegistry::RegisterPool(uint64_t owner, void* ptr, size_t size,
                                        size_t max_span, size_t* new_spans) {
    if (new_spans) *new_spans = 0;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    if (!begin || !size || max_span < kPageSize || max_span % kPageSize ||
        size > UINTPTR_MAX - begin || begin + size > UINTPTR_MAX - kPageMask) {
        return ErrorCode::INVALID_PARAMS;
    }
    const uintptr_t first = begin & ~kPageMask;
    const uintptr_t end = AlignUpToPage(begin + size);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& records = owner_regs_[owner];
    auto& range = records[ptr];
    if (range.complete && end <= range.end) return ErrorCode::OK;
    const uintptr_t walk_end = std::max(end, range.end);
    auto discard_empty = [&] {
        if (range.spans.empty()) records.erase(ptr);
        if (records.empty()) owner_regs_.erase(owner);
    };
    // A partial deregistration of a coalesced SPDK MR is illegal. Reject
    // independent registrations cutting such an MR before altering any refs.
    const auto left = SpanInteriorLocked(first);
    const auto right = SpanInteriorLocked(walk_end);
    for (auto edge : {left, right}) {
        if (edge != spans_.end() && edge->second.quarantined) {
            if (range.spans.insert(edge->first).second)
                ++edge->second.references;
            range.end = walk_end;
            range.complete = false;
            return ErrorCode::NOF_REGISTRATION_STUCK;
        }
    }
    if (left != spans_.end() || right != spans_.end()) {
        discard_empty();
        return ErrorCode::INVALID_PARAMS;
    }

    std::vector<uintptr_t> added;
    range.complete = false;
    bool stuck = false;
    std::vector<uintptr_t> split_ends;
    for (uintptr_t base = first; base < walk_end;) {
        auto it = spans_.lower_bound(base);
        if (it == spans_.end() || it->first != base) {
            const uintptr_t gap_end =
                it == spans_.end() ? walk_end : std::min(walk_end, it->first);
            // Only coalesce whole pages inside the supplied allocation.
            size_t len = kPageSize;
            if (base >= begin && base < begin + size) {
                len = std::min({max_span, size_t(gap_end - base),
                                size_t((begin + size - base) & ~kPageMask)});
                len = std::max(len, size_t(kPageSize));
            }
            while (!split_ends.empty() && split_ends.back() <= base)
                split_ends.pop_back();
            if (!split_ends.empty())
                len = std::min(len, size_t(split_ends.back() - base));
            int rc = register_fn_(reinterpret_cast<void*>(base), len);
            while (rc == -EBUSY && len > kPageSize) {
                // SPDK checks overlap before marking. Only individual pages
                // returning EBUSY are known to be externally registered.
                // Bisect to preserve large MRs either side of a busy page.
                split_ends.push_back(base + len);
                len = (len / kPageSize / 2) * kPageSize;
                rc = register_fn_(reinterpret_cast<void*>(base), len);
            }
            it = spans_.emplace_hint(it, base, Span{len});
            if (rc == -EBUSY) {
                it->second.external = true;
            } else if (rc != 0) {
                LOG(ERROR) << "NoF host registration failed: base="
                           << reinterpret_cast<void*>(base) << " bytes=" << len
                           << " rc=" << rc;
                // SPDK may already have marked this range. Clean it once
                // with the exact address and length originally submitted.
                if (unregister_fn_(reinterpret_cast<void*>(base), len) != 0) {
                    it->second.quarantined = true;
                    ++it->second.references;
                    range.spans.insert(base);
                    range.end = std::max(range.end, base + len);
                    return ErrorCode::NOF_REGISTRATION_STUCK;
                }
                spans_.erase(it);
                for (auto pos = added.rbegin(); pos != added.rend(); ++pos) {
                    if (!ReleaseSpanLocked(*pos)) {
                        range.end =
                            std::max(range.end, *pos + spans_.at(*pos).length);
                        return ErrorCode::NOF_REGISTRATION_STUCK;
                    }
                    range.spans.erase(*pos);
                }
                discard_empty();
                return ErrorCode::INTERNAL_ERROR;
            } else if (new_spans) {
                ++*new_spans;
            }
        }

        if (range.spans.insert(base).second) {
            ++it->second.references;
            added.push_back(base);
        }
        stuck |= it->second.quarantined;
        base += it->second.length;
    }
    range.end = walk_end;
    range.complete = !stuck;
    return stuck ? ErrorCode::NOF_REGISTRATION_STUCK : ErrorCode::OK;
}

bool NofPageRegistry::ReleaseSpanLocked(uintptr_t base) {
    auto it = spans_.find(base);
    auto& span = it->second;
    if (span.quarantined) return false;
    if (span.references == 1) {
        if (!span.external &&
            unregister_fn_(reinterpret_cast<void*>(base), span.length) != 0) {
            LOG(ERROR) << "NoF host unregister failed: base="
                       << reinterpret_cast<void*>(base)
                       << " bytes=" << span.length;
            span.quarantined = true;
            return false;
        }
        spans_.erase(it);
    } else {
        --span.references;
    }
    return true;
}

bool NofPageRegistry::ReleaseRangeLocked(OwnerRange& range) {
    range.complete = false;
    while (!range.spans.empty()) {
        auto it = range.spans.begin();
        if (!ReleaseSpanLocked(*it)) return false;
        range.spans.erase(it);
    }
    return true;
}

ErrorCode NofPageRegistry::Unregister(uint64_t owner, void* ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto owner_it = owner_regs_.find(owner);
    if (owner_it == owner_regs_.end()) {
        return ErrorCode::OK;  // this owner registered nothing: no-op
    }
    auto it = owner_it->second.find(ptr);
    if (it == owner_it->second.end()) {
        return ErrorCode::OK;  // not registered by this owner: no-op
    }
    if (!ReleaseRangeLocked(it->second)) {
        // The remaining spans stay charged to this owner so the
        // caller's quarantine path keeps the mapping pinned.
        return ErrorCode::INTERNAL_ERROR;
    }
    owner_it->second.erase(it);
    if (owner_it->second.empty()) {
        owner_regs_.erase(owner_it);
    }
    return ErrorCode::OK;
}

ErrorCode NofPageRegistry::UnregisterAll(uint64_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto owner_it = owner_regs_.find(owner);
    if (owner_it == owner_regs_.end()) {
        return ErrorCode::OK;
    }
    bool all_released = true;
    for (auto it = owner_it->second.begin(); it != owner_it->second.end();) {
        if (ReleaseRangeLocked(it->second)) {
            it = owner_it->second.erase(it);
        } else {
            // Record retained so the caller's mapping stays quarantined.
            all_released = false;
            ++it;
        }
    }
    if (owner_it->second.empty()) {
        owner_regs_.erase(owner_it);
    }
    return all_released ? ErrorCode::OK : ErrorCode::INTERNAL_ERROR;
}

}  // namespace mooncake
