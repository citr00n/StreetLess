#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
enum { GFX_TOP=0, GFX_BOTTOM=1, GFX_LEFT=0, GSP_BGR8_OES=0, GSP_RGB565_OES=1, KEY_UP=1, KEY_DOWN=2, KEY_A=4, KEY_B=8, KEY_X=16, KEY_Y=32, KEY_LEFT=64, KEY_RIGHT=128 };
u8 *gfxGetFramebuffer(int, int, u16 *, u16 *);
void gfxSetScreenFormat(int,int); void gfxSetDoubleBuffering(int,bool);
void gfxFlushBuffers(void); void gfxScreenSwapBuffers(int, bool); void consoleInit(int, void *); void consoleClear(void);
