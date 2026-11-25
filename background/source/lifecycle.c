#include "lifecycle.h"
#include <stdint.h>

#define SYNC_INTERVAL_NS (UINT64_C(15) * 60 * 1000000000)

static uint64_t after_interval(uint64_t now) {
    return UINT64_MAX - now < SYNC_INTERVAL_NS ? UINT64_MAX : now + SYNC_INTERVAL_NS;
}

void bg_init(BgLifecycle *bg, uint64_t now) {
    *bg = (BgLifecycle){.next_sync = now};
}

bool bg_begin(BgLifecycle *bg, uint64_t now) {
    if (bg->active || bg->paused || bg->stopping || bg->restore_failed || now < bg->next_sync)
        return false;
    bg->active = true;
    return true;
}

void bg_complete(BgLifecycle *bg, BgOutcome outcome, uint64_t now) {
    bg->active = false;
    bg->error = outcome != BG_SUCCESS;
    if (outcome == BG_RESTORE_FAILED) bg->restore_failed = true;
    bg->next_sync = after_interval(now);
}

void bg_pause(BgLifecycle *bg) { bg->paused = true; }

void bg_resume(BgLifecycle *bg, uint64_t now) {
    bg->paused = false;
    /* Do not schedule catch-up exchanges while the console was asleep. */
    bg->next_sync = after_interval(now);
}

void bg_stop(BgLifecycle *bg) { bg->stopping = true; }

bool bg_stopped(const BgLifecycle *bg) {
    return bg->stopping && !bg->active && !bg->restore_failed;
}

unsigned bg_status(const BgLifecycle *bg) {
    if (bg->restore_failed) return BG_ERROR;
    if (bg->stopping) return BG_STOPPING;
    return bg->error ? BG_ERROR : BG_RUNNING;
}
