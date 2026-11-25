#ifndef STREETLESS_BACKGROUND_LIFECYCLE_H
#define STREETLESS_BACKGROUND_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

enum { BG_RUNNING = 1, BG_STOPPING = 2, BG_ERROR = 3 };
typedef enum { BG_SUCCESS, BG_RETRY, BG_RESTORE_FAILED } BgOutcome;

typedef struct {
    uint64_t next_sync;
    bool active;
    bool paused;
    bool stopping;
    bool error;
    bool restore_failed;
} BgLifecycle;

void bg_init(BgLifecycle *bg, uint64_t now);
bool bg_begin(BgLifecycle *bg, uint64_t now);
void bg_complete(BgLifecycle *bg, BgOutcome outcome, uint64_t now);
void bg_pause(BgLifecycle *bg);
void bg_resume(BgLifecycle *bg, uint64_t now);
void bg_stop(BgLifecycle *bg);
bool bg_stopped(const BgLifecycle *bg);
unsigned bg_status(const BgLifecycle *bg);

#endif
