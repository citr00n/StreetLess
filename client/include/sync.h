#ifndef STREETLESS_SYNC_H
#define STREETLESS_SYNC_H
#include "http.h"

/* Returns -1 if CECD could not be restored or rolled back, -2 on a
 * recoverable error or cancellation, otherwise the committed slot count. */
int sync_run(Http *http, const Config *config);
#endif
