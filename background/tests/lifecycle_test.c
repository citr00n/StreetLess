#include "lifecycle.h"
#include <assert.h>
#include <stdint.h>

#define INTERVAL (UINT64_C(15) * 60 * 1000000000)

int main(void) {
    BgLifecycle bg;
    bg_init(&bg, 0);
    assert(bg_status(&bg) == BG_RUNNING);
    assert(bg_begin(&bg, 0));
    assert(!bg_begin(&bg, 0));
    bg_complete(&bg, BG_RETRY, 10);
    assert(bg_status(&bg) == BG_ERROR);
    assert(!bg_begin(&bg, 10 + INTERVAL - 1));
    assert(bg_begin(&bg, 10 + INTERVAL));
    bg_complete(&bg, BG_SUCCESS, 20 + INTERVAL);
    assert(bg_status(&bg) == BG_RUNNING);
    bg_pause(&bg);
    assert(!bg_begin(&bg, UINT64_MAX));
    bg_resume(&bg, 30 + INTERVAL);
    assert(!bg_begin(&bg, 30 + 2 * INTERVAL - 1));
    assert(bg_begin(&bg, 30 + 2 * INTERVAL));
    bg_stop(&bg);
    assert(bg_status(&bg) == BG_STOPPING);
    assert(!bg_stopped(&bg));
    bg_complete(&bg, BG_SUCCESS, 40 + 2 * INTERVAL);
    assert(bg_stopped(&bg));
    assert(!bg_begin(&bg, UINT64_MAX));

    bg_init(&bg, 0);
    assert(bg_begin(&bg, 0));
    bg_complete(&bg, BG_RESTORE_FAILED, 0);
    bg_stop(&bg);
    assert(bg_status(&bg) == BG_ERROR);
    assert(!bg_stopped(&bg));
    assert(!bg_begin(&bg, UINT64_MAX));

    bg_init(&bg, 0);
    assert(bg_begin(&bg, 0));
    bg_pause(&bg);
    assert(bg.active);
    assert(!bg_begin(&bg, UINT64_MAX));
    bg_complete(&bg, BG_RETRY, 1);
    assert(!bg_begin(&bg, UINT64_MAX));
    bg_resume(&bg, 2);
    assert(!bg_begin(&bg, 2 + INTERVAL - 1));
    assert(bg_begin(&bg, 2 + INTERVAL));
    bg_stop(&bg);
    bg_complete(&bg, BG_RESTORE_FAILED, 3);
    assert(!bg_stopped(&bg));
    assert(bg_status(&bg) == BG_ERROR);

    bg_init(&bg, UINT64_MAX - 1);
    assert(bg_begin(&bg, UINT64_MAX - 1));
    bg_complete(&bg, BG_RETRY, UINT64_MAX - 1);
    assert(!bg_begin(&bg, UINT64_MAX - 1));
    assert(bg_begin(&bg, UINT64_MAX));
    return 0;
}
