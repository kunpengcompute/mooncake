#include "memory_alloc.h"
#include "spdk/spdk_wrapper.h"

void *hugepage_memory_alloc(size_t size)
{
    return mooncake::SpdkWrapper::GetInstance().Alloc(size, 0x1000, -1);
}

void hugepage_memory_free(void *ptr)
{
    mooncake::SpdkWrapper::GetInstance().Free(ptr);
}
