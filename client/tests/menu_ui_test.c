#include "menu_ui.h"
#include <3ds.h>
#include <citro2d.h>
#include <assert.h>
#include <string.h>

static int format[2] = {GSP_RGB565_OES, GSP_BGR8_OES};
static bool double_buffered[2] = {false, true};
static C3D_RenderTarget targets[2] = {{0}, {1}};
static u8 pixels[320 * 240 * 3];
static unsigned inputs[] = {KEY_DOWN, KEY_DOWN, KEY_A, KEY_B,
                            KEY_RIGHT, KEY_LEFT, KEY_RIGHT, KEY_X, KEY_Y, 0, KEY_A, KEY_A, KEY_A | KEY_B};
static size_t input_index;
static int frames, initializations, shutdowns;
static bool apt_restore_hook;
static bool saw_first_page, saw_second_page, saw_indicator;

bool ui_running(void) { return true; }
u32 ui_keys(void) { assert(input_index < sizeof(inputs)/sizeof(inputs[0])); return inputs[input_index++]; }
void ui_message(const char *message) { (void)message; assert(0); }
u8 *gfxGetFramebuffer(int screen, int side, u16 *width, u16 *height) {
    assert(screen == GFX_BOTTOM && side == GFX_LEFT); *width = 240; *height = 320; return pixels;
}
void gfxSetScreenFormat(int screen, int value) { format[screen] = value; }
void gfxSetDoubleBuffering(int screen, bool value) { double_buffered[screen] = value; }
void gfxFlushBuffers(void) {}
void gfxScreenSwapBuffers(int screen, bool stereo) { (void)screen; (void)stereo; }
void consoleInit(int screen, void *console) { (void)console; format[screen] = GSP_RGB565_OES; double_buffered[screen] = false; }
void consoleClear(void) {}
bool C3D_Init(int size) { (void)size; ++initializations; apt_restore_hook = true; return true; }
bool C2D_Init(int size) { (void)size; return true; }
void C2D_Prepare(void) {}
C3D_RenderTarget *C2D_CreateScreenTarget(int screen, int side) { (void)side; return &targets[screen]; }
C2D_TextBuf C2D_TextBufNew(size_t size) { (void)size; return &targets[0]; }
void C2D_TextBufClear(C2D_TextBuf buf) { (void)buf; }
void C2D_TextBufDelete(C2D_TextBuf buf) { (void)buf; }
void C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *value) { (void)buf; text->str=value; }
void C2D_TextGetDimensions(C2D_Text *text, float x, float y, float *width, float *height) { (void)y; if (width) *width=(float)strlen(text->str)*x; if (height) *height=0; }
void C2D_DrawText(C2D_Text *text, int flags, float x, float y, float z, float sx, float sy, u32 color) {
    if (!strcmp(text->str, "first page")) saw_first_page = true;
    if (!strcmp(text->str, "second page")) saw_second_page = true;
    if (strstr(text->str, "DETAILS 2 / 2")) saw_indicator = true;
    (void)flags;(void)x;(void)y;(void)z;(void)sx;(void)sy;(void)color;
}
void C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 color) { (void)x;(void)y;(void)z;(void)w;(void)h;(void)color; }
void C2D_TargetClear(C3D_RenderTarget *target, u32 color) { (void)target;(void)color; }
void C2D_SceneBegin(C3D_RenderTarget *target) { (void)target; }
bool C3D_FrameBegin(int flags) {
    (void)flags;
    /* Citro2D transfers RGB8 to both targets: RGB565 makes the top image corrupt. */
    assert(format[GFX_TOP] == GSP_BGR8_OES && format[GFX_BOTTOM] == GSP_BGR8_OES);
    assert(double_buffered[GFX_TOP] && double_buffered[GFX_BOTTOM]);
    if (flags & C3D_FRAME_SYNCDRAW) ++frames;
    return true;
}
void C3D_FrameEnd(int flags) { (void)flags; }
void C3D_FrameSync(void) {}
void C3D_RenderTargetDelete(C3D_RenderTarget *target) { (void)target; }
void C2D_Fini(void) {}
void C3D_Fini(void) { ++shutdowns; apt_restore_hook = false; }
int main(void) {
    const char *items[] = {"Sync", "Settings", "Account"};
    unsigned selected = 0;
    assert(menu_ui_choose("Streetless", "HTTPS", items, 3, &selected, false));
    assert(selected == 2 && frames == 3);
    assert(format[GFX_TOP] == GSP_RGB565_OES && !double_buffered[GFX_TOP]);
    /* The APT restore hook must remain installed across the system keyboard. */
    assert(initializations == 1 && shutdowns == 0 && apt_restore_hook);
    /* Entering a second menu must switch formats again; B returns to the previous screen. */
    assert(!menu_ui_choose("Account", "HTTPS", items, 3, &selected, true));
    assert(format[GFX_TOP] == GSP_RGB565_OES && !double_buffered[GFX_TOP]);
    assert(menu_ui_prompt("Details", "first page\n2\n3\n4\n5\nsecond page",
                          NULL, 0, NULL, KEY_X, "X Continue") == KEY_X);
    assert(saw_first_page && saw_second_page && saw_indicator);
    assert(format[GFX_TOP] == GSP_RGB565_OES && !double_buffered[GFX_TOP]);
    selected = 99;
    assert(menu_ui_prompt("Actions", "", items, 3, &selected, KEY_Y, "Y Action") == KEY_Y);
    assert(selected == 0);
    assert(menu_ui_prompt("Exit", "", NULL, 0, NULL, KEY_A, "A Continue") == 0);
    saw_first_page = saw_second_page = false;
    assert(menu_ui_prompt("Confirm", "first page\n2\n3\n4\n5\nsecond page",
                          NULL, 0, NULL, KEY_A | KEY_B, "A Confirm") == KEY_A);
    assert(saw_first_page && saw_second_page);
    assert(menu_ui_prompt("Cancel", "", NULL, 0, NULL, KEY_A | KEY_B, "A Confirm B Cancel") == KEY_B);
    assert(initializations == 1 && shutdowns == 0 && apt_restore_hook);
    menu_ui_shutdown();
    assert(shutdowns == 1 && !apt_restore_hook);
    return 0;
}
