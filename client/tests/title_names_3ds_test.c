#include "title_names.h"
#include <3ds.h>
#include <assert.h>
#include <string.h>

static int full_reads;
Result amInit(void) { return 0; }
void amExit(void) {}
Result AM_GetTitleCount(FS_MediaType media, u32 *count) {
    *count = media == MEDIATYPE_NAND || media == MEDIATYPE_SD ? 1 : 0;
    return 0;
}
Result AM_GetTitleList(u32 *read, FS_MediaType media, u32 count, u64 *ids) {
    assert(count == 1);
    *read = 1;
    ids[0] = media == MEDIATYPE_NAND ? UINT64_C(0x0004001000022800) : UINT64_C(0x0004000e00022800);
    return 0;
}
Result FSUSER_OpenFileDirectly(Handle *out, int archive, FS_Path ap, FS_Path fp, u32 flags, u32 attrs) {
    (void)ap; (void)fp; (void)attrs;
    assert(archive == ARCHIVE_SAVEDATA_AND_CONTENT && flags == FS_OPEN_READ);
    *out = 1;
    return 0;
}
Result FSFILE_GetSize(Handle file, u64 *size) { assert(file == 1); *size = 0x36c0; return 0; }
Result FSFILE_Read(Handle file, u32 *read, u64 offset, void *buffer, u32 size) {
    assert(file == 1 && offset == 0);
    /* Real 3DS ExeFS rejects shorter reads, including aligned 0x2200. */
    if (size != 0x36c0) return -1;
    ++full_reads;
    unsigned char *icon = buffer;
    memset(icon, 0, size);
    memcpy(icon, "SMDH", 4);
    icon[0x2035] = 0x08; icon[0x2036] = 0x02;
    const char *name = "StreetPass Mii Plaza";
    for (size_t i = 0; name[i]; ++i) icon[8 + 0x200 + i * 2] = (unsigned char)name[i];
    *read = size;
    return 0;
}
Result FSFILE_Close(Handle file) { assert(file == 1); return 0; }
int main(void) {
    uint32_t ids[] = { 0x00020800, 0x12345678 };
    char names[2][64];
    title_names_resolve(ids, 2, names);
    assert(full_reads == 2);
    assert(strcmp(names[0], "StreetPass Mii Plaza") == 0);
    assert(strcmp(names[1], "CEC 12345678") == 0);
    return 0;
}
