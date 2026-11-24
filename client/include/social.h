#ifndef STREETLESS_SOCIAL_H
#define STREETLESS_SOCIAL_H

#include "http.h"

typedef enum {
    SOCIAL_PRIVACY,
    SOCIAL_FRIENDS,
    SOCIAL_ROOMS,
    SOCIAL_OUTGOING,
} SocialPage;

void social_open(Http *http, SocialPage page);

#endif
