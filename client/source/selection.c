#include "selection.h"
#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t checksum(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

bool selection_parse(const uint8_t *data, size_t size, Selection *selection) {
    if (!data || !selection || size < 12 || memcmp(data, "SPS1", 4)) return false;
    uint32_t count = read32(data + 4);
    if (count > SELECTION_MAX || size != 12u + (size_t)count * 4u ||
        checksum(data, size - 4) != read32(data + size - 4)) return false;
    Selection parsed = {0};
    parsed.count = count;
    for (size_t i = 0; i < count; ++i) {
        parsed.ids[i] = read32(data + 8 + i * 4);
        if (!parsed.ids[i]) return false;
        for (size_t j = 0; j < i; ++j) if (parsed.ids[j] == parsed.ids[i]) return false;
    }
    *selection = parsed;
    return true;
}

size_t selection_encode(const Selection *selection, uint8_t *data, size_t capacity) {
    if (!selection || !data || selection->initial || selection->count > SELECTION_MAX) return 0;
    size_t size = 12 + selection->count * 4;
    if (capacity < size) return 0;
    memcpy(data, "SPS1", 4);
    write32(data + 4, (uint32_t)selection->count);
    for (size_t i = 0; i < selection->count; ++i) {
        if (!selection->ids[i]) return 0;
        for (size_t j = 0; j < i; ++j) if (selection->ids[j] == selection->ids[i]) return 0;
        write32(data + 8 + i * 4, selection->ids[i]);
    }
    write32(data + size - 4, checksum(data, size - 4));
    return size;
}

bool selection_allows(const Selection *selection, uint32_t id) {
    if (!selection || !id) return false;
    if (selection->initial) return true;
    for (size_t i = 0; i < selection->count; ++i) if (selection->ids[i] == id) return true;
    return false;
}

bool selection_set(Selection *selection, uint32_t id, bool enabled) {
    if (!selection || selection->initial || !id) return false;
    for (size_t i = 0; i < selection->count; ++i) {
        if (selection->ids[i] != id) continue;
        if (!enabled) selection->ids[i] = selection->ids[--selection->count];
        return true;
    }
    if (!enabled) return true;
    if (selection->count == SELECTION_MAX) return false;
    selection->ids[selection->count++] = id;
    return true;
}

#if !defined(STREETLESS_SELECTION_HOST_TEST) || defined(STREETLESS_SELECTION_STORAGE_TEST)
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define SELECTION_DIR "sdmc:/3ds/streetless"
#define SELECTION_PATH SELECTION_DIR "/outgoing.bin"
#define SELECTION_TMP SELECTION_DIR "/outgoing.bin.tmp"
#define SELECTION_MARKER SELECTION_DIR "/outgoing.protected"

static bool ensure_dir(const char *path);
static bool save_failed;

bool selection_load(Selection *selection) {
    if (save_failed) return false;
    if (!ensure_dir("sdmc:/3ds") || !ensure_dir(SELECTION_DIR)) return false;
    FILE *file = fopen(SELECTION_PATH, "rb");
    if (!file) {
        if (errno != ENOENT) return false;
        struct stat marker;
        /* A missing committed file is only a first run if no selection has
         * ever been saved. A failed FAT rename must never re-enable all. */
        if (stat(SELECTION_MARKER, &marker) == 0 || errno != ENOENT) return false;
        *selection = (Selection){.initial = true};
        return true;
    }
    uint8_t data[SELECTION_BYTES + 1];
    size_t size = fread(data, 1, sizeof(data), file);
    bool ok = !ferror(file) && feof(file);
    if (fclose(file) != 0) ok = false;
    return ok && selection_parse(data, size, selection);
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    struct stat st;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool selection_save(const Selection *selection) {
    uint8_t data[SELECTION_BYTES];
    size_t size = selection_encode(selection, data, sizeof(data));
    Selection current;
    if (!size || !selection_load(&current) || !ensure_dir("sdmc:/3ds") || !ensure_dir(SELECTION_DIR)) {
        save_failed = true;
        return false;
    }
    FILE *marker = fopen(SELECTION_MARKER, "ab");
    if (!marker) { save_failed = true; return false; }
    bool protected = fflush(marker) == 0 && fsync(fileno(marker)) == 0;
    if (fclose(marker) != 0) protected = false;
    if (!protected) { save_failed = true; return false; }
    FILE *file = fopen(SELECTION_TMP, "wb");
    if (!file) { save_failed = true; return false; }
    bool ok = fwrite(data, 1, size, file) == size && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(SELECTION_TMP, SELECTION_PATH) == 0) return true;
    unlink(SELECTION_TMP);
    save_failed = true;
    return false;
}
#endif

#ifndef STREETLESS_SELECTION_HOST_TEST
#include "cecd.h"
#include "menu_ui.h"
#include "title_names.h"
#include "ui.h"

bool selection_menu(void) {
    Selection selection;
    if (!selection_load(&selection)) {
        ui_message("Outgoing preferences missing or invalid.\nSending is disabled. Restore outgoing.bin,\nor delete it and outgoing.protected to\nreset ALL games ON. Both are on the SD.");
        return true;
    }
    consoleClear();
    puts("Reading native StreetPass titles...");
    Handle service;
    Result rc = cecd_open(&service);
    if (R_FAILED(rc)) {
        ui_message("Cannot access cecd:s. Use Luma3DS and\ncurrent system software.");
        return true;
    }
    CecExchange session = {0};
    CecSlot slots[SL_TITLES];
    u32 count = 0;
    const char *phase = "opening exchange";
    rc = cecd_begin(service, &session, slots, &count, &phase);
    Result rollback, restored = cecd_finish(service, &session, &rollback);
    svcCloseHandle(service);
    if (R_FAILED(restored)) {
        ui_message("Could not restore normal StreetPass.\nRestart the console before using CECD again.");
        return false;
    }
    if (R_FAILED(rc) || R_FAILED(rollback)) {
        char message[192];
        snprintf(message, sizeof(message), "Cannot read titles (%s: %08lx; rollback: %08lx).",
            phase, (unsigned long)(u32)rc, (unsigned long)(u32)rollback);
        ui_message(message);
        return true;
    }
    if (!count) { ui_message("No native StreetPass titles found."); return true; }
    uint32_t ids[SL_TITLES];
    char names[SL_TITLES][64];
    for (u32 i = 0; i < count; ++i) ids[i] = slots[i].title;
    title_names_resolve(ids, count, names);
    if (selection.initial) {
        selection.initial = false;
        for (u32 i = 0; i < count; ++i) selection_set(&selection, ids[i], true);
    }
    unsigned cursor = 0;
    while (ui_running()) {
        char labels[SL_TITLES][96];
        const char *items[SL_TITLES];
        for (u32 i = 0; i < count; ++i) {
            snprintf(labels[i], sizeof(labels[i]), "[%s] %08lX %.63s",
                selection_allows(&selection, ids[i]) ? "ON" : "OFF",
                (unsigned long)ids[i], names[i]);
            items[i] = labels[i];
        }
        uint32_t keys = menu_ui_prompt("Outgoing games",
            "New games default OFF after saving.\nMode 0 games may not receive without an outgoing pass. "
            "Already staged or queued data cannot be retracted here.",
            items, count, &cursor, KEY_A | KEY_B | KEY_X,
            "Up/Down Choose  A Toggle  X Save  B Cancel  START Exit");
        if (!keys || (keys & KEY_B)) break;
        if (keys & KEY_A) {
            if (!selection_set(&selection, ids[cursor], !selection_allows(&selection, ids[cursor])))
                ui_message("Too many selected games; disable one first.");
        } else if (keys & KEY_X) {
            if (selection_save(&selection)) ui_message("Outgoing preferences saved on this console.");
            else ui_message("Could not save outgoing preferences.\nSending is disabled until preferences\ncan be read safely.");
            break;
        }
    }
    return true;
}
#endif
