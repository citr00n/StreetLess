#ifndef STREETLESS_TEST_3DS_H
#define STREETLESS_TEST_3DS_H
#include <stdint.h>
typedef int32_t Result;
typedef uint64_t FS_Archive;
typedef struct { int type; uint32_t size; const void *data; } FS_Path;
enum { ARCHIVE_SDMC = 9, PATH_EMPTY = 1, PATH_ASCII = 3, RD_ALREADY_EXISTS = 0x3fc };
#define R_FAILED(result) ((result) < 0)
#define R_SUCCEEDED(result) ((result) >= 0)
#define R_DESCRIPTION(result) ((result) & 0x3ff)
FS_Path fsMakePath(int type, const void *path);
Result FSUSER_OpenArchive(FS_Archive *archive, int id, FS_Path path);
Result FSUSER_CloseArchive(FS_Archive archive);
Result FSUSER_RenameFile(FS_Archive src, FS_Path from, FS_Archive dst, FS_Path to);
Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path);
#endif
