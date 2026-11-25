#include <3ds.h>

#include "export.h"
#include <string.h>
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

static bool enter_debug_pin(char pin[5]) {
    if (!ui_confirm("Debug mode", NULL,
        "Isolated non-production use only.\n"
        "HTTP is plaintext. A LAN attacker can\n"
        "impersonate the server and read traffic.\n"
        "Never use production credentials.")) return false;
    SwkbdState state;
    char entered[5] = "";
    swkbdInit(&state, SWKBD_TYPE_NUMPAD, 2, 4);
    swkbdSetHintText(&state, "Four-digit server debug PIN");
    if (swkbdInputText(&state, entered, sizeof(entered)) != SWKBD_BUTTON_CONFIRM) return false;
    if (!http_debug_pin_valid(entered)) {
        ui_message("PIN must be exactly four digits.");
        return false;
    }
    memcpy(pin, entered, sizeof(entered));
    return true;
}

static void run_menu(Config *config, bool valid) {
    static const char *const items[] = {"Sync", "Privacy", "Friends", "Rooms", "Outgoing", "Pair console", "Debug mode", "Export local slot"};
    static const SocialPage pages[] = {SOCIAL_PRIVACY, SOCIAL_FRIENDS, SOCIAL_ROOMS, SOCIAL_OUTGOING};
    Http http;
    char pin[5] = "";
    bool debug = false;
    bool connected = valid && http_open(&http, config, "romfs:/certs.pem", NULL);
    unsigned selected = valid ? 0 : 5;
    bool consented = false, sync_disabled = false;
    while (ui_running()) {
        unsigned count = debug ? 8 : 7;
        consoleClear();
        printf("Streetless\n\n%s\n\n", debug ? "DEBUG: plaintext HTTP" : "Production: verified HTTPS");
        for (unsigned i = 0; i < count; ++i)
            printf("%s %s%s\n", selected == i ? ">" : " ", items[i],
                   i == 0 && sync_disabled ? " (restart console)" : "");
        puts("\nD-pad Select   A Open   START Exit");
        uint32_t keys = ui_keys();
        if (keys & KEY_UP) selected = (selected + count - 1) % count;
        else if (keys & KEY_DOWN) selected = (selected + 1) % count;
        else if (keys & KEY_A) {
            if (selected == 6) {
                if (debug) {
                    consoleClear();
                    puts("Debug mode\n\nA Re-enter server PIN\nX Return to production HTTPS\nB Cancel");
                    uint32_t k;
                    do { k = ui_keys(); } while (k && !(k & (KEY_A | KEY_B | KEY_X)));
                    if (k & KEY_X) { debug = false; memset(pin, 0, sizeof(pin)); }
                    else if (k & KEY_A) { if (!enter_debug_pin(pin)) continue; }
                    else continue;
                } else {
                    if (!enter_debug_pin(pin)) continue;
                    debug = true;
                }
                if (connected) { http_close(&http); connected = false; }
                if (valid) connected = http_open(&http, config, "romfs:/certs.pem", debug ? pin : NULL);
            } else if (selected == 7) {
                if (sync_disabled) ui_message("Restart the console before using CECD again.");
                else if (!export_run()) sync_disabled = true;
            } else if (selected == 5) {
                if (connected) { http_close(&http); connected = false; }
                if (pair_run(config, debug ? pin : NULL)) valid = true;
                if (valid) connected = http_open(&http, config, "romfs:/certs.pem", debug ? pin : NULL);
            } else if (!valid) {
                ui_message("Pair this console first, or install an existing\nconfig.txt in /3ds/streetless/.");
            } else if (!connected) {
                connected = http_open(&http, config, "romfs:/certs.pem", debug ? pin : NULL);
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
    memset(pin, 0, sizeof(pin));
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
