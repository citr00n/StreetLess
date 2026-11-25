#include "export.h"
#include "cecd.h"
#include "menu_ui.h"
#include "ui.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPORT_DIR "sdmc:/3ds/streetless"

typedef struct { CecSlot slot; uint8_t *data; } ExportSlot;

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
    if (unlink(temporary) != 0 && errno != ENOENT)
        ui_message("Cannot remove temporary file from SD.");
    return false;
}

static void report(char *message, size_t capacity, const char *phase, Result result) {
    size_t used = strlen(message);
    if (used < capacity)
        snprintf(message + used, capacity - used, "CECD: %s (%08lx)\n",
            phase, (unsigned long)(u32)result);
}

bool export_run(void) {
    consoleClear();
    puts("Streetless - diagnostic slot export\n\nChecking local StreetPass outboxes...");
    char message[512] = "";
    Handle service;
    Result rc = cecd_open(&service);
    if (R_FAILED(rc)) {
        report(message, sizeof(message), "cannot access cecd:s", rc);
        strncat(message, "Use Luma3DS and current system software.",
            sizeof(message) - strlen(message) - 1);
        ui_message(message);
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
    if (!ok) report(message, sizeof(message), phase, rc);
    for (u32 i = 0; ok && i < native_count; ++i) {
        const CecSlot *slot = &metadata[i];
        if (!slot->size || slot->mode == 1) continue;
        uint8_t *data = malloc(slot->size);
        if (!data) {
            snprintf(message, sizeof(message), "Not enough memory to read outbox.\n");
            ok = false;
            break;
        }
        rc = cecd_get_slot(service, slot, data);
        if (R_FAILED(rc)) {
            report(message, sizeof(message), "reading local outbox", rc);
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
        report(message, sizeof(message), "ending read-only exchange", rollback);
        ok = false;
    }
    if (R_FAILED(restored)) {
        report(message, sizeof(message), "could not restore normal StreetPass", restored);
        strncat(message, "Restart the console before using StreetPass.\n",
            sizeof(message) - strlen(message) - 1);
        ok = false;
    }
    if (!ok || !count) {
        size_t used = strlen(message);
        if (ok) {
            snprintf(message + used, sizeof(message) - used, "No valid public outboxes to export.\n");
            used = strlen(message);
        }
        if (skipped)
            snprintf(message + used, sizeof(message) - used,
                "Skipped %u invalid or friends-only slot(s).", skipped);
        ui_message(message);
        goto done;
    }

    unsigned selected = 0;
    char labels[SL_TITLES][48];
    const char *items[SL_TITLES];
    for (u32 i = 0; i < count; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "%08lX  (%lu bytes)",
            (unsigned long)valid[i].slot.title, (unsigned long)valid[i].slot.size);
        items[i] = labels[i];
    }
    char details[96] = "";
    if (skipped) snprintf(details, sizeof(details), "Skipped %u invalid or friends-only slot(s).", skipped);
    while (ui_running()) {
        uint32_t keys = menu_ui_prompt("Diagnostic slot export", details, items, count,
            &selected, KEY_A | KEY_B, "Up/Down Choose  A Select  B Back  START Exit");
        if (!keys || (keys & KEY_B)) break;
        if (!(keys & KEY_A)) continue;
        char path[sizeof(EXPORT_DIR "/test-12345678.slot")];
        snprintf(path, sizeof(path), EXPORT_DIR "/test-%08lx.slot",
            (unsigned long)valid[selected].slot.title);
        bool replace;
        if (!check_destination(path, &replace)) {
            ui_message("Cannot use this SD destination.");
            continue;
        }
        if (!ui_confirm(replace ? "Replace existing file?" : "Export native outbox?",
            path, "Contains the game's original StreetPass data. Share only with consent.")) continue;
        if (save_slot(path, &valid[selected], replace)) {
            snprintf(message, sizeof(message), "Saved %s", path);
            ui_message(message);
        } else if (ui_running()) {
            ui_message("Could not write slot file to SD.");
        }
        break;
    }
 done:
    for (u32 i = 0; i < count; ++i) free(valid[i].data);
    return R_SUCCEEDED(restored);
}
