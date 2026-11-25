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
#ifdef __3DS__
bool pair_run(Config *config, const char *debug_pin);
#endif
#endif
