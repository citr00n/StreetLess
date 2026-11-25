#include "background_control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BG_SERVICE "slss:bg"
#define BG_TITLE_ID 0x0004013000F5A302ULL
#define BG_CXI "sdmc:/luma/sysmodules/0004013000F5A302.cxi"
#define BG_MARKER "sdmc:/3ds/streetless/background.pending"

/* Luma's svcControlService(STEAL_CLIENT_SESSION), SVC 0xB0. */
static Result steal_pm(Handle *out) {
    register u32 r0 __asm__("r0") = 0;
    register Handle *r1 __asm__("r1") = out;
    register const char *r2 __asm__("r2") = "pm:app";
    __asm__ volatile ("svc 0xb0" : "+r"(r0), "+r"(r1), "+r"(r2) : : "r3", "memory", "cc");
    return (Result)r0;
}

/* A smaller system tick proves a reboot occurred since the launch attempt.
 * In every other case the marker remains authoritative, even after reopening
 * the application. This deliberately favors safety over automatic recovery. */
static bool marker_pending(bool *pending) {
    FILE *f = fopen(BG_MARKER, "rb");
    if (!f) {
        if (errno != ENOENT) return false;
        *pending = false;
        return true;
    }
    u64 tick;
    bool ok = fread(&tick, 1, sizeof(tick), f) == sizeof(tick) && fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) return false;
    if (svcGetSystemTick() < tick) {
        if (unlink(BG_MARKER) != 0) return false;
        *pending = false;
    } else {
        *pending = true;
    }
    return true;
}

static bool create_marker(void) {
    int fd = open(BG_MARKER, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    u64 tick = svcGetSystemTick();
    ssize_t written = write(fd, &tick, sizeof(tick));
    bool ok = written == sizeof(tick) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    /* Do not remove a partial marker: it must fail closed. */
    return ok;
}

static Result connect_service(Handle *handle) {
    srvSetBlockingPolicy(true);
    Result rc = srvGetServiceHandleDirect(handle, BG_SERVICE);
    srvSetBlockingPolicy(false);
    return rc;
}

static Result command(Handle handle, unsigned id, u32 *state) {
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(id, 0, 0);
    Result rc = svcSendSyncRequest(handle);
    if (R_FAILED(rc)) return rc;
    if (cmd[0] != IPC_MakeHeader(id, id == 1 ? 2 : 1, 0))
        return (Result)0xD9001830;
    rc = (Result)cmd[1];
    if (R_SUCCEEDED(rc) && state) *state = cmd[2];
    return rc;
}

BackgroundStatus background_status(Result *detail) {
    bool registered;
    *detail = 0;
    Result rc = srvIsServiceRegistered(&registered, BG_SERVICE);
    if (R_FAILED(rc)) { *detail = rc; return BACKGROUND_UNCERTAIN; }
    if (!registered) {
        bool pending;
        if (!marker_pending(&pending)) return BACKGROUND_UNCERTAIN;
        return pending ? BACKGROUND_UNCERTAIN : BACKGROUND_ABSENT;
    }
    Handle handle = 0;
    rc = connect_service(&handle);
    if (R_FAILED(rc)) { *detail = rc; return BACKGROUND_UNCERTAIN; }
    u32 state = 0;
    rc = command(handle, 1, &state);
    svcCloseHandle(handle);
    if (R_FAILED(rc)) { *detail = rc; return BACKGROUND_UNCERTAIN; }
    switch (state) {
        case 1: return BACKGROUND_RUNNING;
        case 2: return BACKGROUND_STOPPING;
        case 3: return BACKGROUND_RETRY;
        default: return BACKGROUND_UNCERTAIN;
    }
}

bool background_start(Result *detail) {
    if (background_status(detail) != BACKGROUND_ABSENT) return false;
    s64 version = 0;
    Result probe = svcGetSystemInfo(&version, 0x10000, 0);
    if (R_FAILED(probe) || version < SYSTEM_VERSION(13, 0, 0)) {
        *detail = (Result)0xD900182E;
        return false;
    }
    FILE *module = fopen(BG_CXI, "rb");
    if (!module) { *detail = (Result)0xD900182F; return false; }
    if (fclose(module) != 0) { *detail = (Result)0xD900182F; return false; }
    Handle pm = 0;
    Result rc = steal_pm(&pm);
    if (R_FAILED(rc)) { *detail = rc; return false; }
    if (!create_marker()) { svcCloseHandle(pm); *detail = (Result)0xD9001830; return false; }
    FS_ProgramInfo info = { .programId = BG_TITLE_ID, .mediaType = MEDIATYPE_NAND };
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(1, 5, 0);
    memcpy(&cmd[1], &info, sizeof(info));
    cmd[5] = PMLAUNCHFLAG_LOAD_DEPENDENCIES;
    rc = svcSendSyncRequest(pm);
    if (R_SUCCEEDED(rc)) rc = (Result)cmd[1];
    svcCloseHandle(pm);
    if (R_FAILED(rc)) { *detail = rc; return false; }
    for (unsigned i = 0; i < 80; ++i) {
        BackgroundStatus status = background_status(detail);
        if (status == BACKGROUND_RUNNING || status == BACKGROUND_RETRY) return true;
        if (status == BACKGROUND_STOPPING) return false;
        svcSleepThread(100000000LL);
    }
    return false;
}

bool background_stop(Result *detail) {
    *detail = 0;
    Handle handle = 0;
    Result rc = connect_service(&handle);
    if (R_FAILED(rc)) { *detail = rc; return false; }
    rc = command(handle, 2, NULL);
    svcCloseHandle(handle);
    if (R_FAILED(rc)) { *detail = rc; return false; }
    if (unlink(BG_MARKER) != 0 && errno != ENOENT) return false;
    return true;
}
