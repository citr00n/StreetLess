#include <3ds.h>
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>

void __system_allocateHeaps(void);
char *fake_heap_start;
char *fake_heap_end;
static jmp_buf exit_env;
static int fail_allocation;
static int calls;
static int initialized;

Result svcControlMemory(u32 *out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm) {
    calls++;
    assert(addr0 == OS_HEAP_AREA_BEGIN && addr1 == 0);
    assert(size == 8u * 1024 * 1024 && op == MEMOP_ALLOC);
    assert(perm == (MEMPERM_READ | MEMPERM_WRITE));
    if (fail_allocation) return -1;
    *out = addr0;
    return 0;
}

_Noreturn void svcExitProcess(void) {
    longjmp(exit_env, 1);
}

void mappableInit(u32 min, u32 max) {
    assert(min == OS_MAP_AREA_BEGIN && max == OS_MAP_AREA_END);
    initialized++;
}

int main(void) {
    __system_allocateHeaps();
    assert(calls == 1 && initialized == 1);
    assert((uintptr_t)fake_heap_start == OS_HEAP_AREA_BEGIN);
    assert((uintptr_t)fake_heap_end == OS_HEAP_AREA_BEGIN + 8u * 1024 * 1024);

    calls = initialized = 0;
    fail_allocation = 1;
    fake_heap_start = fake_heap_end = 0;
    if (setjmp(exit_env) == 0) {
        __system_allocateHeaps();
        assert(!"failed heap allocation must terminate the process");
    }
    assert(calls == 1 && initialized == 0);
    assert(!fake_heap_start && !fake_heap_end);
    return 0;
}
