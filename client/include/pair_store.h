#ifndef STREETLESS_PAIR_STORE_H
#define STREETLESS_PAIR_STORE_H
#include "pair.h"

typedef struct {
    const char *step;
    int error;
    uint32_t native;
} PairStoreError;

/* A failed rename leaves the flushed temporary file for recovery. */
bool pair_store_save(const char *tmp, const char *path, const PairData *data,
                     bool pending, bool debug, PairStoreError *error);
/* Recover only a flushed config matching this pending claim: 1 recovered,
 * 0 no matching config, -1 filesystem error (the temporary file is retained). */
int pair_store_recover(const char *tmp, const char *path, const PairData *pending,
                       PairStoreError *error);
#endif
