// Folder sidebar policies and native TreeView UI. File I/O remains owned by Notepad3.
#include "Helpers.h"
#include <shlwapi.h>
#include "Sidebar.h"
#include "Notepad3.h"
#include "Dialogs.h"
#include "DarkMode/DarkMode.h"

static HWND s_tree = NULL;
static HWND s_splitter = NULL;
static int _Sidebar_ClampWidth(int logicalWidth, int clientWidth, UINT dpi);

// Resize only while this splitter owns mouse capture. Coordinates are converted
// to the parent so dragging remains stable when the child moves under the mouse.
static LRESULT CALLBACK _Sidebar_SplitterProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(data);
    switch (message) {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return TRUE;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd) {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            HWND const parent = GetParent(hwnd);
            MapWindowPoints(hwnd, parent, &point, 1);
            RECT client;
            GetClientRect(parent, &client);
            UINT const dpi = Scintilla_GetWindowDPI(parent);
            int const pixels = _Sidebar_ClampWidth(MulDiv(point.x, 96, dpi), client.right, dpi);
            Settings.SidebarWidth = MulDiv(pixels, 96, dpi);
            UpdateUI(parent);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, _Sidebar_SplitterProc, 0);
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

typedef enum { SW_SAME_FILE, SW_SAVE_SILENT, SW_ASK_UNTITLED, SW_NO_SAVE } SwitchAction;

// Decide whether switching may proceed or must first save. Inputs describe the
// target identity, current dirty state, and whether the current document has a path.
static SwitchAction _Sidebar_DecideSwitch(bool bSameFile, bool bSaveNeeded, bool bHasPath)
{
    if (bSameFile) {
        return SW_SAME_FILE;
    }
    if (!bSaveNeeded) {
        return SW_NO_SAVE;
    }
    return bHasPath ? SW_SAVE_SILENT : SW_ASK_UNTITLED;
}

// Compare non-null entry names in Explorer order; directory flags take precedence.
// A negative/zero/positive result means before/equal/after, ignoring name case.
static int _Sidebar_CompareEntries(bool bDirectory1, LPCWSTR name1, bool bDirectory2, LPCWSTR name2)
{
    if (bDirectory1 != bDirectory2) {
        return bDirectory1 ? -1 : 1;
    }
    return StrCmpLogicalW(name1, name2);
}

// Convert a logical requested width to physical pixels and reserve 200 logical
// pixels for the editor. Extremely narrow windows retain the 100-pixel minimum.
static int _Sidebar_ClampWidth(int logicalWidth, int clientWidth, UINT dpi)
{
    int const minimum = MulDiv(100, dpi, 96);
    int const available = clientWidth - MulDiv(200, dpi, 96);
    int const maximum = available < minimum ? minimum : available;
    int const requested = MulDiv(logicalWidth, dpi, 96);
    return requested < minimum ? minimum : (requested > maximum ? maximum : requested);
}

bool Sidebar_Create(HWND parent)
{
    s_tree = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, NULL,
                           WS_CHILD | WS_CLIPSIBLINGS | TVS_HASBUTTONS | TVS_HASLINES |
                           TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                           0, 0, 0, 0, parent, (HMENU)IDC_SIDEBAR_TREE, Globals.hInstance, NULL);
    s_splitter = CreateWindowEx(0, WC_STATIC, NULL, WS_CHILD | WS_CLIPSIBLINGS,
                               0, 0, 0, 0, parent, (HMENU)IDC_SIDEBAR_SPLIT, Globals.hInstance, NULL);
    if (!s_tree || !s_splitter || !SetWindowSubclass(s_splitter, _Sidebar_SplitterProc, 0, 0)) {
        if (s_tree) { DestroyWindow(s_tree); }
        if (s_splitter) { DestroyWindow(s_splitter); }
        s_tree = s_splitter = NULL;
        return false;
    }
    TreeView_SetExtendedStyle(s_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    Sidebar_OnThemeChanged();
    Sidebar_Show(Settings.ShowSidebar);
    return true;
}

void Sidebar_Show(bool visible)
{
    if (!s_tree) {
        return;
    }
    if (!visible && (GetFocus() == s_tree || GetFocus() == s_splitter)) {
        SetFocus(Globals.hwndEdit);
    }
    ShowWindow(s_tree, visible ? SW_SHOWNA : SW_HIDE);
    ShowWindow(s_splitter, visible ? SW_SHOWNA : SW_HIDE);
}

void Sidebar_Layout(RECT* available)
{
    if (!s_tree || !Settings.ShowSidebar) {
        return;
    }
    UINT const dpi = Scintilla_GetWindowDPI(GetParent(s_tree));
    int const width = _Sidebar_ClampWidth(Settings.SidebarWidth, available->right - available->left, dpi);
    int const split = MulDiv(4, dpi, 96);
    int const height = max_i(0, available->bottom - available->top);
    SetWindowPos(s_tree, NULL, available->left, available->top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(s_splitter, NULL, available->left + width, available->top, split, height, SWP_NOZORDER | SWP_NOACTIVATE);
    available->left += width + split;
}

void Sidebar_OnThemeChanged(void)
{
    if (!s_tree) {
        return;
    }
    InitWindowCommon(s_tree, true);
    InitTreeView(s_tree);
#ifdef D_NP3_WIN10_DARK_MODE
    AllowDarkModeForWindowEx(s_tree, UseDarkMode());
    AllowDarkModeForWindowEx(s_splitter, UseDarkMode());
#endif
}
