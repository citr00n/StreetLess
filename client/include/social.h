#ifndef STREETLESS_SOCIAL_H
#define STREETLESS_SOCIAL_H

#include "http.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOCIAL_PRIVACY,
    SOCIAL_FRIENDS,
    SOCIAL_ROOMS,
    SOCIAL_OUTGOING,
} SocialPage;

enum { SOCIAL_PUBLIC_OUTBOX_MAX = 12 };

typedef struct {
    int64_t queued_total;
    bool public_enabled;
    size_t public_count;
    uint32_t public_titles[SOCIAL_PUBLIC_OUTBOX_MAX];
} SocialStats;

/* False means unavailable or invalid; never substitutes a zero count. */
bool social_stats(Http *http, SocialStats *stats);

void social_open(Http *http, SocialPage page);

#endif
