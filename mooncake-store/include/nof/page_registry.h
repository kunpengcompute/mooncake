#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>

#include "nof/io_buffer.h"
#include "types.h"  // ErrorCode

namespace mooncake {

// Bookkeeping for the process-global DMA translation table (#3131), factored
// out of the SPDK initiator so the policy is unit-testable without SPDK. The
// two memory (un)registration calls are injected: the SPDK initiator wires
// spdk_mem_register/spdk_mem_unregister, tests wire fakes.
//
// SPDK v23.01.1 semantics (lib/env_dpdk/memory.c) this mirrors:
//   - register/unregister require (vaddr, len) 2MB-aligned, else -EINVAL;
//   - registering an already-registered page fails with -EBUSY (e.g. DPDK
//     memseg memory from spdk_zmalloc, registered by the memseg walk);
//   - spdk_mem_register marks a range BEFORE its notify callbacks and does
//     not roll back on failure; -EINVAL on unregister covers both "nothing
//     marked" and "partially translated" — the two cannot be told apart.
//
// Ownership: registrations are charged to an owner token (a unique,
// never-recycled id per initiator instance). Registration refcounts are
// process-wide, so one owner's Unregister never unmaps pages another owner
// still relies on.
//
// Failure handling is fail-stop (the baseline SpdkWrapper contract): a
// failed registration is cleaned up once; anything whose cleanup cannot be
// confirmed is QUARANTINED — terminally pinned, never unregistered, never
// re-registered, never munmapped (the kernel reclaims it at process exit).
// A quarantined page is unknown translation state, NOT usable coverage: any
// registration whose range contains one reports NOF_REGISTRATION_STUCK, and
// teardown fails to release it, keeping the mapping pinned.
//
class NofPageRegistry {
   public:
    using MemRegisterFn = int (*)(void* addr, size_t len);
    using MemUnregisterFn = int (*)(void* addr, size_t len);

    NofPageRegistry(MemRegisterFn register_fn, MemUnregisterFn unregister_fn);

    NofPageRegistry(const NofPageRegistry&) = delete;
    NofPageRegistry& operator=(const NofPageRegistry&) = delete;

    ErrorCode Register(uint64_t owner, void* ptr, size_t size);
    // Whole-pool registration. Suballocations use the pool registration for
    // I/O. Independently registered borrowers must cover whole backend spans.
    // Ordinary Register keeps page-level sharing for general user buffers.
    static constexpr size_t kDefaultPoolSpan = 256ULL << 20;
    ErrorCode RegisterPool(uint64_t owner, void* ptr, size_t size,
                           size_t max_span = kDefaultPoolSpan,
                           size_t* new_spans = nullptr);
    ErrorCode Unregister(uint64_t owner, void* ptr);
    ErrorCode UnregisterAll(uint64_t owner);

   private:
    static constexpr uintptr_t kPageSize = kNofRegistrationPageSize;

    struct Span {
        size_t length;
        size_t references = 0;
        bool external = false;
        bool quarantined = false;
    };

    struct OwnerRange {
        uintptr_t end = 0;
        bool complete = false;
        std::set<uintptr_t> spans;
    };

    bool ReleaseSpanLocked(uintptr_t base);
    bool ReleaseRangeLocked(OwnerRange& range);
    using SpanMap = std::map<uintptr_t, Span>;
    SpanMap::iterator SpanInteriorLocked(uintptr_t address);

    MemRegisterFn register_fn_;
    MemUnregisterFn unregister_fn_;
    SpanMap spans_;
    std::map<uint64_t, std::map<void*, OwnerRange>> owner_regs_;
    std::mutex mutex_;
};

}  // namespace mooncake
