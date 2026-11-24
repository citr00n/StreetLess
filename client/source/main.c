#include <3ds.h>

#ifdef STREETLESS_TEST_SLOT_EXPORT
#include "export.h"

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    export_run();
    gfxExit();
    return 0;
}
#else
#include "config.h"
#include "http.h"
#include "pair.h"
#include "social.h"
#include "sync.h"
#include "ui.h"

static bool running = true;

bool ui_running(void) {
    return running;
}

uint32_t ui_keys(void) {
    gfxFlushBuffers();
    while (running && aptMainLoop()) {
        hidScanInput();
        uint32_t keys = hidKeysDown();
        if (keys & KEY_START) break;
        if (keys) return keys;
        gspWaitForVBlank();
    }
    running = false;
    return 0;
}

static void wait_for_back(void) {
    uint32_t keys;
    while ((keys = ui_keys()) && !(keys & (KEY_A | KEY_B))) {}
}

void ui_message(const char *message) {
    consoleClear();
    printf("Streetless\n\n%s\n\nA/B Continue   START Exit\n", message);
    wait_for_back();
}

bool ui_confirm(const char *heading, const char *target, const char *message) {
    consoleClear();
    printf("Streetless / %s\n\n", heading);
    if (target) printf("%s\n\n", target);
    printf("%s\n\nA Confirm   B Cancel   START Exit\n", message);
    uint32_t keys;
    while ((keys = ui_keys())) {
        if (keys & KEY_B) return false;
        if (keys & KEY_A) return true;
    }
    return false;
}

static void run_menu(Config *config, bool valid) {
    static const char *const items[] = {"Sync", "Privacy", "Friends", "Rooms", "Outgoing", "Pair console"};
    static const SocialPage pages[] = {SOCIAL_PRIVACY, SOCIAL_FRIENDS, SOCIAL_ROOMS, SOCIAL_OUTGOING};
    enum { count = sizeof(items) / sizeof(items[0]) };
    Http http;
    bool connected = valid && http_open(&http, config, "romfs:/certs.pem");
    unsigned selected = valid ? 0 : count - 1;
    bool consented = false, sync_disabled = false;
    while (ui_running()) {
        consoleClear();
        puts("Streetless\n\nStreetPass over the Internet\n");
        for (unsigned i = 0; i < count; ++i)
            printf("%s %s%s\n", selected == i ? ">" : " ", items[i],
                   i == 0 && sync_disabled ? " (restart console)" : "");
        puts("\nD-pad Select   A Open   START Exit");
        uint32_t keys = ui_keys();
        if (keys & KEY_UP) selected = (selected + count - 1) % count;
        else if (keys & KEY_DOWN) selected = (selected + 1) % count;
        else if (keys & KEY_A) {
            if (selected == count - 1) {
                if (connected) { http_close(&http); connected = false; }
                if (pair_run(config)) valid = true;
                if (valid) connected = http_open(&http, config, "romfs:/certs.pem");
            } else if (!valid) {
                ui_message("Pair this console first, or install an existing\nconfig.txt in /3ds/streetless/.");
            } else if (!connected) {
                connected = http_open(&http, config, "romfs:/certs.pem");
                if (!connected) ui_message("Cannot initialize Streetless networking.");
            } else if (selected) {
                social_open(&http, pages[selected - 1]);
            } else if (sync_disabled) {
                ui_message("CECD could not return to normal operation.\nRestart the console before syncing again.");
            } else if (consented || ui_confirm("Sync", NULL,
                       "Sync sends eligible game passes through\n"
                       "the routes you allow. The public hall allows\n"
                       "at most 3 incoming passes per 24 hours.\n"
                       "Some games require a two-way exchange.")) {
                consented = true;
                consoleClear();
                puts("Streetless / Sync\n");
                gfxFlushBuffers();
                sync_disabled = sync_run(&http, config) < 0;
                puts("\nA/B Main menu   START Exit");
                wait_for_back();
            }
        }
    }
    if (connected) http_close(&http);
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    Config config;
    FILE *file = fopen(STREETLESS_CONFIG_PATH, "r");
    bool valid = file && config_read(file, &config);
    if (file) fclose(file);

    if (R_FAILED(romfsInit())) {
        ui_message("Cannot open RomFS for TLS certificates.");
    } else {
        run_menu(&config, valid);
        romfsExit();
    }
    gfxExit();
    return 0;
}
#endif
