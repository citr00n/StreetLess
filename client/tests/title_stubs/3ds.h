#ifndef TITLE_TEST_3DS_H
#define TITLE_TEST_3DS_H
#include <stdint.h>
typedef int32_t Result;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int Handle;
typedef enum { MEDIATYPE_NAND, MEDIATYPE_SD, MEDIATYPE_GAME_CARD } FS_MediaType;
typedef struct { int type; u32 size; const void *data; } FS_Path;
enum { PATH_BINARY = 1, ARCHIVE_SAVEDATA_AND_CONTENT = 2, FS_OPEN_READ = 1 };
#define R_FAILED(r) ((r) < 0)
#define R_SUCCEEDED(r) ((r) >= 0)
Result amInit(void);
void amExit(void);
Result AM_GetTitleCount(FS_MediaType media, u32 *count);
Result AM_GetTitleList(u32 *read, FS_MediaType media, u32 count, u64 *ids);
Result FSUSER_OpenFileDirectly(Handle *out, int archive, FS_Path archive_path, FS_Path file_path, u32 flags, u32 attributes);
Result FSFILE_GetSize(Handle file, u64 *size);
Result FSFILE_Read(Handle file, u32 *read, u64 offset, void *buffer, u32 size);
Result FSFILE_Close(Handle file);
#endif
