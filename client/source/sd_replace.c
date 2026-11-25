#include "sd_replace.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __3DS__
#include <3ds.h>

bool sd_replace(const char *source, const char *destination, SDReplaceError *error) {
    *error = (SDReplaceError){0};
    if (strncmp(source, "sdmc:/", 6) || strncmp(destination, "sdmc:/", 6)) {
        error->error = EINVAL;
        return false;
    }
    FS_Archive archive;
    Result result = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(result)) {
        error->error = EIO;
        error->native = (uint32_t)result;
        return false;
    }
    FS_Path from = fsMakePath(PATH_ASCII, source + 5);
    FS_Path to = fsMakePath(PATH_ASCII, destination + 5);
    result = FSUSER_RenameFile(archive, from, archive, to);
    /* SDMC returns its own destination-exists code rather than the generic
     * RD_ALREADY_EXISTS checked by libctru's rename implementation. */
    if ((uint32_t)result == UINT32_C(0xC82044BE) ||
        (R_FAILED(result) && R_DESCRIPTION(result) == RD_ALREADY_EXISTS)) {
        result = FSUSER_DeleteFile(archive, to);
        if (R_SUCCEEDED(result)) result = FSUSER_RenameFile(archive, from, archive, to);
    }
    FSUSER_CloseArchive(archive);
    if (R_FAILED(result)) {
        error->error = EIO;
        error->native = (uint32_t)result;
        return false;
    }
    return true;
}
#else
bool sd_replace(const char *source, const char *destination, SDReplaceError *error) {
    *error = (SDReplaceError){0};
    if (rename(source, destination) == 0) return true;
    error->error = errno ? errno : EIO;
    return false;
}
#endif
