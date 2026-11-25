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

#define DETAIL_COLUMNS 44
#define DETAIL_ROWS 5

/* Consume one display line without losing long words or embedded newlines. */
static const char *detail_line(const char *p, char line[DETAIL_COLUMNS + 1]) {
    size_t n = 0, last_space = 0;
    while (p[n] && p[n] != '\n' && n < DETAIL_COLUMNS) {
        unsigned char c = (unsigned char)p[n];
        line[n] = c >= 32 && c <= 126 ? (char)c : ' ';
        if (line[n] == ' ') last_space = n + 1;
        ++n;
    }
    if (n == DETAIL_COLUMNS && p[n] && p[n] != '\n' && last_space)
        n = last_space;
    line[n] = '\0';
    p += n;
    if (*p == '\n') ++p;
    return p;
}

static const char *detail_page(const char *p, char lines[DETAIL_ROWS][DETAIL_COLUMNS + 1]) {
    for (size_t i = 0; i < DETAIL_ROWS; ++i) {
        lines[i][0] = '\0';
        if (*p) p = detail_line(p, lines[i]);
    }
    return p;
}

static const char *page_start(const char *details, size_t page) {
    char lines[DETAIL_ROWS][DETAIL_COLUMNS + 1];
    const char *p = details ? details : "";
    while (page-- && *p) p = detail_page(p, lines);
    return p;
}

static size_t page_count(const char *details) {
    char lines[DETAIL_ROWS][DETAIL_COLUMNS + 1];
    const char *p = details ? details : "";
    size_t count = 1;
    while (*(p = detail_page(p, lines))) ++count;
    return count;
}

static void draw_menu(C3D_RenderTarget *top, C3D_RenderTarget *bottom,
                      C2D_TextBuf buf, const char *title, const char *details,
                      const char *const *items, size_t count, unsigned selected,
                      const char *footer, size_t page, size_t pages) {
    const u32 ink = COLOR(22, 40, 34), muted = COLOR(76, 102, 91);
    const u32 green = COLOR(0, 108, 79), white = COLOR(255, 255, 255);
    const u32 pale = COLOR(240, 245, 242), border = COLOR(210, 224, 215);
    size_t first = selected >= VISIBLE_ROWS ? selected - VISIBLE_ROWS + 1 : 0;
    char position[64], lines[DETAIL_ROWS][DETAIL_COLUMNS + 1];
    detail_page(page_start(details, page), lines);
    C2D_TextBufClear(buf);
    C2D_TargetClear(top, pale);
    C2D_SceneBegin(top);
    rect(0, 0, 400, 8, green);
    label(buf, "STREETLESS", 20, 14, 360, 0.43f, green);
    label(buf, title, 20, 37, 360, 0.65f, ink);
    rect(20, 70, 360, 1, border);
    for (size_t i = 0; i < DETAIL_ROWS; ++i)
        label(buf, lines[i], 20, 77 + i * 17, 360, 0.49f, muted);
    if (count) {
        label(buf, "SELECTED", 20, 169, 360, 0.35f, muted);
        label(buf, items[selected], 20, 184, 360, 0.52f, ink);
    }
    if (pages > 1) {
        snprintf(position, sizeof(position), "DETAILS %zu / %zu   LEFT/RIGHT", page + 1, pages);
        label(buf, position, 20, 220, 360, 0.38f, muted);
    } else if (count) {
        snprintf(position, sizeof(position), "%u / %zu", selected + 1, count);
        label(buf, position, 20, 220, 360, 0.38f, muted);
    }

    C2D_TargetClear(bottom, COLOR(229, 238, 232));
    C2D_SceneBegin(bottom);
    rect(0, 0, 320, 5, green);
    for (size_t row = 0; row < VISIBLE_ROWS && first + row < count; ++row) {
        size_t index = first + row;
        float y = 13.0f + row * 37.0f;
        bool active = index == selected;
        rect(12, y, 296, 32, active ? green : white);
        if (active) rect(12, y, 4, 32, COLOR(110, 221, 174));
        label(buf, items[index], 25, y + 6, 260, 0.55f, active ? white : ink);
    }
    rect(12, 204, 296, 1, border);
    label(buf, footer, 14, 214, 292, 0.43f, muted);
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

static uint32_t input_action(uint32_t keys, size_t count, unsigned *selected,
                             size_t *page, size_t pages, uint32_t accepted) {
    if (!keys) return 0;
    if (keys & accepted & KEY_B) return KEY_B;
    if (keys & accepted & KEY_A) {
        /* For a modal confirmation, show every page before accepting. */
        if (!count && *page + 1 < pages) { ++*page; return 0; }
        return KEY_A;
    }
    if (keys & accepted & KEY_X) return KEY_X;
    if (keys & accepted & KEY_Y) return KEY_Y;
    if (count && (keys & KEY_UP))
        *selected = *selected ? *selected - 1 : (unsigned)(count - 1);
    else if (count && (keys & KEY_DOWN))
        *selected = *selected + 1 < count ? *selected + 1 : 0;
    else if ((keys & KEY_LEFT) && *page) --*page;
    else if ((keys & KEY_RIGHT) && *page + 1 < pages) ++*page;
    return 0;
}

static uint32_t text_menu(const char *title, const char *details,
                          const char *const *items, size_t count, unsigned *selected,
                          uint32_t accepted, const char *footer, size_t page, size_t pages) {
    while (ui_running()) {
        char safe[97], lines[DETAIL_ROWS][DETAIL_COLUMNS + 1];
        detail_page(page_start(details, page), lines);
        consoleClear();
        clean_text(safe, sizeof(safe), title);
        printf("Graphics unavailable / %s\n\n", safe);
        for (size_t i = 0; i < DETAIL_ROWS; ++i) puts(lines[i]);
        if (pages > 1) printf("Details %zu / %zu  LEFT/RIGHT\n", page + 1, pages);
        size_t first = count && *selected >= VISIBLE_ROWS ? *selected - VISIBLE_ROWS + 1 : 0;
        for (size_t i = first; i < count && i < first + VISIBLE_ROWS; ++i) {
            clean_text(safe, sizeof(safe), items[i]);
            printf("%s %s\n", i == *selected ? ">" : " ", safe);
        }
        clean_text(safe, sizeof(safe), footer);
        puts(safe);
        uint32_t keys = ui_keys();
        if (!keys) return 0;
        uint32_t action = input_action(keys, count, selected, &page, pages, accepted);
        if (action) return action;
    }
    return 0;
}

/* Keep Citro3D's APT hook installed while system applets temporarily own
 * the GPU. Its restore hook marks shaders and GPU state dirty on return. */
static struct {
    C3D_RenderTarget *top, *bottom;
    C2D_TextBuf text;
    bool c3d, c2d, ready, unavailable;
} graphics;

void menu_ui_shutdown(void) {
    if (graphics.text) C2D_TextBufDelete(graphics.text);
    if (graphics.top) C3D_RenderTargetDelete(graphics.top);
    if (graphics.bottom) C3D_RenderTargetDelete(graphics.bottom);
    if (graphics.c2d) C2D_Fini();
    if (graphics.c3d) C3D_Fini();
    graphics.text = NULL;
    graphics.top = graphics.bottom = NULL;
    graphics.c3d = graphics.c2d = graphics.ready = false;
}

static bool graphics_init(void) {
    if (graphics.ready) return true;
    if (!(graphics.c3d = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE))) return false;
    if (!(graphics.c2d = C2D_Init(C2D_DEFAULT_MAX_OBJECTS))) return false;
    C2D_Prepare();
    graphics.top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    graphics.bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    graphics.text = C2D_TextBufNew(2048);
    graphics.ready = graphics.top && graphics.bottom && graphics.text;
    return graphics.ready;
}

uint32_t menu_ui_prompt(const char *title, const char *details,
                        const char *const *items, size_t count, unsigned *selected,
                        uint32_t accepted_keys, const char *footer) {
    bool failed = false;
    uint32_t action = 0;
    size_t page = 0, pages;

    if (count > UINT_MAX || (count && (!items || !selected))) return 0;
    if (count && *selected >= count) *selected = 0;
    pages = page_count(details);
    if (graphics.unavailable)
        return text_menu(title, details, items, count, selected, accepted_keys, footer, page, pages);
    /* consoleInit uses RGB565 and single buffering. Citro2D's screen
     * targets output RGB8; reset both screens before any GPU transfer. */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    if (!graphics_init()) goto error;

    while (ui_running()) {
        if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) goto error;
        draw_menu(graphics.top, graphics.bottom, graphics.text, title, details, items, count,
                  count ? *selected : 0, footer, page, pages);
        C3D_FrameEnd(0);
        C3D_FrameSync();
        uint32_t keys = ui_keys();
        if (!keys) break;
        action = input_action(keys, count, selected, &page, pages, accepted_keys);
        if (action) break;
    }
    goto cleanup;

error:
    failed = true;
cleanup:
    /* Begin waits for the prior transfer. End the empty frame before
     * switching to CPU-rendered console output. */
    if (graphics.ready && C3D_FrameBegin(0)) C3D_FrameEnd(0);
    if (failed) menu_ui_shutdown();
    restore_console();
    if (failed && ui_running()) {
        graphics.unavailable = true;
        return text_menu(title, details, items, count, selected, accepted_keys, footer, page, pages);
    }
    return action;
}

bool menu_ui_choose(const char *title, const char *status,
                    const char *const *items, size_t count,
                    unsigned *selected, bool allow_back) {
    if (!count) return false;
    return menu_ui_prompt(title, status, items, count, selected,
                          KEY_A | (allow_back ? KEY_B : 0),
                          allow_back ? "A Select    B Back    START Exit"
                                     : "A Select    START Exit") == KEY_A;
}
