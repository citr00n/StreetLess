#pragma once
#include <3ds.h>
#include <stddef.h>
typedef struct { int id; } C3D_RenderTarget;
typedef void *C2D_TextBuf;
typedef struct { const char *str; } C2D_Text;
#define C3D_DEFAULT_CMDBUF_SIZE 1
#define C2D_DEFAULT_MAX_OBJECTS 1
#define C3D_FRAME_SYNCDRAW 0
#define C2D_WithColor 0
#define C2D_Color32(r,g,b,a) ((u32)(r) | ((u32)(g)<<8) | ((u32)(b)<<16) | ((u32)(a)<<24))
bool C3D_Init(int); bool C2D_Init(int); void C2D_Prepare(void);
C3D_RenderTarget *C2D_CreateScreenTarget(int,int); C2D_TextBuf C2D_TextBufNew(size_t); void C2D_TextBufClear(C2D_TextBuf); void C2D_TextBufDelete(C2D_TextBuf);
void C2D_TextParse(C2D_Text *, C2D_TextBuf, const char *); void C2D_TextGetDimensions(C2D_Text *,float,float,float *,float *); void C2D_DrawText(C2D_Text *,int,float,float,float,float,float,u32);
void C2D_DrawRectSolid(float,float,float,float,float,u32); void C2D_TargetClear(C3D_RenderTarget *,u32); void C2D_SceneBegin(C3D_RenderTarget *);
bool C3D_FrameBegin(int); void C3D_FrameEnd(int); void C3D_FrameSync(void); void C3D_RenderTargetDelete(C3D_RenderTarget *); void C2D_Fini(void); void C3D_Fini(void);
