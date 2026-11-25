#include <3ds.h>

#include "background_control.h"
#include "export.h"
#include <string.h>
#include "config.h"
#include "http.h"
#include "pair.h"
#include "social.h"
#include "selection.h"
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

static bool stop_background_for_action(void) {
    Result detail;
    BackgroundStatus status = background_status(&detail);
    if (status == BACKGROUND_ABSENT) return true;
    if (status == BACKGROUND_UNCERTAIN) {
        char message[256];
        snprintf(message, sizeof(message),
            "Background state is uncertain (%08lx).\n"
            "Cannot safely use StreetPass. Retry status,\n"
            "or power off and reopen Streetless promptly.",
            (unsigned long)(u32)detail);
        ui_message(message);
        return false;
    }
    if (!ui_confirm("Stop background", NULL,
            "Background sync must stop before this\n"
            "action. Its current exchange will finish\n"
            "or abort and restore StreetPass first.")) return false;
    consoleClear();
    puts("Streetless / Background\n\nStopping safely; please wait...");
    gfxFlushBuffers();
    if (!background_stop(&detail)) {
        char message[192];
        snprintf(message, sizeof(message),
            "Stop was not confirmed (%08lx).\n"
            "No manual StreetPass operation is allowed.\n"
            "Retry stop or restart the console.",
            (unsigned long)(u32)detail);
        ui_message(message);
        return false;
    }
    return true;
}

static void background_menu(bool debug, bool valid, bool sync_disabled) {
    Result detail;
    BackgroundStatus status = background_status(&detail);
    if (status == BACKGROUND_UNCERTAIN) {
        (void)stop_background_for_action();
        return;
    }
    if (status != BACKGROUND_ABSENT) {
        const char *state = status == BACKGROUND_RETRY ? "Running; last sync needs retry." :
                            status == BACKGROUND_STOPPING ? "Stopping." : "Running.";
        if (!ui_confirm("Background sync", state,
                "Stop background sync? The current exchange\n"
                "must finish or abort before it stops.")) return;
        consoleClear();
        puts("Streetless / Background\n\nStopping safely; please wait...");
        gfxFlushBuffers();
        if (background_stop(&detail)) ui_message("Background sync stopped safely.");
        else {
            char message[160];
            snprintf(message, sizeof(message),
                "Stop not confirmed (%08lx).\n"
                "Retry, or restart the console before\n"
                "using StreetPass manually.", (unsigned long)(u32)detail);
            ui_message(message);
        }
        return;
    }
    if (debug) {
        ui_message("Background sync requires production HTTPS.\nExit debug mode before starting it.");
        return;
    }
    if (!valid) {
        ui_message("Pair the console before starting\nbackground sync.");
        return;
    }
    if (sync_disabled) {
        ui_message("StreetPass was not restored.\nRestart the console before background sync.");
        return;
    }
    if (!ui_confirm("Start background sync", NULL,
            "Selected outgoing passes will be sent\n"
            "periodically using your allowed routes.\n"
            "Sync works only while the console is awake,\n"
            "including after Streetless is closed.\n"
            "Start again manually after each boot.\n"
            "Previously queued passes cannot be recalled.")) return;
    consoleClear();
    puts("Streetless / Background\n\nStarting; please wait...");
    gfxFlushBuffers();
    if (background_start(&detail)) {
        ui_message("Background service connected.\n"
                   "Close Streetless; sync runs while awake.\n"
                   "Open Background sync to check or stop it.");
    } else {
        char message[300];
        if ((u32)detail == 0xD900182E)
            snprintf(message, sizeof(message), "Luma3DS 13 or newer is required.");
        else if ((u32)detail == 0xD900182F)
            snprintf(message, sizeof(message),
                "Background module is not installed.\n"
                "Install 0004013000F5A302.cxi in\n"
                "/luma/sysmodules/ and enable external\n"
                "modules in Luma configuration.");
        else
            snprintf(message, sizeof(message),
                "Start failed or was not confirmed (%08lx).\n"
                "Check Luma external module loading and\n"
                "the installed module. Retry status; if\n"
                "uncertain, power off and reopen promptly.",
                (unsigned long)(u32)detail);
        ui_message(message);
    }
}

static void run_menu(Config *config, bool valid) {
    static const char *const items[] = {"Sync", "Privacy", "Friends", "Rooms", "Outgoing", "Outgoing games", "Pair console", "Debug mode", "Background sync", "Export local slot"};
    static const SocialPage pages[] = {SOCIAL_PRIVACY, SOCIAL_FRIENDS, SOCIAL_ROOMS, SOCIAL_OUTGOING};
    Http http;
    char pin[5] = "";
    bool debug = false;
    bool connected = valid && http_open(&http, config, "romfs:/certs.pem", NULL);
    unsigned selected = valid ? 0 : 6;
    bool consented = false, sync_disabled = false;
    while (ui_running()) {
        unsigned count = debug ? 10 : 9;
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
            if (selected == 7) {
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
            } else if (selected == 8) {
                background_menu(debug, valid, sync_disabled);
            } else if (selected == 9) {
                if (sync_disabled) ui_message("Restart the console before using CECD again.");
                else if (stop_background_for_action() && !export_run()) sync_disabled = true;
            } else if (selected == 5) {
                if (sync_disabled) ui_message("Restart the console before using CECD again.");
                else if (stop_background_for_action() && !selection_menu()) sync_disabled = true;
            } else if (selected == 6) {
                if (!stop_background_for_action()) continue;
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
                       "Sync sends selected game passes through\n"
                       "the routes you allow. New games default off\n"
                       "after saving Outgoing games. Mode 0 may\n"
                       "prevent receipt without an outgoing pass.\n"
                       "Already staged or queued data cannot be\n"
                       "retracted by this local setting.")) {
                if (!stop_background_for_action()) continue;
                consented = true;
                consoleClear();
                puts("Streetless / Sync\n");
                gfxFlushBuffers();
                sync_disabled = sync_run(&http, config) == -1;
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
