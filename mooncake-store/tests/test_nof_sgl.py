"""Compile the production NoF SGL callbacks and padding pool without SPDK.

Checks logical byte order, chunk offsets, tail padding, and DMA pool ownership.
This is a CPU regression check, not a hardware DMA test.
"""

from nof_test_support import CppTestCase, REPO, definition, main


HARNESS = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#ifdef _WIN32
// MinGW lacks this declaration; SystemDmaAllocator is not instantiated here.
extern "C" void* aligned_alloc(size_t, size_t);
#endif
#include "nof/padding_buffer_pool.h"

namespace mooncake {
struct Slice { void* ptr; size_t size; };
struct NofTask { std::vector<Slice> slices; };
struct NofSubTask {
    NofTask* task;
    uint64_t payload_byte_offset = 0;
    uint32_t payload_byte_length = 0;
    size_t sgl_index = 0;
    size_t sgl_offset = 0;
    uint32_t sgl_remaining = 0;
};
@@HELPERS@@
@@CALLBACKS@@
}

using namespace mooncake;
class TestAllocator : public DmaBufferAllocator {
 public:
    int allocations = 0;
    int frees = 0;
    bool fail = false;
    void* Alloc(size_t size, size_t, int) override {
        if (fail) return nullptr;
        ++allocations;
        return std::malloc(size);
    }
    void Free(void* ptr) override {
        if (ptr) { ++frees; std::free(ptr); }
    }
};

std::vector<char> Gather(NofTask& task, uint64_t start, uint32_t length,
                         uint32_t reset = 0) {
    NofSubTask sub{&task, start, length};
    nvmf_reset_sgl(&sub, reset);
    std::vector<char> bytes;
    while (sub.sgl_remaining) {
        void* address = nullptr;
        uint32_t size = 0;
        assert(nvmf_next_sge(&sub, &address, &size) == 0);
        auto* data = static_cast<char*>(address);
        bytes.insert(bytes.end(), data, data + size);
    }
    void* address = nullptr;
    uint32_t size = 0;
    assert(nvmf_next_sge(&sub, &address, &size) == -EINVAL);
    return bytes;
}

int main() {
    auto allocator = std::make_shared<TestAllocator>();
    auto pool = std::make_shared<NofPaddingPool>(allocator);
    void* zero = nullptr;
    std::shared_ptr<void> zero_owner;
    allocator->fail = true;
    assert(pool->Acquire(5, true, &zero, &zero_owner) == -ENOMEM);
    allocator->fail = false;
    assert(pool->Acquire(5, true, &zero, &zero_owner) == 0);
    assert(allocator->allocations == 1);
    assert(pool->Acquire(0, true, &zero, &zero_owner) == -EINVAL);
    assert(pool->Acquire(4097, true, &zero, &zero_owner) == -E2BIG);

    // Host alignment gaps contain sentinel bytes and must not reach the SSD.
    std::array<char, 192> host;
    host.fill('!');
    std::memcpy(host.data(), "abc", 3);
    std::memcpy(host.data() + 64, "defghijk", 8);
    std::vector<Slice> logical;
    assert(BuildNofLogicalSlices({{host.data(), 3}, {host.data() + 64, 8}},
                                 11, &logical));
    NofTask task{logical};
    task.slices.push_back({zero, 5});
    std::vector<char> expected{'a','b','c','d','e','f','g','h','i','j','k',0,0,0,0,0};
    assert(Gather(task, 0, 16) == expected);
    assert(Gather(task, 4, 8) == std::vector<char>(expected.begin() + 4,
                                                  expected.begin() + 12));
    assert(Gather(task, 4, 8, 3) == std::vector<char>(expected.begin() + 7,
                                                     expected.begin() + 12));
    assert(Gather(task, 0, 16, 17).empty());
    assert(BuildNofLogicalSlices(logical, 9, &task.slices));
    assert(task.slices.back().size == 6);
    assert(!BuildNofLogicalSlices(logical, 12, &task.slices));
    assert(!BuildNofLogicalSlices({{nullptr, 4}}, 4, &task.slices));

    // Read padding is exclusive and separate from the immutable write zeros.
    void* first = nullptr;
    void* second = nullptr;
    std::shared_ptr<void> first_owner, second_owner;
    assert(pool->Acquire(5, false, &first, &first_owner) == 0);
    assert(pool->Acquire(5, false, &second, &second_owner) == 0);
    assert(first != second && first != zero && second != zero);
    std::memset(first, 'x', 5);
    assert(std::memcmp(zero, expected.data() + 11, 5) == 0);
    first_owner.reset();
    void* reused = nullptr;
    assert(pool->Acquire(5, false, &reused, &first_owner) == 0);
    assert(reused == first);

    // Fill the pool, fail predictably, then recover after a read completes.
    std::vector<std::shared_ptr<void>> owners;
    int result = 0;
    do {
        std::shared_ptr<void> owner;
        void* ptr = nullptr;
        result = pool->Acquire(1, false, &ptr, &owner);
        if (result == 0) owners.push_back(std::move(owner));
    } while (result == 0);
    assert(result == -EAGAIN && owners.size() == 1022);
    owners.clear();
    assert(allocator->allocations == 1);
    pool.reset();
    assert(allocator->frees == 0);
    first_owner.reset(); second_owner.reset(); zero_owner.reset();
    assert(allocator->frees == 1);
}
"""


class NofSglTest(CppTestCase):
    def test_production_sgl_and_padding_pool(self):
        production = (REPO / "mooncake-store/src/transfer_task.cpp").read_text(
            encoding="utf-8"
        )
        helpers = "\n".join(
            definition(production, marker)
            for marker in ("bool BuildNofLogicalSlices(", "bool LocateNofSglOffset(")
        )
        callbacks = "\n".join(
            definition(production, marker)
            for marker in ("static void nvmf_reset_sgl(", "static int nvmf_next_sge(")
        )
        source = HARNESS.replace("@@HELPERS@@", helpers).replace(
            "@@CALLBACKS@@", callbacks
        )
        self.run_case(self.build(source))


if __name__ == "__main__":
    main()
