#include "pair.h"
#include <string.h>

static bool hex_code(const char *s, size_t n) {
    return strlen(s) == n && strspn(s, "0123456789abcdef") == n;
}

bool pair_parse(const unsigned char *payload, size_t length, PairData *out) {
    static const char prefix[] = "streetless1|";
    if (!payload || !out || length <= sizeof(prefix) - 1 || length > 320 ||
        memcmp(payload, prefix, sizeof(prefix) - 1) || memchr(payload, 0, length)) return false;
    PairData parsed = {0};
    char port[6];
    char *fields[] = {parsed.config.host, parsed.config.ipv4, port, parsed.code};
    const size_t capacities[] = {sizeof(parsed.config.host), sizeof(parsed.config.ipv4), sizeof(port), sizeof(parsed.code)};
    size_t pos = sizeof(prefix) - 1;
    for (size_t i = 0; i < 4; ++i) {
        size_t start = pos;
        while (pos < length && payload[pos] != '|') ++pos;
        size_t n = pos - start;
        if (!n || n >= capacities[i] || (i < 3 && pos == length) || (i == 3 && pos != length)) return false;
        memcpy(fields[i], payload + start, n);
        fields[i][n] = 0;
        if (i < 3) ++pos;
    }
    unsigned number = 0;
    for (const char *p = port; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        number = number * 10 + (unsigned)(*p - '0');
    }
    if (!number || number > 65535 || !hex_code(parsed.code, 32)) return false;
    parsed.config.port = (uint16_t)number;
    if (!config_endpoint_valid(&parsed.config)) return false;
    *out = parsed;
    return true;
}

#ifdef __3DS__
#include "http.h"
#include "pair_store.h"
#include "ui.h"
#include "quirc.h"
#include <3ds.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAIR_PATH "sdmc:/3ds/streetless/pair.pending"
#define PAIR_TMP "sdmc:/3ds/streetless/pair.pending.tmp"
#define CONFIG_TMP "sdmc:/3ds/streetless/config.txt.tmp"
#define WIDTH 320
#define HEIGHT 240

static bool remove_file(const char *path) { return unlink(path) == 0 || errno == ENOENT; }

static void save_error(const char *message, const PairStoreError *error) {
    char text[240];
    snprintf(text, sizeof(text), "%s\nSD %s failed (errno %d).\nRetry from Pair console.",
             message, error->step, error->error);
    ui_message(text);
}

static bool finish_pairing(Config *config, const PairData *data) {
    *config = data->config;
    if (!remove_file(PAIR_TMP) || !remove_file(PAIR_PATH))
        ui_message("Config saved, but recovery file remains.\nRemove pair.pending from SD when safe.");
    else ui_message("Console paired. Your configuration is saved.");
    return true;
}


/* 0 absent, 1 valid, -1 inaccessible or damaged: never overwrite it. */
static int load_pending(PairData *data, bool *debug) {
    FILE *f = fopen(PAIR_PATH, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;
    char line[38];
    bool ok = fgets(line, sizeof(line), f) != NULL;
    if (ok) {
        char *code = line;
        if (strlen(line) == 35 && line[1] == ':' && (line[0] == 'D' || line[0] == 'P')) {
            *debug = line[0] == 'D';
            code += 2;
        } else {
            *debug = false; /* Existing recovery files were always HTTPS. */
        }
        ok = strlen(code) == 33 && code[32] == '\n';
        if (ok) {
            code[32] = 0;
            ok = hex_code(code, 32) && config_read(f, &data->config);
            if (ok) memcpy(data->code, code, 33);
        }
    }
    if (fclose(f) != 0) ok = false;
    return ok ? 1 : -1;
}

static bool keyboard(const char *prompt, char *value, size_t cap) {
    SwkbdState state;
    swkbdInit(&state, SWKBD_TYPE_QWERTY, 2, (int)cap - 1);
    swkbdSetHintText(&state, prompt);
    swkbdSetInitialText(&state, value);
    return swkbdInputText(&state, value, cap) == SWKBD_BUTTON_CONFIRM;
}

static bool manual(PairData *data, bool debug) {
    char port[6], payload[321];
    snprintf(port, sizeof(port), "%s", debug ? "8080" : "443");
    *data = (PairData){0};
    if (!keyboard("Server hostname (no scheme)", data->config.host, sizeof(data->config.host)) ||
        !keyboard("Server IPv4 address", data->config.ipv4, sizeof(data->config.ipv4)) ||
        !keyboard(debug ? "HTTP port" : "HTTPS port", port, sizeof(port)) ||
        !keyboard("32-character pairing code", data->code, sizeof(data->code))) return false;
    int n = snprintf(payload, sizeof(payload), "streetless1|%s|%s|%s|%s",
        data->config.host, data->config.ipv4, port, data->code);
    PairData parsed;
    if (n < 0 || (size_t)n >= sizeof(payload) || !pair_parse((unsigned char *)payload, (size_t)n, &parsed)) {
        ui_message("Invalid hostname, IPv4, port or code.");
        return false;
    }
    *data = parsed;
    return true;
}

static bool scan(PairData *data) {
    struct quirc *decoder = quirc_new();
    uint16_t *frame = linearAlloc(WIDTH * HEIGHT * sizeof(*frame));
    bool initialized = false, active = false, capturing = false, found = false, error = false;
    u32 unit = 0;
    if (!decoder || !frame || quirc_resize(decoder, WIDTH, HEIGHT) < 0) { error = true; goto done; }
    if (R_FAILED(camInit())) { error = true; goto done; }
    initialized = true;
    if (R_FAILED(CAMU_SetSize(SELECT_OUT1, SIZE_QVGA, CONTEXT_A)) ||
        R_FAILED(CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A)) ||
        R_FAILED(CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15)) ||
        R_FAILED(CAMU_SetTrimming(PORT_CAM1, false)) ||
        R_FAILED(CAMU_GetMaxBytes(&unit, WIDTH, HEIGHT)) || !unit || unit > 32767 ||
        R_FAILED(CAMU_SetTransferBytes(PORT_CAM1, unit, WIDTH, HEIGHT)) ||
        R_FAILED(CAMU_Activate(SELECT_OUT1))) { error = true; goto done; }
    active = true;
    if (R_FAILED(CAMU_ClearBuffer(PORT_CAM1)) || R_FAILED(CAMU_StartCapture(PORT_CAM1))) { error = true; goto done; }
    capturing = true;
    consoleClear();
    puts("Streetless / Scan QR\n\nPoint the outer camera at the code.\n\nB Cancel   START Exit");
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    while (aptMainLoop()) {
        hidScanInput();
        u32 keys = hidKeysDown();
        if (keys & (KEY_B | KEY_START)) break;
        Handle event = 0;
        if (R_FAILED(CAMU_SetReceiving(&event, frame, PORT_CAM1, WIDTH * HEIGHT * 2, (s16)unit))) { error = true; break; }
        Result rc = svcWaitSynchronization(event, 1000000000LL);
        svcCloseHandle(event);
        if (R_FAILED(rc)) { error = true; break; }
        GSPGPU_InvalidateDataCache(frame, WIDTH * HEIGHT * 2);
        unsigned char *image = quirc_begin(decoder, NULL, NULL);
        u8 *screen = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        for (int y = 0; y < HEIGHT; ++y) for (int x = 0; x < WIDTH; ++x) {
            uint16_t p = frame[y * WIDTH + x];
            unsigned r = (p & 31) << 3, g = ((p >> 5) & 63) << 2, b = (p >> 11) << 3;
            image[y * WIDTH + x] = (unsigned char)((77 * r + 150 * g + 29 * b) >> 8);
            size_t v = (size_t)(x * HEIGHT + HEIGHT - 1 - y) * 3;
            screen[v] = r; screen[v + 1] = g; screen[v + 2] = b;
        }
        quirc_end(decoder);
        for (int i = 0; i < quirc_count(decoder); ++i) {
            struct quirc_code code;
            struct quirc_data decoded;
            quirc_extract(decoder, i, &code);
            if (quirc_decode(&code, &decoded) == QUIRC_SUCCESS && decoded.payload_len > 0 &&
                pair_parse(decoded.payload, (size_t)decoded.payload_len, data)) { found = true; break; }
        }
        gfxFlushBuffers();
        gspWaitForVBlank();
        if (found) break;
    }
done:
    if (capturing) CAMU_StopCapture(PORT_CAM1);
    if (active) CAMU_Activate(SELECT_NONE);
    if (initialized) camExit();
    if (frame) linearFree(frame);
    if (decoder) quirc_destroy(decoder);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    if (error) ui_message("Camera unavailable or capture failed.\nUse the one-time code instead.");
    return found;
}

static bool generate_token(char token[65]) {
    unsigned char bytes[32];
    static const char digits[] = "0123456789abcdef";
    if (R_FAILED(psInit())) return false;
    Result result = PS_GenerateRandomBytes(bytes, sizeof(bytes));
    psExit();
    if (R_FAILED(result)) return false;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        token[2 * i] = digits[bytes[i] >> 4];
        token[2 * i + 1] = digits[bytes[i] & 15];
        bytes[i] = 0;
    }
    token[64] = 0;
    return true;
}

bool pair_run(Config *config, const char *debug_pin) {
    PairData data;
    bool pending_debug = false;
    int pending = load_pending(&data, &pending_debug);
    if (pending < 0) {
        ui_message("Cannot read pending pairing on SD.\nDo not overwrite it; check the SD card.");
        return false;
    }
    if (pending) {
        consoleClear();
        printf("Streetless / Pending pairing\n\n%s:%u\n\nA Retry saved claim\nX Discard saved claim\nB Cancel   START Exit\n",
            data.config.host, (unsigned)data.config.port);
        uint32_t keys;
        do { keys = ui_keys(); } while (keys && !(keys & (KEY_A | KEY_B | KEY_X)));
        if (!keys || (keys & KEY_B)) return false;
        if (keys & KEY_X) {
            if (ui_confirm("Discard claim", data.config.host,
                    "The server may already have changed keys.\nDiscard only if you will use a new code.")) {
                if (!remove_file(PAIR_TMP) || !remove_file(PAIR_PATH))
                    ui_message("Cannot remove pending pairing from SD.");
                else ui_message("Saved claim discarded. Create a new code.");
            }
            return false;
        }
        if (pending_debug != (debug_pin != NULL)) {
            ui_message("Saved pairing belongs to the other mode.\nSwitch modes to retry or discard it.");
            return false;
        }
        PairStoreError recovery_error;
        int recovered = pair_store_recover(CONFIG_TMP, STREETLESS_CONFIG_PATH, &data, &recovery_error);
        if (recovered < 0) {
            save_error("Cannot recover saved config.", &recovery_error);
            return false;
        }
        if (recovered > 0) return finish_pairing(config, &data);
    } else {
        consoleClear();
        puts("Streetless / Pair console\n\nA Scan QR\nY Enter one-time code\nB Cancel   START Exit");
        uint32_t keys;
        do { keys = ui_keys(); } while (keys && !(keys & (KEY_A | KEY_Y | KEY_B)));
        if (!keys || (keys & KEY_B)) return false;
        if (!((keys & KEY_A) ? scan(&data) : manual(&data, debug_pin != NULL))) return false;
        char target[290];
        snprintf(target, sizeof(target), "%s:%u\nIPv4: %s", data.config.host,
            (unsigned)data.config.port, data.config.ipv4);
        if (!ui_confirm("Confirm relay", target,
                "Only continue if this is your trusted server.\nPairing replaces its existing console key.")) return false;
        if (!generate_token(data.config.token)) { ui_message("Secure random generator unavailable."); return false; }
        if ((mkdir("sdmc:/3ds", 0700) != 0 && errno != EEXIST) ||
            (mkdir("sdmc:/3ds/streetless", 0700) != 0 && errno != EEXIST)) {
            ui_message("Cannot create pairing directory on SD.\nNo request was sent."); return false;
        }
        PairStoreError pending_error;
        if (!pair_store_save(PAIR_TMP, PAIR_PATH, &data, true, debug_pin != NULL, &pending_error)) {
            save_error("Cannot save pairing recovery state.\nNo request was sent.", &pending_error);
            return false;
        }
    }
    Http http;
    if (!http_open(&http, &data.config, "romfs:/certs.pem", debug_pin)) {
        ui_message("Cannot initialize networking.\nRetry the saved pairing later."); return false;
    }
    char body[110], response[256];
    int len = snprintf(body, sizeof(body), "code=%s&token=%s", data.code, data.config.token);
    size_t received;
    long status;
    CURLcode rc = http_pair(&http, body, (size_t)len, response, sizeof(response), &received, &status);
    http_close(&http);
    if (rc != CURLE_OK || status != 204) {
        char message[240];
        if (rc != CURLE_OK) snprintf(message, sizeof(message), "Pairing: %s\nRetry the saved claim before it expires.", http_error(rc));
        else snprintf(message, sizeof(message), "Pairing failed (HTTP %ld).\nRetry the saved claim before it expires.", status);
        ui_message(message);
        return false;
    }
    PairStoreError config_error;
    if (!pair_store_save(CONFIG_TMP, STREETLESS_CONFIG_PATH, &data, false, debug_pin != NULL, &config_error)) {
        save_error("Pairing succeeded; config save failed.", &config_error);
        return false;
    }
    return finish_pairing(config, &data);
}
#endif
