#pragma once
#include <stdint.h>

typedef uint32_t u32;
typedef int32_t Result;
typedef enum { MEMOP_ALLOC = 3 } MemOp;
typedef enum { MEMPERM_READ = 1, MEMPERM_WRITE = 2 } MemPerm;

#define R_FAILED(result) ((result) < 0)
#define OS_HEAP_AREA_BEGIN 0x08000000u
#define OS_MAP_AREA_BEGIN 0x10000000u
#define OS_MAP_AREA_END 0x14000000u

Result svcControlMemory(u32 *out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm);
_Noreturn void svcExitProcess(void);
void mappableInit(u32 min, u32 max);
