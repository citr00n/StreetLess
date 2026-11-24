#ifndef STREETLESS_SYNC_H
#define STREETLESS_SYNC_H
#include "http.h"

/* Returns -1 if CECD could not be restored; otherwise the local slot count
 * (0 includes a reported, recoverable error). */
int sync_run(Http *http, const Config *config);
#endif
