#include "sd_replace.h"
#include <3ds.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

#define FS_EXISTS ((Result)UINT32_C(0xC82044BE))
#define FS_MISSING ((Result)UINT32_C(0xC8804478))
#define FS_DENIED ((Result)UINT32_C(0xC8A04401))
static int source_exists, destination, deletes, renames, closes;
static Result delete_error, retry_error;

FS_Path fsMakePath(int type, const void *path) {
    return (FS_Path){.type = type, .data = path};
}
Result FSUSER_OpenArchive(FS_Archive *archive, int id, FS_Path path) {
    assert(id == ARCHIVE_SDMC && path.type == PATH_EMPTY);
    *archive = 1;
    return 0;
}
Result FSUSER_CloseArchive(FS_Archive archive) { assert(archive == 1); ++closes; return 0; }
Result FSUSER_RenameFile(FS_Archive src, FS_Path from, FS_Archive dst, FS_Path to) {
    assert(src == 1 && dst == 1 && from.type == PATH_ASCII && to.type == PATH_ASCII);
    assert(!strcmp(from.data, "/3ds/streetless/config.txt.tmp"));
    assert(!strcmp(to.data, "/3ds/streetless/config.txt"));
    ++renames;
    if (!source_exists) return FS_MISSING;
    if (destination) return FS_EXISTS;
    if (retry_error) return retry_error;
    source_exists = 0;
    destination = 2;
    return 0;
}
Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path) {
    assert(archive == 1 && !strcmp(path.data, "/3ds/streetless/config.txt"));
    ++deletes;
    if (delete_error) return delete_error;
    assert(destination == 1);
    destination = 0;
    return 0;
}

static void reset(void) {
    source_exists = 1;
    destination = 1;
    deletes = renames = closes = 0;
    delete_error = retry_error = 0;
}

int main(void) {
    const char *source = "sdmc:/3ds/streetless/config.txt.tmp";
    const char *target = "sdmc:/3ds/streetless/config.txt";
    SDReplaceError error;

    reset();
    assert(sd_replace(source, target, &error));
    assert(!source_exists && destination == 2 && renames == 2 && deletes == 1 && closes == 1);

    reset(); source_exists = 0;
    assert(!sd_replace(source, target, &error));
    assert(error.native == (uint32_t)FS_MISSING && destination == 1 && deletes == 0 && closes == 1);

    reset(); delete_error = FS_DENIED;
    assert(!sd_replace(source, target, &error));
    assert(error.native == (uint32_t)FS_DENIED && source_exists && destination == 1 && closes == 1);

    reset(); retry_error = FS_DENIED;
    assert(!sd_replace(source, target, &error));
    assert(error.native == (uint32_t)FS_DENIED && source_exists && !destination && closes == 1);

    reset(); destination = 0;
    assert(sd_replace(source, target, &error));
    assert(!source_exists && destination == 2 && deletes == 0 && renames == 1 && closes == 1);

    assert(!sd_replace("other:/source", target, &error));
    assert(error.error == EINVAL);
    return 0;
}
