#include <3ds.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "http.h"
#include "lifecycle.h"
#include "sync.h"

#define SERVICE_NAME "slss:bg"
#define ERR_CONTROL ((Result)0xD8E007F7)

/* Do not initialize APT, graphics or HID in a system process. */
void __appInit(void) { srvInit(); }
u32 __ctru_heap_size = 8 * 1024 * 1024;
u32 __ctru_linear_heap_size = 0x1000;
void __appExit(void) { srvExit(); }

extern const unsigned char _binary_build_romfs_certs_pem_start[];
extern const unsigned char _binary_build_romfs_certs_pem_end[];

static atomic_bool cancel_work;
static atomic_int work_result;
static Handle done_event;

static bool cancelled(void *unused) {
    (void)unused;
    return atomic_load_explicit(&cancel_work, memory_order_acquire);
}

static void exchange(void *unused) {
    (void)unused;
    int result = -2;
    srvSetBlockingPolicy(true);
    Config config = {0};
    FILE *file = fopen(STREETLESS_CONFIG_PATH, "rb");
    if (file) {
        bool valid = config_read(file, &config);
        if (fclose(file)) valid = false;
        if (valid && !cancelled(NULL)) {
            Http http;
            if (http_open(&http, &config, "romfs:/certs.pem", NULL)) {
                size_t ca_size = (uintptr_t)_binary_build_romfs_certs_pem_end -
                    (uintptr_t)_binary_build_romfs_certs_pem_start;
                if (ca_size && http_set_ca_blob(&http, _binary_build_romfs_certs_pem_start, ca_size) &&
                    http_set_cancel(&http, cancelled, NULL) && !cancelled(NULL))
                    result = sync_run(&http, &config);
                http_close(&http);
            }
        }
    }
    memset(&config, 0, sizeof(config));
    atomic_store_explicit(&work_result, result, memory_order_release);
    svcSignalEvent(done_event);
}

static uint64_t now_ns(void) {
    const u64 frequency = SYSCLOCK_ARM11;
    u64 tick = svcGetSystemTick();
    return (tick / frequency) * UINT64_C(1000000000) +
        (tick % frequency) * UINT64_C(1000000000) / frequency;
}

static void arm_timer(Handle timer, BgLifecycle *bg) {
    Result cancelled = svcCancelTimer(timer);
    Result cleared = svcClearTimer(timer);
    if (R_FAILED(cancelled) || R_FAILED(cleared)) bg->error = true;
    if (bg->active || bg->paused || bg->stopping || bg->restore_failed) return;
    uint64_t now = now_ns();
    uint64_t wait = bg->next_sync > now ? bg->next_sync - now : 1;
    if (R_FAILED(svcSetTimer(timer, wait > INT64_MAX ? INT64_MAX : (s64)wait, 0)))
        bg->error = true;
}

static void reply_status(unsigned state) {
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(1, 2, 0);
    cmd[1] = 0;
    cmd[2] = state;
}

static void reply_stop(Result result) {
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 1, 0);
    cmd[1] = (u32)result;
}

int main(void) {
    Handle port = 0, notifications = 0, timer = 0, quit = 0, session = 0;
    Thread worker = NULL;
    bool fs_ready = false, sd_ready = false;
    bool sleep_reply_pending = false, shutdown = false, replying_stop = false;
    Handle reply = 0;
    BgLifecycle bg;
    bg_init(&bg, now_ns());

    /* Registration is the singleton and precedes FS, HTTP and CECD activity. */
    if (R_FAILED(srvRegisterService(&port, SERVICE_NAME, 1))) return 1;
    if (R_FAILED(srvEnableNotification(&notifications)) ||
        R_FAILED(ptmSysmInit()) ||
        R_FAILED(srvSubscribe(PTMNOTIFID_SLEEP_REQUESTED)) ||
        R_FAILED(srvSubscribe(PTMNOTIFID_SLEEP_DENIED)) ||
        R_FAILED(srvSubscribe(PTMNOTIFID_GOING_TO_SLEEP)) ||
        R_FAILED(srvSubscribe(PTMNOTIFID_FULLY_WAKING_UP)) ||
        R_FAILED(srvSubscribe(PTMNOTIFID_FULLY_AWAKE)) ||
        R_FAILED(srvSubscribe(PTMNOTIFID_SHUTDOWN)) ||
        R_FAILED(srvSubscribe(0x100)) ||
        R_FAILED(svcCreateTimer(&timer, RESET_ONESHOT)) ||
        R_FAILED(svcCreateEvent(&done_event, RESET_ONESHOT)) ||
        R_FAILED(svcCreateEvent(&quit, RESET_ONESHOT))) {
        /* No exchange has started, so removing the registration is safe. */
        srvUnregisterService(SERVICE_NAME);
        return 1;
    }
    arm_timer(timer, &bg);

    for (;;) {
        Handle handles[6];
        int n = 0;
        handles[n++] = port;
        handles[n++] = notifications;
        handles[n++] = timer;
        handles[n++] = done_event;
        handles[n++] = quit;
        if (session) handles[n++] = session;
        s32 index = -1;
        Result rc = svcReplyAndReceive(&index, handles, n, reply);
        reply = 0;
        if (replying_stop) {
            replying_stop = false;
            if (R_SUCCEEDED(rc)) break;
            svcClearEvent(quit);
            if (session) svcCloseHandle(session);
            session = 0;
        }
        if (R_FAILED(rc)) {
            /* A disconnected control client must never release the singleton. */
            if (index >= 0 && index < n && handles[index] == session) {
                svcCloseHandle(session);
                session = 0;
            }
            continue;
        }
        Handle signaled = handles[index];
        if (signaled == port) {
            Handle accepted = 0;
            if (R_SUCCEEDED(svcAcceptSession(&accepted, port))) {
                if (!session) session = accepted;
                else svcCloseHandle(accepted);
            }
        } else if (signaled == notifications) {
            u32 id = 0;
            if (R_FAILED(srvReceiveNotification(&id))) continue;
            if (id == PTMNOTIFID_SLEEP_REQUESTED) {
                bg_pause(&bg);
                atomic_store_explicit(&cancel_work, true, memory_order_release);
                if (bg.active) sleep_reply_pending = true;
                else PTMSYSM_ReplyToSleepQuery(false);
                arm_timer(timer, &bg);
            } else if (id == PTMNOTIFID_SLEEP_DENIED || id == PTMNOTIFID_FULLY_AWAKE) {
                sleep_reply_pending = false;
                bg_resume(&bg, now_ns());
                arm_timer(timer, &bg);
            } else if (id == PTMNOTIFID_GOING_TO_SLEEP || id == PTMNOTIFID_FULLY_WAKING_UP) {
                PTMSYSM_NotifySleepPreparationComplete(ptmSysmGetNotificationAckValue(id));
            } else if (id == PTMNOTIFID_SHUTDOWN || id == 0x100) {
                shutdown = true;
                bg_stop(&bg);
                atomic_store_explicit(&cancel_work, true, memory_order_release);
                if (!bg.active) break;
            }
        } else if (signaled == timer) {
            if (!bg_begin(&bg, now_ns())) { arm_timer(timer, &bg); continue; }
            if (!fs_ready) fs_ready = R_SUCCEEDED(fsInit());
            if (fs_ready && !sd_ready) sd_ready = R_SUCCEEDED(archiveMountSdmc());
            atomic_store_explicit(&cancel_work, false, memory_order_release);
            if (sd_ready)
                worker = threadCreate(exchange, NULL, 0x40000, 0x30, -2, false);
            if (!worker) {
                bg_complete(&bg, BG_RETRY, now_ns());
                arm_timer(timer, &bg);
            }
        } else if (signaled == done_event) {
            if (worker) {
                threadJoin(worker, U64_MAX);
                threadFree(worker);
                worker = NULL;
                int result = atomic_load_explicit(&work_result, memory_order_acquire);
                bg_complete(&bg, result == -1 ? BG_RESTORE_FAILED : result < 0 ? BG_RETRY : BG_SUCCESS, now_ns());
            }
            if (sleep_reply_pending) {
                sleep_reply_pending = false;
                PTMSYSM_ReplyToSleepQuery(false);
            }
            if (shutdown) break;
            if (bg.stopping && session) {
                reply_stop(bg_stopped(&bg) ? 0 : ERR_CONTROL);
                reply = session;
                if (bg_stopped(&bg)) { svcSignalEvent(quit); replying_stop = true; }
            }
            arm_timer(timer, &bg);
        } else if (signaled == session) {
            u32 *cmd = getThreadCommandBuffer();
            if (cmd[0] == IPC_MakeHeader(1, 0, 0)) {
                reply_status(bg_status(&bg));
                reply = session;
            } else if (cmd[0] == IPC_MakeHeader(2, 0, 0)) {
                bg_stop(&bg);
                atomic_store_explicit(&cancel_work, true, memory_order_release);
                arm_timer(timer, &bg);
                if (!bg.active) {
                    reply_stop(bg_stopped(&bg) ? 0 : ERR_CONTROL);
                    reply = session;
                    if (bg_stopped(&bg)) { svcSignalEvent(quit); replying_stop = true; }
                }
            } else {
                cmd[0] = IPC_MakeHeader(cmd[0] >> 16, 1, 0);
                cmd[1] = (u32)ERR_CONTROL;
                reply = session;
            }
        }
    }
    srvUnregisterService(SERVICE_NAME);
    if (session) svcCloseHandle(session);
    svcCloseHandle(port);
    svcCloseHandle(notifications);
    svcCloseHandle(timer);
    svcCloseHandle(done_event);
    svcCloseHandle(quit);
    ptmSysmExit();
    if (sd_ready) archiveUnmount("sdmc");
    if (fs_ready) fsExit();
    return 0;
}
