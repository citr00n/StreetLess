#include <3ds.h>
#include <stdint.h>

// Newlib's heap bounds are set before constructors or application code run.
extern char *fake_heap_start;
extern char *fake_heap_end;

void __system_allocateHeaps(void) {
    enum { heap_size = 8 * 1024 * 1024 };
    u32 heap = 0;

    // This sysmodule uses no linear allocations. The default libctru allocator
    // always allocates a linear heap, which can fail for system processes.
    Result result = svcControlMemory(&heap, OS_HEAP_AREA_BEGIN, 0, heap_size,
                                     MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
    if (R_FAILED(result)) svcExitProcess();

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)(uintptr_t)heap;
    fake_heap_end = fake_heap_start + heap_size;
}
