#include <cstdint>

void *hugepage_memory_alloc(size_t size);
void hugepage_memory_free(void *ptr);