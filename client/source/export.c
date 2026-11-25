#include "export.h"
#include "cecd.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPORT_DIR "sdmc:/3ds/streetless"

typedef struct { CecSlot slot; uint8_t *data; } ExportSlot;

static void wait_to_exit(void) {
    puts("\nSTART: Exit");
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_START) return;
    }
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    struct stat attrs;
    return errno == EEXIST && stat(path, &attrs) == 0 && S_ISDIR(attrs.st_mode);
}

static bool check_destination(const char *path, bool *exists) {
    struct stat attrs;
    *exists = stat(path, &attrs) == 0;
    return *exists ? S_ISREG(attrs.st_mode) : errno == ENOENT;
}

static bool save_slot(const char *path, const ExportSlot *slot, bool replace) {
    if (!ensure_dir("sdmc:/3ds") || !ensure_dir(EXPORT_DIR)) return false;
    char temporary[sizeof(EXPORT_DIR "/test-12345678.slot.tmp")];
    snprintf(temporary, sizeof(temporary), EXPORT_DIR "/test-%08lx.slot.tmp",
        (unsigned long)slot->slot.title);
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    size_t size = slot->slot.size;
    bool ok = fwrite(slot->data, 1, size, file) == size && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    bool exists;
    if (ok && check_destination(path, &exists) && (!exists || replace) &&
        rename(temporary, path) == 0) return true;
    if (unlink(temporary) != 0 && errno != ENOENT) puts("Cannot remove temporary file from SD.");
    return false;
}

static void report(const char *phase, Result result) {
    printf("CECD: %s\n(%08lx)\n", phase, (unsigned long)(u32)result);
}

bool export_run(void) {
    consoleClear();
    puts("Streetless - diagnostic slot export\n\nChecking local StreetPass outboxes...");
    Handle service;
    Result rc = cecd_open(&service);
    if (R_FAILED(rc)) {
        report("cannot access cecd:s", rc);
        puts("Use Luma3DS and current system software.");
        wait_to_exit();
        return true;
    }
    CecExchange session = {0};
    CecSlot metadata[SL_TITLES];
    ExportSlot valid[SL_TITLES];
    u32 native_count = 0, count = 0;
    unsigned skipped = 0;
    const char *phase;
    rc = cecd_begin(service, &session, metadata, &native_count, &phase);
    bool ok = R_SUCCEEDED(rc);
    if (!ok) report(phase, rc);
    for (u32 i = 0; ok && i < native_count; ++i) {
        const CecSlot *slot = &metadata[i];
        if (!slot->size || slot->mode == 1) continue;
        uint8_t *data = malloc(slot->size);
        if (!data) {
            puts("Not enough memory to read outbox.");
            ok = false;
            break;
        }
        rc = cecd_get_slot(service, slot, data);
        if (R_FAILED(rc)) {
            report("reading local outbox", rc);
            free(data);
            ok = false;
            break;
        }
        SLStats stats;
        SLSlotResult result = sl_validate_slot(data, slot->size, slot->title, &stats);
        if (result == SL_SLOT_OK) valid[count++] = (ExportSlot){*slot, data};
        else {
            free(data);
            ++skipped;
        }
    }
    Result rollback;
    Result restored = cecd_finish(service, &session, &rollback);
    svcCloseHandle(service);
    if (R_FAILED(rollback)) {
        report("ending read-only exchange", rollback);
        ok = false;
    }
    if (R_FAILED(restored)) {
        report("could not restore normal StreetPass", restored);
        puts("Restart the console before using StreetPass.");
        ok = false;
    }
    if (!ok || !count) {
        if (ok) puts("No valid public outboxes to export.");
        if (skipped) printf("Skipped %u invalid or friends-only slot(s).\n", skipped);
        wait_to_exit();
        goto done;
    }

    u32 selected = 0;
    bool confirm = false, redraw = true;
    bool path_ok = false, replace = false;
    while (aptMainLoop()) {
        char path[sizeof(EXPORT_DIR "/test-12345678.slot")];
        snprintf(path, sizeof(path), EXPORT_DIR "/test-%08lx.slot",
            (unsigned long)valid[selected].slot.title);
        if (redraw) {
            consoleClear();
            puts("Streetless - diagnostic slot export\n");
            if (!confirm) {
                for (u32 i = 0; i < count; ++i) {
                    const CecSlot *slot = &valid[i].slot;
                    printf("%c %08lx  (%lu bytes)\n", selected == i ? '>' : ' ',
                        (unsigned long)slot->title, (unsigned long)slot->size);
                }
                if (skipped) printf("Skipped %u invalid/friends-only.\n", skipped);
                puts("\nUp/Down: Choose  A: Select  START: Exit");
            } else {
                path_ok = check_destination(path, &replace);
                if (!path_ok) puts("Cannot use this SD destination.");
                else if (replace) puts("Replace existing file?");
                else puts("Export native outbox to this file?");
                printf("%s\n\n", path);
                puts("Contains the game's original StreetPass data.\nShare only with consent.\n");
                puts(path_ok ? "A: Confirm  B: Back  START: Exit" : "B: Back  START: Exit");
            }
            redraw = false;
        }
        gspWaitForVBlank();
        hidScanInput();
        u32 keys = hidKeysDown();
        if (keys & KEY_START) break;
        if (confirm) {
            if (keys & KEY_B) { confirm = false; redraw = true; }
            else if ((keys & KEY_A) && path_ok) {
                if (save_slot(path, &valid[selected], replace)) printf("\nSaved %s\n", path);
                else puts("\nCould not write slot file to SD.");
                wait_to_exit();
                break;
            }
        } else if (keys & KEY_A) { confirm = true; redraw = true; }
        else if (keys & KEY_UP) { selected = (selected + count - 1) % count; redraw = true; }
        else if (keys & KEY_DOWN) { selected = (selected + 1) % count; redraw = true; }
    }
 done:
    for (u32 i = 0; i < count; ++i) free(valid[i].data);
    return R_SUCCEEDED(restored);
}
