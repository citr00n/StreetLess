#include "pair_store.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[128];
    assert(snprintf(dir, sizeof(dir), "/tmp/streetless-pair-%ld", (long)getpid()) > 0);
    assert(mkdir(dir, 0700) == 0);
    char tmp[256], path[256], blocked[256], missing[256];
    assert(snprintf(tmp, sizeof(tmp), "%s/config.tmp", dir) > 0);
    assert(snprintf(path, sizeof(path), "%s/config.txt", dir) > 0);
    assert(snprintf(blocked, sizeof(blocked), "%s/blocked", dir) > 0);
    assert(snprintf(missing, sizeof(missing), "%s/missing/tmp", dir) > 0);
    PairData data = {.config = {.host = "relay.test", .ipv4 = "127.0.0.1", .port = 8080}};
    memset(data.code, 'a', 32);
    memset(data.config.token, 'b', 64);
    PairStoreError error;

    assert(pair_store_recover(tmp, path, &data, &error) == 0);
    assert(!pair_store_save(missing, path, &data, false, true, &error));
    assert(!strcmp(error.step, "open") && error.error == ENOENT);

    assert(pair_store_save(tmp, path, &data, false, true, &error));
    FILE *f = fopen(path, "rb");
    assert(f);
    Config saved;
    assert(config_read(f, &saved));
    assert(fclose(f) == 0);
    assert(!strcmp(saved.token, data.config.token) && saved.port == 8080);

    /* A failed rename retains the fully flushed config, recoverable without
     * resending a code that may already have expired. */
    assert(mkdir(blocked, 0700) == 0);
    assert(!pair_store_save(tmp, blocked, &data, false, true, &error));
    assert(!strcmp(error.step, "rename") && error.error != 0);
    assert(access(tmp, F_OK) == 0);
    PairData other = data;
    other.config.token[0] = 'c';
    assert(pair_store_recover(tmp, path, &other, &error) == 0);
    assert(access(tmp, F_OK) == 0);
    assert(pair_store_recover(tmp, blocked, &data, &error) == -1);
    assert(!strcmp(error.step, "rename") && access(tmp, F_OK) == 0);
    assert(pair_store_recover(tmp, path, &data, &error) == 1);
    assert(access(tmp, F_OK) != 0 && errno == ENOENT);
    f = fopen(path, "rb");
    assert(f && config_read(f, &saved));
    assert(fclose(f) == 0);
    assert(!strcmp(saved.token, data.config.token));

    assert(unlink(path) == 0);
    assert(rmdir(blocked) == 0);
    assert(rmdir(dir) == 0);
    return 0;
}
