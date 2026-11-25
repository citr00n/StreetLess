#ifndef STREETLESS_BACKGROUND_CONTROL_H
#define STREETLESS_BACKGROUND_CONTROL_H

#include <3ds.h>

typedef enum {
    BACKGROUND_ABSENT,
    BACKGROUND_RUNNING,
    BACKGROUND_STOPPING,
    BACKGROUND_RETRY,
    BACKGROUND_UNCERTAIN
} BackgroundStatus;

/* Any error or unconfirmed launch is BACKGROUND_UNCERTAIN. */
BackgroundStatus background_status(Result *detail);
/* A failed launch may still be in progress; check status before any CECD use. */
bool background_start(Result *detail);
/* Success means the module has restored CECD and will not use it again. */
bool background_stop(Result *detail);

#endif
