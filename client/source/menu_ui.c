#include "menu_ui.h"
#include "ui.h"

#include <3ds.h>
#include <citro2d.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define COLOR(r, g, b) C2D_Color32((r), (g), (b), 255)
#define VISIBLE_ROWS 5

/* Only printable ASCII is used in menu labels. This also prevents control
 * sequences and malformed UTF-8 from reaching the system font parser. */
static void clean_text(char *out, size_t capacity, const char *in) {
    size_t n = 0;
    if (!in) in = "";
    while (*in && n + 1 < capacity) {
        unsigned char c = (unsigned char)*in++;
        out[n++] = c >= 32 && c <= 126 ? (char)c : ' ';
    }
    if (*in && capacity >= 4) {
        out[n - 3] = '.';
        out[n - 2] = '.';
        out[n - 1] = '.';
    }
    out[n] = '\0';
}

static void rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

/* Fit each bounded, single-line label to its actual pixel width. */
static void label(C2D_TextBuf buf, const char *value, float x, float y,
                  float max_width, float scale, u32 color) {
    char safe[97];
    C2D_Text text;
    float width;
    clean_text(safe, sizeof(safe), value);
    C2D_TextParse(&text, buf, safe);
    C2D_TextGetDimensions(&text, scale, scale, &width, NULL);
    if (width > max_width) scale *= max_width / width;
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static void draw_menu(C3D_RenderTarget *top, C3D_RenderTarget *bottom,
                      C2D_TextBuf buf, const char *title, const char *status,
                      const char *const *items, size_t count, unsigned selected,
                      bool allow_back) {
    const u32 ink = COLOR(22, 40, 34), muted = COLOR(76, 102, 91);
    const u32 green = COLOR(0, 108, 79), white = COLOR(255, 255, 255);
    const u32 pale = COLOR(240, 245, 242), border = COLOR(210, 224, 215);
    size_t first = selected >= VISIBLE_ROWS ? selected - VISIBLE_ROWS + 1 : 0;
    char position[40];

    char status_text[85], status_line[43];
    clean_text(status_text, sizeof(status_text), status);
    size_t status_len = strlen(status_text);
    size_t split = status_len > 42 ? 42 : status_len;
    if (split < status_len) {
        while (split > 0 && status_text[split] != ' ') --split;
        if (!split) split = 42;
    }
    memcpy(status_line, status_text, split);
    status_line[split] = '\0';
    C2D_TextBufClear(buf);
    C2D_TargetClear(top, pale);
    C2D_SceneBegin(top);
    rect(0, 0, 400, 8, green);
    label(buf, "STREETLESS", 20, 18, 360, 0.48f, green);
    label(buf, title, 20, 46, 360, 0.78f, ink);
    rect(20, 86, 360, 1, border);
    label(buf, status_line, 20, 97, 360, 0.49f, muted);
    if (split < status_len) {
        const char *rest = status_text + split;
        if (*rest == ' ') ++rest;
        label(buf, rest, 20, 115, 360, 0.49f, muted);
    }
    label(buf, "SELECTED", 20, 145, 360, 0.40f, muted);
    label(buf, items[selected], 20, 165, 360, 0.67f, ink);
    snprintf(position, sizeof(position), "%u / %zu", selected + 1, count);
    label(buf, position, 20, 207, 110, 0.40f, muted);
    label(buf, "D-PAD TO NAVIGATE", 195, 207, 185, 0.40f, muted);

    C2D_TargetClear(bottom, COLOR(229, 238, 232));
    C2D_SceneBegin(bottom);
    rect(0, 0, 320, 5, green);
    for (size_t row = 0; row < VISIBLE_ROWS && first + row < count; ++row) {
        size_t index = first + row;
        float y = 13.0f + row * 37.0f;
        bool active = index == selected;
        rect(12, y, 296, 32, active ? green : white);
        if (active) rect(12, y, 4, 32, COLOR(110, 221, 174));
        label(buf, items[index], 25, y + 6, 260, 0.55f,
              active ? white : ink);
    }
    rect(12, 204, 296, 1, border);
    label(buf, allow_back ? "A Select    B Back    START Exit"
                          : "A Select                 START Exit",
          14, 214, 292, 0.43f, muted);
}

static void restore_console(void) {
    /* Clear both bottom buffers: consoleInit also swaps the bottom screen. */
    u16 width, height;
    for (unsigned i = 0; i < 2; ++i) {
        u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height);
        if (fb) memset(fb, 0, (size_t)width * height * 3);
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
    }
    consoleInit(GFX_TOP, NULL);
    consoleClear();
}

static bool text_menu(const char *title, const char *status, const char *const *items,
                      size_t count, unsigned *selected, bool allow_back) {
    while (ui_running()) {
        consoleClear();
        printf("Streetless / %s\n\n%s\n\n", title, status);
        size_t first = *selected >= VISIBLE_ROWS ? *selected - VISIBLE_ROWS + 1 : 0;
        for (size_t i = first; i < count && i < first + VISIBLE_ROWS; ++i)
            printf("%s %s\n", i == *selected ? ">" : " ", items[i]);
        puts(allow_back ? "\nD-pad Select  A Open  B Back  START Exit"
                        : "\nD-pad Select  A Open  START Exit");
        uint32_t keys = ui_keys();
        if (!keys || (allow_back && (keys & KEY_B))) return false;
        if (keys & KEY_UP) *selected = *selected ? *selected - 1 : (unsigned)(count - 1);
        else if (keys & KEY_DOWN) *selected = *selected + 1 < count ? *selected + 1 : 0;
        else if (keys & KEY_A) return true;
    }
    return false;
}

bool menu_ui_choose(const char *title, const char *status,
                    const char *const *items, size_t count,
                    unsigned *selected, bool allow_back) {
    C3D_RenderTarget *top = NULL, *bottom = NULL;
    C2D_TextBuf buf = NULL;
    bool c3d = false, c2d = false, chosen = false, failed = false;

    if (!items || !selected || !count || count > UINT_MAX) return false;
    if (*selected >= count) *selected = 0;
    static bool graphics_unavailable;
    if (graphics_unavailable) return text_menu(title, status, items, count, selected, allow_back);
    /* consoleInit uses RGB565 and single buffering. Citro2D's screen
     * targets output RGB8; reset both screens before any GPU transfer. */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    if (!(c3d = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE))) goto error;
    if (!(c2d = C2D_Init(C2D_DEFAULT_MAX_OBJECTS))) goto error;
    C2D_Prepare();
    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    buf = C2D_TextBufNew(2048);
    if (!top || !bottom || !buf) goto error;

    while (ui_running()) {
        if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) goto error;
        draw_menu(top, bottom, buf, title, status, items, count, *selected, allow_back);
        C3D_FrameEnd(0);
        C3D_FrameSync();
        uint32_t keys = ui_keys();
        if (!keys) break;
        if (keys & KEY_UP) *selected = *selected ? *selected - 1 : (unsigned)(count - 1);
        else if (keys & KEY_DOWN) *selected = *selected + 1 < count ? *selected + 1 : 0;
        else if (keys & KEY_A) { chosen = true; break; }
        else if (allow_back && (keys & KEY_B)) break;
    }
    goto cleanup;

error:
    failed = true;
cleanup:
    if (c3d) C3D_FrameSync();
    if (buf) C2D_TextBufDelete(buf);
    if (top) C3D_RenderTargetDelete(top);
    if (bottom) C3D_RenderTargetDelete(bottom);
    if (c2d) C2D_Fini();
    if (c3d) C3D_Fini();
    restore_console();
    if (failed && ui_running()) {
        graphics_unavailable = true;
        ui_message("Graphical menu unavailable.\nUsing text navigation.");
        return text_menu(title, status, items, count, selected, allow_back);
    }
    return chosen;
}
