// C boundary for the sidebar's GDI+ shapes and explicit, script-aware GDI fonts.
// All handles are owned by the UI thread; callers retain ownership of their HDC.
#pragma once
#include "TypeDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SidebarDrawing SidebarDrawing;
typedef enum SidebarFont { SidebarFontBody,
    SidebarFontSmall,
    SidebarFontTitle,
    SidebarFontBold } SidebarFont;

// Start GDI+ and create fonts at dpi. Return NULL on failure; destroy after child
// controls stop using the borrowed fonts. A replacement may coexist during DPI updates.
SidebarDrawing* SidebarDrawing_Create(UINT dpi);
void            SidebarDrawing_Destroy(SidebarDrawing* drawing);
// Borrow the body CJK font for the native EDIT, whose system caret/cue stay native.
HFONT SidebarDrawing_EditFont(const SidebarDrawing* drawing);
// Measure/draw using the same explicit font runs. Text is non-null UTF-16; drawing
// clips to bounds and uses a trailing ellipsis on overflow. Dimensions are pixels.
int  SidebarDrawing_Measure(HDC dc, const SidebarDrawing* drawing, LPCWSTR text, SidebarFont font);
void SidebarDrawing_Text(HDC dc, const SidebarDrawing* drawing, LPCWSTR text, SidebarFont font, RECT bounds, COLORREF color);
// Draw an antialiased rounded rectangle; CLR_INVALID omits its fill or 1px stroke.
// Empty bounds are ignored. Radius and coordinates are physical pixels.
void SidebarDrawing_RoundRect(HDC dc, RECT bounds, int radius, COLORREF fill, COLORREF stroke);
// Draw a plus or refresh icon (refresh=true) within a circular button's bounds.
void SidebarDrawing_Icon(HDC dc, RECT bounds, bool refresh, COLORREF color, UINT dpi);

#ifdef __cplusplus
}
#endif
