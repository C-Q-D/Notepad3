// Sidebar drawing resources and geometry. This unit exposes only C-compatible
// functions; it does not own windows, editor state, or sidebar interaction policy.
#include "SidebarDrawing.h"
#include <objidl.h>
#include <gdiplus.h>
#include <new>

// One resource set per panel/DPI. Font columns are monospace, CJK and symbol.
struct SidebarDrawing {
    ULONG_PTR token = 0;
    HFONT     fonts[4][3] = {};
    bool      arrowInMono[4] = {};
};

// Select explicit scripts rather than relying on machine-specific font linking.
// Supplementary characters stay together and use the CJK font's glyph fallback.
static int FontColumn(const SidebarDrawing* drawing, SidebarFont font, WCHAR ch)
{
    if (ch == L'\x276f' && !drawing->arrowInMono[font]) {
        return 2;
    }
    return (ch >= 0x2e80 && ch <= 0xdfff) || (ch >= 0xf900 && ch <= 0xfaff) || ch >= 0xff00 ? 1 : 0;
}

// Count a contiguous UTF-16 font run, including surrogate pairs in one run.
static int RunLength(const SidebarDrawing* drawing, SidebarFont font, LPCWSTR text, int length)
{
    int       count = 1;
    int const column = FontColumn(drawing, font, *text);
    while (count < length && FontColumn(drawing, font, text[count]) == column) {
        ++count;
    }
    return count;
}

SidebarDrawing* SidebarDrawing_Create(UINT dpi)
{
    SidebarDrawing* drawing = new (std::nothrow) SidebarDrawing;
    if (!drawing) {
        return nullptr;
    }
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&drawing->token, &input, nullptr) != Gdiplus::Ok) {
        delete drawing;
        return nullptr;
    }
    HDC dc = GetDC(nullptr);
    if (!dc) {
        SidebarDrawing_Destroy(drawing);
        return nullptr;
    }
    for (int role = 0; role < 4; ++role) {
        int const halfPixels = role == SidebarFontTitle ? 29 : (role == SidebarFontSmall ? 23 : 25);
        int const weight = role >= SidebarFontTitle ? FW_BOLD : FW_NORMAL;
        for (int column = 0; column < 3; ++column) {
            LPCWSTR face = column == 0 ? L"Cascadia Mono" : (column == 1 ? L"Microsoft YaHei UI" : L"Segoe UI Symbol");
            HFONT   font = CreateFontW(-MulDiv(halfPixels, dpi, 192), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  DEFAULT_PITCH, face);
            if (font && column == 0) {
                HFONT const previous = (HFONT)SelectObject(dc, font);
                WCHAR       actual[LF_FACESIZE] = {};
                GetTextFaceW(dc, LF_FACESIZE, actual);
                SelectObject(dc, previous);
                if (_wcsicmp(actual, face) != 0) {
                    DeleteObject(font);
                    font = CreateFontW(-MulDiv(halfPixels, dpi, 192), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Consolas");
                }
            }
            drawing->fonts[role][column] = font;
            if (!font) {
                ReleaseDC(nullptr, dc);
                SidebarDrawing_Destroy(drawing);
                return nullptr;
            }
        }
        HFONT const previous = (HFONT)SelectObject(dc, drawing->fonts[role][0]);
        WORD        glyph = 0xffff;
        GetGlyphIndicesW(dc, L"\x276f", 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS);
        drawing->arrowInMono[role] = glyph != 0xffff;
        SelectObject(dc, previous);
    }
    ReleaseDC(nullptr, dc);
    return drawing;
}

void SidebarDrawing_Destroy(SidebarDrawing* drawing)
{
    if (drawing) {
        for (auto& role : drawing->fonts) {
            for (HFONT font : role) {
                if (font) {
                    DeleteObject(font);
                }
            }
        }
        if (drawing->token) {
            Gdiplus::GdiplusShutdown(drawing->token);
        }
        delete drawing;
    }
}

HFONT SidebarDrawing_EditFont(const SidebarDrawing* drawing)
{
    return drawing->fonts[SidebarFontBody][1];
}

int SidebarDrawing_Measure(HDC dc, const SidebarDrawing* drawing, LPCWSTR text, SidebarFont font)
{
    int           width = 0;
    int           remaining = lstrlenW(text);
    HGDIOBJ const previous = GetCurrentObject(dc, OBJ_FONT);
    while (remaining) {
        int const count = RunLength(drawing, font, text, remaining);
        SelectObject(dc, drawing->fonts[font][FontColumn(drawing, font, *text)]);
        SIZE size = {};
        GetTextExtentPoint32W(dc, text, count, &size);
        width += size.cx;
        text += count;
        remaining -= count;
    }
    SelectObject(dc, previous);
    return width;
}

void SidebarDrawing_Text(HDC dc, const SidebarDrawing* drawing, LPCWSTR text, SidebarFont font, RECT bounds, COLORREF color)
{
    int const available = bounds.right - bounds.left;
    if (available <= 0 || bounds.bottom <= bounds.top) {
        return;
    }
    int const saved = SaveDC(dc);
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    int        remaining = lstrlenW(text);
    bool const truncated = SidebarDrawing_Measure(dc, drawing, text, font) > available;
    int const  ellipsis = truncated ? SidebarDrawing_Measure(dc, drawing, L"\x2026", font) : 0;
    int        space = available - ellipsis;
    int        x = bounds.left;
    while (remaining && space > 0) {
        int const count = RunLength(drawing, font, text, remaining);
        SelectObject(dc, drawing->fonts[font][FontColumn(drawing, font, *text)]);
        int  fit = 0;
        SIZE size = {};
        GetTextExtentExPointW(dc, text, count, space, &fit, nullptr, &size);
        if (fit > 0 && fit < count && IS_HIGH_SURROGATE(text[fit - 1])) {
            --fit;
        }
        if (fit) {
            TEXTMETRICW metrics = {};
            GetTextMetricsW(dc, &metrics);
            int const y = bounds.top + (bounds.bottom - bounds.top - metrics.tmHeight) / 2;
            ExtTextOutW(dc, x, y, ETO_CLIPPED, &bounds, text, fit, nullptr);
            GetTextExtentPoint32W(dc, text, fit, &size);
            x += size.cx;
            space -= size.cx;
        }
        if (fit < count) {
            break;
        }
        text += count;
        remaining -= count;
    }
    if (truncated) {
        SelectObject(dc, drawing->fonts[font][0]);
        TEXTMETRICW metrics = {};
        GetTextMetricsW(dc, &metrics);
        ExtTextOutW(dc, x, bounds.top + (bounds.bottom - bounds.top - metrics.tmHeight) / 2,
            ETO_CLIPPED, &bounds, L"\x2026", 1, nullptr);
    }
    RestoreDC(dc, saved);
}

// Convert opaque COLORREF channels; all transparency is blended into the palette.
static Gdiplus::Color Color(COLORREF color)
{
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void SidebarDrawing_RoundRect(HDC dc, RECT bounds, int radius, COLORREF fill, COLORREF stroke)
{
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::REAL const x = (Gdiplus::REAL)bounds.left + 0.5f;
    Gdiplus::REAL const y = (Gdiplus::REAL)bounds.top + 0.5f;
    Gdiplus::REAL const width = (Gdiplus::REAL)(bounds.right - bounds.left - 1);
    Gdiplus::REAL const height = (Gdiplus::REAL)(bounds.bottom - bounds.top - 1);
    Gdiplus::REAL       diameter = (Gdiplus::REAL)(radius * 2);
    if (diameter > width) {
        diameter = width;
    }
    if (diameter > height) {
        diameter = height;
    }
    if (diameter <= 0) {
        return;
    }
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush brush(Color(fill));
        graphics.FillPath(&brush, &path);
    }
    if (stroke != CLR_INVALID) {
        Gdiplus::Pen pen(Color(stroke), 1.0f);
        graphics.DrawPath(&pen, &path);
    }
}

void SidebarDrawing_Icon(HDC dc, RECT bounds, bool refresh, COLORREF color, UINT dpi)
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::REAL const cx = (bounds.left + bounds.right) / 2.0f;
    Gdiplus::REAL const cy = (bounds.top + bounds.bottom) / 2.0f;
    Gdiplus::REAL const r = 5.0f * dpi / 96.0f;
    Gdiplus::REAL const head = 4.0f * dpi / 96.0f;
    Gdiplus::Pen        pen(Color(color), 1.5f * dpi / 96.0f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (refresh) {
        graphics.DrawArc(&pen, cx - r, cy - r, r * 2, r * 2, 30.0f, 290.0f);
        Gdiplus::PointF arrow[] = { { cx + r - head, cy - r }, { cx + r, cy - r }, { cx + r, cy - r - head } };
        graphics.DrawLines(&pen, arrow, 3);
    }
    else {
        graphics.DrawLine(&pen, cx - r, cy, cx + r, cy);
        graphics.DrawLine(&pen, cx, cy - r, cx, cy + r);
    }
}
