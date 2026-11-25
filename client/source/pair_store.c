#include "pair_store.h"
#include "sd_replace.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

static bool fail(PairStoreError *error, const char *step) {
    error->step = step;
    error->error = errno ? errno : EIO;
    return false;
}

bool pair_store_save(const char *tmp, const char *path, const PairData *data,
                     bool pending, bool debug, PairStoreError *error) {
    *error = (PairStoreError){0};
    FILE *f = fopen(tmp, "wb");
    if (!f) return fail(error, "open");
    bool ok = true;
    errno = 0;
    if ((pending && fprintf(f, "%c:%s\n", debug ? 'D' : 'P', data->code) != 35) ||
        fprintf(f, "host=%s\nipv4=%s\nport=%u\ntoken=%s\n", data->config.host,
                data->config.ipv4, (unsigned)data->config.port, data->config.token) <= 0)
        ok = fail(error, "write");
    if (ok) {
        errno = 0;
        if (fflush(f) != 0) ok = fail(error, "flush");
    }
    if (ok) {
        errno = 0;
        if (fsync(fileno(f)) != 0) ok = fail(error, "sync");
    }
    errno = 0;
    if (fclose(f) != 0 && ok) ok = fail(error, "close");
    if (!ok) { unlink(tmp); return false; }
    /* libctru may delete an existing destination before a failed rename. */
    SDReplaceError replacement;
    if (!sd_replace(tmp, path, &replacement)) {
        error->step = "rename";
        error->error = replacement.error;
        error->native = replacement.native;
        return false;
    }
    return true;
}

int pair_store_recover(const char *tmp, const char *path, const PairData *pending,
                       PairStoreError *error) {
    *error = (PairStoreError){0};
    FILE *f = fopen(tmp, "rb");
    if (!f) {
        if (errno == ENOENT) return 0;
        fail(error, "open temporary config");
        return -1;
    }
    Config saved;
    bool valid = config_read(f, &saved);
    if (fclose(f) != 0) {
        fail(error, "close temporary config");
        return -1;
    }
    if (!valid || saved.port != pending->config.port ||
        strcmp(saved.host, pending->config.host) || strcmp(saved.ipv4, pending->config.ipv4) ||
        strcmp(saved.token, pending->config.token)) return 0;
    SDReplaceError replacement;
    if (!sd_replace(tmp, path, &replacement)) {
        error->step = "rename";
        error->error = replacement.error;
        error->native = replacement.native;
        return -1;
    }
    return 1;
}
