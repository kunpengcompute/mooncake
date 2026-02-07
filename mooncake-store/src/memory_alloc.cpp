#include "memory_alloc.h"
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <spdk/env.h>

class SpdkMemoryAllocatorAtomic {
public:
    static void* alloc(size_t size, size_t align, int socket_id = -1);

private:
    static std::atomic<bool> initialized;
    static std::mutex init_mutex;

    static bool initialize_spdk();
};

std::atomic<bool> SpdkMemoryAllocatorAtomic::initialized{false};
std::mutex SpdkMemoryAllocatorAtomic::init_mutex;

bool SpdkMemoryAllocatorAtomic::initialize_spdk() {
    // 双重检查锁定模式
    if (initialized.load(std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(init_mutex);

    // 再次检查（防止竞争条件）
    if (initialized.load(std::memory_order_relaxed)) {
        return true;
    }

    // SPDK 初始化
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "spdk_allocator";

    int rc = spdk_env_init(&opts);
    if (rc != 0) {
        fprintf(stderr, "SPDK init failed: %d\n", rc);
        return false;
    }

    // 设置初始化标志
    initialized.store(true, std::memory_order_release);
    return true;
}

void* SpdkMemoryAllocatorAtomic::alloc(size_t size, size_t align, int socket_id) {
    // 确保 SPDK 已初始化
    if (!initialize_spdk()) {
        return nullptr;
    }

    // 分配内存
    void *buf = spdk_zmalloc(size, align, nullptr, socket_id, SPDK_MALLOC_DMA);
    fprintf(stderr, "spdk zmalloc: addr %p, size %lu, align %lu\n", buf, size, align);
    return buf;
}

void *hugepage_memory_alloc(size_t size)
{
    return SpdkMemoryAllocatorAtomic::alloc(size, 0x1000, -1);
}

void hugepage_memory_free(void *ptr)
{
    if (ptr) {
        spdk_free(ptr);
    }
}
