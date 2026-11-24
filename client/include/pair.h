#ifndef STREETLESS_PAIR_H
#define STREETLESS_PAIR_H
#include "config.h"
#include <stddef.h>

typedef struct {
    Config config;
    char code[33];
} PairData;

/* Parses exactly streetless1|HOST|IPV4|PORT|CODE, without a URL or token. */
bool pair_parse(const unsigned char *payload, size_t length, PairData *out);
#if defined(__3DS__) && !defined(STREETLESS_TEST_SLOT_EXPORT)
bool pair_run(Config *config);
#endif
#endif
