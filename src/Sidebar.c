// Folder sidebar panel, native tree and navigation policies. File I/O remains
// owned by Notepad3. Tree rows keep filesystem names separate from their drawing.
#include "Helpers.h"
#include <shlwapi.h>
#include <stdlib.h>
#include "Sidebar.h"
#include "SidebarDrawing.h"
#include "PathLib.h"
#include "Notepad3.h"
#include "MuiLanguage.h"
#include "Dialogs.h"
#include "DarkMode/DarkMode.h"
#include "Config/Config.h"

static HWND s_tree = NULL;
static HWND s_splitter = NULL;
static HWND s_panel = NULL;
static HWND s_filter = NULL;
static HWND s_tooltip = NULL;
static SidebarDrawing* s_drawing = NULL;
static UINT s_dpi = 96;
static int s_hover = 0;
static WCHAR s_hint[MAX_PATH_EXPLICIT];
static RECT s_filterRect, s_newRect, s_refreshRect, s_hiddenRect, s_sortRect;
static int _Sidebar_ClampWidth(int logicalWidth, int clientWidth, UINT dpi);
static void _Sidebar_UpdateChrome(void);
static int _Sidebar_Pixels(int logical);
static HTREEITEM _Sidebar_RowAt(POINT point);
static void _Sidebar_ActivateRow(HTREEITEM item, POINT point);
static HTREEITEM s_recent = NULL, s_folders = NULL, s_currentItem = NULL, s_hotItem = NULL, s_pressedItem = NULL;
static HTREEITEM s_recentCurrentItem = NULL;
static int s_recentCount = 0;
static UINT s_rootDirectories = 0, s_rootFiles = 0;
static bool s_restoreGroupCollapse = false;

// Opaque colors include the specified alpha blended against each local surface.
typedef struct SidebarPalette {
    COLORREF island, border, text, title, secondary, accent, button, buttonHot;
    COLORREF filter, filterFocus, filterHotBorder, filterFocusBorder, status, divider, textHot;
    COLORREF weak, prefix, folder, ini, source, binary, selected, rowHot;
    COLORREF hint;
} SidebarPalette;
static SidebarPalette const s_darkPalette = {
    RGB(18,20,18), RGB(32,34,32), RGB(201,209,201), RGB(238,243,238), RGB(139,148,158), RGB(126,231,135),
    RGB(27,31,27), RGB(37,43,37), RGB(26,30,26), RGB(21,25,21), RGB(38,48,38), RGB(63,92,65),
    RGB(15,17,15), RGB(27,32,27), RGB(28,32,28),
    RGB(125,134,125), RGB(63,74,63), RGB(121,192,255), RGB(210,168,255), RGB(255,166,87), RGB(139,148,158), RGB(31,38,31), RGB(24,28,24), RGB(0xE6,0xC0,0x7B)
};
static SidebarPalette const s_lightPalette = {
    RGB(251,252,251), RGB(226,227,226), RGB(36,41,47), RGB(31,35,40), RGB(87,96,106), RGB(26,127,55),
    RGB(236,238,236), RGB(227,234,227), RGB(236,238,236), RGB(238,242,238), RGB(196,204,196), RGB(26,127,55),
    RGB(239,242,239), RGB(225,229,225), RGB(227,234,227),
    RGB(110,119,129), RGB(196,204,196), RGB(9,105,218), RGB(130,80,223), RGB(188,76,0), RGB(140,149,159), RGB(227,234,227), RGB(238,242,238), RGB(0x9A,0x67,0x00)
};
static SidebarPalette const* s_palette = &s_lightPalette;

// Replace the transient footer message on the UI thread. NULL/empty clears it.
// The panel name exposes the same visible status to assistive technology.
static void _Sidebar_SetHint(LPCWSTR text)
{
    StringCchCopy(s_hint, COUNTOF(s_hint), text ? text : L"");
    if (s_panel) {
        SetWindowText(s_panel, s_hint);
        InvalidateRect(s_panel, NULL, FALSE);
    }
}

// Clear transient status on the next key or button press, including native
// scrollbar/non-client input. Callers still dispatch the message normally so
// scrolling, focus and control-specific mouse behavior remain owned by Windows.
static void _Sidebar_ClearHintForInput(UINT message)
{
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDBLCLK:
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
    case WM_NCXBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDBLCLK:
    case WM_NCXBUTTONDBLCLK:
        if (*s_hint) { _Sidebar_SetHint(NULL); }
        break;
    }
}

// Map persisted sort modes to the existing localized footer labels.
static UINT _Sidebar_SortLabelId(void)
{
    return Settings.SidebarSortMode == SIDEBAR_SORT_EXTENSION ? IDS_MUI_SIDEBAR_SORT_EXTENSION :
           Settings.SidebarSortMode == SIDEBAR_SORT_MODIFIED ? IDS_MUI_SIDEBAR_SORT_MODIFIED : IDS_MUI_SIDEBAR_SORT_NAME;
}

int Sidebar_GetStrokeWidth(UINT dpi)
{
    return max_i(1, MulDiv(1, dpi, 96));
}

// Main-window gaps must share the same background as the panel's outer corners.
static COLORREF _Sidebar_Background(void)
{
    return UseDarkMode() ? RGB(12,13,12) : GetSysColor(COLOR_WINDOW);
}

// Own full-row mouse activation so native label/button hit zones cannot toggle
// directories twice. Keyboard navigation remains in the native TreeView.
static LRESULT CALLBACK _Sidebar_TreeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(data);
    _Sidebar_ClearHintForInput(message);
    POINT const point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (message == WM_LBUTTONDOWN) {
        SetFocus(hwnd);
        s_pressedItem = _Sidebar_RowAt(point);
        if (s_pressedItem) { TreeView_SelectItem(hwnd, s_pressedItem); }
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        HTREEITEM const pressed = s_pressedItem;
        s_pressedItem = NULL;
        if (pressed && pressed == _Sidebar_RowAt(point)) { _Sidebar_ActivateRow(pressed, point); }
        return 0;
    }
    if (message == WM_LBUTTONDBLCLK) { return 0; }
    if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
        HTREEITEM const hot = message == WM_MOUSELEAVE ? NULL : _Sidebar_RowAt(point);
        if (s_hotItem != hot) { s_hotItem = hot; InvalidateRect(hwnd, NULL, FALSE); }
        if (message == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT tracking = { sizeof(tracking), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tracking);
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, _Sidebar_TreeProc, 0);
        s_tree = NULL;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

// Low four bits hold kind/loading state; upper bits store directory count+1
// (zero means uncounted, 1000 means 999+) or a recent row's 1-based number.
// Tree deletion owns all metadata, without allocations.
enum { SIDEBAR_FILE = 0, SIDEBAR_UNLOADED = 1, SIDEBAR_LOADED = 2, SIDEBAR_RECENT = 3, SIDEBAR_EMPTY = 4,
       SIDEBAR_RECENT_ITEM = 5, SIDEBAR_RECENT_EMPTY = 6,
       SIDEBAR_KIND_MASK = 15, SIDEBAR_COUNT_SHIFT = 4, SIDEBAR_COUNT_LIMIT = 1000, SIDEBAR_COUNT_FOLDERS = 256 };

// Read native text and kind into the caller's explicit WCHAR capacity. Directory
// names fit MAX_PATH; recent text holds a full path and needs PATHLONG_MAX_CCH.
// False means that the item no longer exists; no returned buffer is borrowed.
static bool _Sidebar_GetItem(HTREEITEM item, LPWSTR name, int capacity, LPARAM* kind)
{
    TVITEMW value = { 0 };
    value.mask = TVIF_TEXT | TVIF_PARAM;
    value.hItem = item;
    value.pszText = name;
    value.cchTextMax = capacity;
    bool const found = TreeView_GetItem(s_tree, &value) != FALSE;
    *kind = value.lParam & SIDEBAR_KIND_MASK;
    return found;
}

// Return an owned path for a directory-tree item or a recent-file row. The
// directory group resolves to its root. NULL, presentation-only rows and other
// groups yield an empty handle; their display text must never become a
// filesystem component.
static HPATHL _Sidebar_ItemPath(HTREEITEM item)
{
    HPATHL result = Path_Allocate(NULL);
    if (!item) {
        return result;
    }
    if (TreeView_GetParent(s_tree, item) == s_recent) {
        LPWSTR const buffer = Path_WriteAccessBuf(result, PATHLONG_MAX_CCH);
        LPARAM kind;
        if (_Sidebar_GetItem(item, buffer, (int)Path_GetBufCount(result), &kind) && kind == SIDEBAR_RECENT_ITEM) {
            Path_Sanitize(result);
        } else { Path_Empty(result, false); }
        return result;
    }
    HSTRINGW relative = StrgCreate(NULL);
    HTREEITEM const root = s_folders;
    while (item && item != root) {
        WCHAR name[MAX_PATH_EXPLICIT];
        LPARAM kind;
        if (!_Sidebar_GetItem(item, name, COUNTOF(name), &kind) ||
            (kind != SIDEBAR_FILE && kind != SIDEBAR_UNLOADED && kind != SIDEBAR_LOADED)) {
            StrgDestroy(relative);
            return result;
        }
        if (StrgGetLength(relative)) {
            StrgInsertCh(relative, 0, L'\\');
        }
        StrgInsert(relative, 0, name);
        item = TreeView_GetParent(s_tree, item);
    }
    if (item != root || !root) {
        StrgDestroy(relative);
        return result;
    }
    Path_Reset(result, Path_Get(Settings.SidebarFolder));
    if (StrgGetLength(relative)) {
        Path_Append(result, StrgGet(relative));
    }
    StrgDestroy(relative);
    return result;
}

// Insert one enumerated name under parent; directory nodes get a lazy-load marker.
// The returned handle belongs to TreeView and is invalidated by collapse/reset.
static HTREEITEM _Sidebar_Insert(HTREEITEM parent, LPCWSTR name, bool directory)
{
    TVINSERTSTRUCTW entry = { 0 };
    entry.hParent = parent;
    entry.hInsertAfter = TVI_LAST;
    entry.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_INTEGRAL;
    entry.item.pszText = (LPWSTR)name;
    entry.item.lParam = directory ? SIDEBAR_UNLOADED : SIDEBAR_FILE;
    entry.item.cChildren = directory ? 1 : 0;
    entry.itemex.iIntegral = parent == TVI_ROOT ? 3 : 2;
    return TreeView_InsertItem(s_tree, &entry);
}

// Remove actual children without expanding the parent or processing user commands.
static void _Sidebar_ClearChildren(HTREEITEM parent)
{
    HTREEITEM child;
    while ((child = TreeView_GetChild(s_tree, parent)) != NULL) {
        TreeView_DeleteItem(s_tree, child);
    }
}

// Programmatic collapse may omit the native notifications after the first expand.
// Reset both the lazy-load marker and children for every collapse entry point.
static void _Sidebar_CollapseDirectory(HTREEITEM item)
{
    if (item == s_folders) { Settings.SidebarFoldersCollapsed = true; }
    TVITEMW state = { 0 };
    state.mask = TVIF_PARAM;
    state.hItem = item;
    TreeView_GetItem(s_tree, &state);
    state.mask |= TVIF_CHILDREN;
    state.lParam = (state.lParam & ~SIDEBAR_KIND_MASK) | SIDEBAR_UNLOADED;
    state.cChildren = 1;
    TreeView_SetItem(s_tree, &state);
    TreeView_Expand(s_tree, item, TVE_COLLAPSE | TVE_COLLAPSERESET);
    _Sidebar_ClearChildren(item);
}

// Resize only while this splitter owns mouse capture. Coordinates are converted
// to the parent so dragging remains stable when the child moves under the mouse.
static LRESULT CALLBACK _Sidebar_SplitterProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(data);
    switch (message) {
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC const dc = BeginPaint(hwnd, &paint);
        SetDCBrushColor(dc, _Sidebar_Background());
        FillRect(dc, &paint.rcPaint, GetStockObject(DC_BRUSH));
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return TRUE;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        // Capture can outlive the button release; ignore moves without a held button.
        if (GetCapture() == hwnd && (wParam & MK_LBUTTON)) {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            HWND const parent = GetParent(hwnd);
            MapWindowPoints(hwnd, parent, &point, 1);
            RECT client;
            GetClientRect(parent, &client);
            UINT const dpi = Scintilla_GetWindowDPI(parent);
            // Persist the island width, excluding its left margin and the drag gap.
            int const overhead = MulDiv(UseDarkMode() ? 24 : 16, dpi, 96) + (UseDarkMode() ? 2 * Sidebar_GetStrokeWidth(dpi) : 0);
            int const pixels = _Sidebar_ClampWidth(MulDiv(point.x - MulDiv(8, dpi, 96), 96, dpi), client.right - overhead, dpi);
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
        s_splitter = NULL;
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

// Save policy for replacing the current document; SW_SAME_FILE must not reload it.
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

// Compare non-null names and unsigned last-write ticks using SIDEBAR_SORT_* mode.
// Directories precede files; equal extensions/times and unknown modes use natural
// case-insensitive name order. Return negative/zero/positive for before/equal/after.
static inline int _Sidebar_CompareSortedEntries(bool bDirectory1, LPCWSTR name1, ULONGLONG modified1, bool bDirectory2, LPCWSTR name2, ULONGLONG modified2, int sortMode)
{
    if (bDirectory1 != bDirectory2) {
        return bDirectory1 ? -1 : 1;
    }
    if (sortMode == SIDEBAR_SORT_EXTENSION) {
        int const extensionOrder = CompareStringOrdinal(PathFindExtensionW(name1), -1, PathFindExtensionW(name2), -1, TRUE) - CSTR_EQUAL;
        if (extensionOrder) {
            return extensionOrder;
        }
    } else if (sortMode == SIDEBAR_SORT_MODIFIED && modified1 != modified2) {
        // Subtraction could overflow or truncate the 64-bit timestamp difference.
        return modified1 > modified2 ? -1 : 1;
    }
    return _Sidebar_CompareEntries(bDirectory1, name1, bDirectory2, name2);
}

// Build a root-to-leaf prefix from ancestorLast[0..ancestorCount) and this row's
// last-sibling flag. ancestorLast may be NULL only at depth zero. output must not
// alias the inputs; cchOutput includes NUL. Return false and clear writable output
// if storage is insufficient or inputs are invalid; never emit a partial prefix.
static inline bool _Sidebar_BuildPrefix(const bool* ancestorLast, size_t ancestorCount, bool last, LPWSTR output, size_t cchOutput)
{
    if (!output || !cchOutput) {
        return false;
    }
    output[0] = L'\0';
    if ((ancestorCount && !ancestorLast) || cchOutput < 4 || ancestorCount > (cchOutput - 4) / 3) {
        return false;
    }
    for (size_t i = 0; i < ancestorCount; ++i) {
        output[3 * i] = ancestorLast[i] ? L' ' : L'\x2502';
        output[3 * i + 1] = output[3 * i + 2] = L' ';
    }
    size_t const end = 3 * ancestorCount;
    output[end] = last ? L'\x2514' : L'\x251c';
    output[end + 1] = L'\x2500';
    output[end + 2] = L' ';
    output[end + 3] = L'\0';
    return true;
}

// Classify a non-null filename by its final extension only, ignoring ordinal case.
// No filesystem access or content sniffing: absent, bare-dot and unknown extensions
// remain text candidates. The list follows the sidebar's editor-opening policy.
static inline bool _Sidebar_IsBinary(LPCWSTR name)
{
    static LPCWSTR const extensions[] = {
        L".exe", L".dll", L".mui", L".sys", L".obj", L".lib", L".pdb", L".ocx", L".cpl", L".drv", L".scr", L".com", L".bin",
        L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".ico", L".webp", L".tif", L".tiff",
        L".zip", L".7z", L".rar", L".gz", L".tar", L".xz", L".cab", L".msi", L".iso"
    };
    LPCWSTR const extension = PathFindExtensionW(name);
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        if (CompareStringOrdinal(extension, -1, extensions[i], -1, TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

// Return the first case-insensitive ordinal match in UTF-16 code units, or -1.
// name and query are non-null; an empty query matches at zero. Ordinal matching
// handles Unicode case without stripping accents or depending on the user locale.
static inline int _Sidebar_MatchName(LPCWSTR name, LPCWSTR query)
{
    return *query ? FindStringOrdinal(FIND_FROMSTART, name, -1, query, -1, TRUE) : 0;
}

// Classify 96-DPI logical width: 0 shows all; 1 hides Editing, recent folder and
// root counts; 2 also hides header buttons, status label, hidden toggle and sort.
// Widths below the minimum still use tier 2; layout owns enforcing that minimum.
static inline int _Sidebar_WidthTier(int logicalWidth)
{
    return logicalWidth < 160 ? 2 : (logicalWidth < 220 ? 1 : 0);
}

// Apply the persisted logical range, convert to physical pixels and reserve 200
// logical pixels for the editor. Extremely narrow windows retain the minimum.
static int _Sidebar_ClampWidth(int logicalWidth, int clientWidth, UINT dpi)
{
    int const minimum = MulDiv(SIDEBAR_MIN_WIDTH, dpi, 96);
    int const available = clientWidth - MulDiv(200, dpi, 96);
    int const limit = MulDiv(SIDEBAR_MAX_WIDTH, dpi, 96);
    int const maximum = available < minimum ? minimum : (available > limit ? limit : available);
    int const requested = MulDiv(logicalWidth, dpi, 96);
    return requested < minimum ? minimum : (requested > maximum ? maximum : requested);
}

// qsort adapter: folders first, then the persisted name/extension/newest order.
static int __cdecl _Sidebar_CompareFindData(const void* left, const void* right)
{
    WIN32_FIND_DATAW const* a = left;
    WIN32_FIND_DATAW const* b = right;
    ULONGLONG const modifiedA = ((ULONGLONG)a->ftLastWriteTime.dwHighDateTime << 32) | a->ftLastWriteTime.dwLowDateTime;
    ULONGLONG const modifiedB = ((ULONGLONG)b->ftLastWriteTime.dwHighDateTime << 32) | b->ftLastWriteTime.dwLowDateTime;
    return _Sidebar_CompareSortedEntries((a->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, a->cFileName, modifiedA,
                                         (b->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, b->cFileName, modifiedB, Settings.SidebarSortMode);
}

// Count one level using the visibility setting, stopping at 1000 (999+). An
// inaccessible directory returns -1, omitting its count rather than claiming empty.
static int _Sidebar_CountChildren(const HPATHL folder)
{
    HPATHL pattern = Path_Copy(folder);
    Path_Append(pattern, L"*");
    WIN32_FIND_DATAW data;
    HANDLE const search = Path_FindFirstFile(pattern, &data);
    DWORD const error = GetLastError();
    Path_Release(pattern);
    if (search == INVALID_HANDLE_VALUE) { return error == ERROR_FILE_NOT_FOUND ? 0 : -1; }
    int count = 0;
    do {
        if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..") &&
            (Settings.SidebarShowHidden || !(data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))) { ++count; }
    } while (count < SIDEBAR_COUNT_LIMIT && FindNextFileW(search, &data));
    bool const success = count == SIDEBAR_COUNT_LIMIT || GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    return success ? count : -1;
}

// Enumerate visible direct entries in the persisted sort order, then count one
// level per child directory when there are at most 256. No recursion or paint I/O.
static bool _Sidebar_LoadChildren(HTREEITEM parent)
{
    HPATHL pattern = _Sidebar_ItemPath(parent);
    Path_Append(pattern, L"*");
    WIN32_FIND_DATAW data;
    HANDLE const search = Path_FindFirstFile(pattern, &data);
    DWORD const firstError = GetLastError();
    Path_Release(pattern);
    if (search == INVALID_HANDLE_VALUE && firstError != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    HCURSOR const cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    SendMessage(s_tree, WM_SETREDRAW, FALSE, 0);
    WIN32_FIND_DATAW* entries = NULL;
    size_t count = 0, capacity = 0;
    bool success = true;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0 ||
                (!Settings.SidebarShowHidden && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))) {
                continue;
            }
            if (count == capacity) {
                size_t const next = capacity ? (capacity <= SIZE_MAX / 2 ? capacity * 2 : 0) : 64;
                WIN32_FIND_DATAW* grown = next && next <= SIZE_MAX / sizeof(*entries) ? ReAllocMem(entries, next * sizeof(*entries), 0) : NULL;
                if (!grown) {
                    success = false;
                    break;
                }
                entries = grown;
                capacity = next;
            }
            entries[count++] = data;
        } while (FindNextFileW(search, &data));
        if (success && GetLastError() != ERROR_NO_MORE_FILES) {
            success = false;
        }
        FindClose(search);
    }
    if (success) {
        if (count > 1) {
            qsort(entries, count, sizeof(*entries), _Sidebar_CompareFindData);
        }
        _Sidebar_ClearChildren(parent);
        size_t directories = 0;
        while (directories < count && (entries[directories].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { ++directories; }
        if (parent == s_folders) { s_rootDirectories = (UINT)directories; s_rootFiles = (UINT)(count - directories); }
        for (size_t i = 0; i < count; ++i) {
            bool const directory = (entries[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            HTREEITEM const inserted = _Sidebar_Insert(parent, entries[i].cFileName, directory);
            if (!inserted) {
                success = false;
                break;
            }
            if (directory && directories <= SIDEBAR_COUNT_FOLDERS) {
                HPATHL path = _Sidebar_ItemPath(inserted);
                int const children = _Sidebar_CountChildren(path);
                Path_Release(path);
                TVITEMW metadata = { 0 };
                metadata.mask = TVIF_PARAM;
                metadata.hItem = inserted;
                metadata.lParam = SIDEBAR_UNLOADED | ((LPARAM)(children + 1) << SIDEBAR_COUNT_SHIFT);
                TreeView_SetItem(s_tree, &metadata);
            } else if (!directory && Path_IsNotEmpty(Paths.CurrentFile)) {
                // Re-expanding a folder restores its current-file marker without
                // doing path reconstruction for every row on every paint.
                HPATHL path = _Sidebar_ItemPath(inserted);
                if (Path_StrgComparePath(path, Paths.CurrentFile, Paths.WorkingDirectory, true) == 0) { s_currentItem = inserted; }
                Path_Release(path);
            }
        }
        TVITEMW state = { 0 };
        state.mask = TVIF_PARAM;
        state.hItem = parent;
        TreeView_GetItem(s_tree, &state);
        state.mask |= TVIF_CHILDREN;
        state.lParam = (state.lParam & ~SIDEBAR_KIND_MASK) | (success ? SIDEBAR_LOADED : SIDEBAR_UNLOADED);
        state.cChildren = 1;
        TreeView_SetItem(s_tree, &state);
        if (success && !count) {
            WCHAR empty[128];
            GetLngString(IDS_MUI_SIDEBAR_EMPTY_ITEM, empty, COUNTOF(empty));
            HTREEITEM const item = _Sidebar_Insert(parent, empty, false);
            TVITEMW marker = { 0 };
            marker.mask = TVIF_PARAM;
            marker.hItem = item;
            marker.lParam = SIDEBAR_EMPTY;
            success = item && TreeView_SetItem(s_tree, &marker);
        }
    }
    FreeMem(entries);
    SendMessage(s_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_tree, NULL, TRUE);
    SetCursor(cursor);
    return success;
}

// Return the borrowed suffix of target below root, or NULL outside that directory.
// Inputs are non-null canonical absolute paths. Ignore DOS/UNC extended prefixes
// without changing the owned paths needed by file APIs; retain directory boundaries.
static LPCWSTR _Sidebar_RelativePath(LPCWSTR root, LPCWSTR target)
{
    LPCWSTR paths[] = { root, target };
    bool unc[] = { false, false };
    for (int i = 0; i < 2; ++i) {
        if (_wcsnicmp(paths[i], L"\\\\?\\UNC\\", 8) == 0) {
            paths[i] += 8;
            unc[i] = true;
        } else if (wcsncmp(paths[i], L"\\\\?\\", 4) == 0) {
            paths[i] += 4;
        } else if (wcsncmp(paths[i], L"\\\\", 2) == 0) {
            paths[i] += 2;
            unc[i] = true;
        }
    }
    root = paths[0];
    target = paths[1];
    size_t const length = wcslen(root);
    size_t const targetLength = wcslen(target);
    if (unc[0] != unc[1] || !length || targetLength < length ||
        CompareStringOrdinal(root, (int)length, target, (int)length, TRUE) != CSTR_EQUAL ||
        (targetLength > length && root[length - 1] != L'\\' && target[length] != L'\\')) {
        return NULL;
    }
    LPCWSTR part = target + length;
    while (*part == L'\\') { ++part; }
    return part;
}

// Abbreviate a canonical absolute folder (not a filename) relative to root, which
// may be empty. Inputs are non-null and may use DOS/UNC extended prefixes; output
// cannot alias them. cchOutput includes NUL. Keep the drive or UNC share and final
// folder outside root, adding an ellipsis only when components are omitted.
// Return false and clear writable output on invalid paths or insufficient storage.
static inline bool _Sidebar_AbbreviateFolder(LPCWSTR root, LPCWSTR folder, LPWSTR output, size_t cchOutput)
{
    if (!output || !cchOutput) {
        return false;
    }
    output[0] = L'\0';
    LPCWSTR const relative = _Sidebar_RelativePath(root, folder);
    HRESULT result;
    if (relative) {
        size_t const length = wcslen(relative);
        result = length ? StringCchPrintf(output, cchOutput, L"%s%s", relative, relative[length - 1] == L'\\' ? L"" : L"\\")
                        : StringCchCopy(output, cchOutput, L".\\");
    } else {
        LPCWSTR path = folder;
        bool unc = false;
        if (_wcsnicmp(path, L"\\\\?\\UNC\\", 8) == 0) {
            path += 8;
            unc = true;
        } else if (wcsncmp(path, L"\\\\?\\", 4) == 0) {
            path += 4;
        } else if (wcsncmp(path, L"\\\\", 2) == 0) {
            path += 2;
            unc = true;
        }
        size_t length = wcslen(path);
        while (length && path[length - 1] == L'\\') {
            --length;
        }
        size_t anchorLength;
        if (unc) {
            LPCWSTR const serverEnd = wcschr(path, L'\\');
            if (!serverEnd || serverEnd == path || !serverEnd[1]) {
                return false;
            }
            LPCWSTR const shareEnd = wcschr(serverEnd + 1, L'\\');
            anchorLength = shareEnd ? (size_t)(shareEnd - path) : length;
            if (anchorLength <= (size_t)(serverEnd - path) + 1) {
                return false;
            }
        } else {
            if (length < 2 || path[1] != L':' || path[2] != L'\\') {
                return false;
            }
            anchorLength = 2;
        }
        size_t leaf = length;
        while (leaf && path[leaf - 1] != L'\\') {
            --leaf;
        }
        if (length <= anchorLength || leaf <= anchorLength + 1) {
            result = StringCchPrintf(output, cchOutput, L"%s%.*s\\", unc ? L"\\\\" : L"", (int)length, path);
        } else {
            result = StringCchPrintf(output, cchOutput, L"%s%.*s\\\x2026\\%.*s\\", unc ? L"\\\\" : L"", (int)anchorLength, path, (int)(length - leaf), path + leaf);
        }
    }
    if (FAILED(result)) {
        output[0] = L'\0';
        return false;
    }
    return true;
}

// Resolve a path below the current root, expanding ancestors only as needed.
// When refreshMissing is true, retry a missing child once in its loaded parent.
static HTREEITEM _Sidebar_FindPath(const HPATHL path, bool refreshMissing)
{
    HTREEITEM parent = s_folders;
    LPCWSTR part = _Sidebar_RelativePath(Path_Get(Settings.SidebarFolder), Path_Get(path));
    if (!parent || !part) {
        return NULL;
    }
    while (*part) {
        LPCWSTR end = wcschr(part, L'\\');
        size_t const size = end ? (size_t)(end - part) : wcslen(part);
        if (size >= MAX_PATH_EXPLICIT) {
            return NULL;
        }
        WCHAR component[MAX_PATH_EXPLICIT];
        StringCchCopyN(component, COUNTOF(component), part, size);
        WCHAR name[MAX_PATH_EXPLICIT];
        LPARAM kind;
        if (!_Sidebar_GetItem(parent, name, COUNTOF(name), &kind) ||
            (kind != SIDEBAR_UNLOADED && kind != SIDEBAR_LOADED)) {
            return NULL;
        }
        if (kind == SIDEBAR_UNLOADED && !_Sidebar_LoadChildren(parent)) {
            return NULL;
        }
        TreeView_Expand(s_tree, parent, TVE_EXPAND);
        HTREEITEM match = NULL;
        for (int attempt = 0; attempt < (refreshMissing ? 2 : 1); ++attempt) {
            for (HTREEITEM child = TreeView_GetChild(s_tree, parent); child; child = TreeView_GetNextSibling(s_tree, child)) {
                // A localized hint can have the same text as a newly created
                // file. Ignore non-path rows so refreshMissing re-enumerates it.
                if (_Sidebar_GetItem(child, name, COUNTOF(name), &kind) &&
                    (kind == SIDEBAR_FILE || kind == SIDEBAR_UNLOADED || kind == SIDEBAR_LOADED) &&
                    CompareStringOrdinal(component, -1, name, -1, TRUE) == CSTR_EQUAL) {
                    match = child;
                    break;
                }
            }
            if (match || !refreshMissing || attempt || !_Sidebar_LoadChildren(parent)) {
                break;
            }
        }
        if (!match) {
            return NULL;
        }
        parent = match;
        part += size;
        while (*part == L'\\') { ++part; }
    }
    return parent;
}

// Match whole canonical paths in the pipe-separated sidebar-only hidden list.
// file is a nonempty absolute path; no MRU or setting is changed by this query.
// Removing the last hide can leave the owned string without an allocated buffer.
static bool _Sidebar_IsRecentHidden(LPCWSTR file)
{
    size_t const length = wcslen(file);
    for (LPCWSTR part = StrgGet(Settings.SidebarRecentHidden); part && *part;) {
        LPCWSTR const end = wcschr(part, L'|');
        size_t const count = end ? (size_t)(end - part) : wcslen(part);
        if (count == length && CompareStringOrdinal(part, (int)count, file, (int)length, TRUE) == CSTR_EQUAL) { return true; }
        if (!end) { break; }
        part = end + 1;
    }
    return false;
}

// Hide or unhide one canonical nonempty path. New hides go first; keep at most
// 32 whole entries, dropping the oldest when full. Normal settings save persists
// the owned string. This deliberately leaves the menu's MRU untouched.
static void _Sidebar_ChangeRecentHidden(LPCWSTR file, bool hide)
{
    HSTRINGW previous = StrgCopy(Settings.SidebarRecentHidden);
    HSTRINGW replacement = StrgCreate(hide ? file : NULL);
    unsigned count = hide ? 1 : 0;
    LPWSTR part = StrgWriteAccessBuf(previous, 0);
    while (*part && count < SIDEBAR_MAX_RECENT_HIDDEN) {
        LPWSTR const end = wcschr(part, L'|');
        if (end) { *end = L'\0'; }
        if (*part && CompareStringOrdinal(part, -1, file, -1, TRUE) != CSTR_EQUAL) {
            if (count++) { StrgCat(replacement, L"|"); }
            StrgCat(replacement, part);
        }
        if (!end) { break; }
        part = end + 1;
    }
    StrgSwap(Settings.SidebarRecentHidden, replacement);
    StrgDestroy(replacement);
    StrgDestroy(previous);
}

// Rebuild just the recent children from real MRU order. Paths use the same
// sanitize/unquote/app-relative expansion as opening recent files. Deleted files
// and sidebar hides do not consume the four visible slots. Preserve group state.
static void _Sidebar_RefreshRecent(void)
{
    if (!s_tree || (!s_recent && !Settings.ShowSidebar)) { return; }
    if (!s_recent) {
        WCHAR title[128];
        GetLngString(IDS_MUI_SIDEBAR_RECENT_TITLE, title, COUNTOF(title));
        s_recent = _Sidebar_Insert(TVI_ROOT, title, false);
        TVITEMW value = { 0 };
        value.mask = TVIF_PARAM;
        value.hItem = s_recent;
        value.lParam = SIDEBAR_RECENT;
        TreeView_SetItem(s_tree, &value);
    }
    bool const selected = TreeView_GetParent(s_tree, TreeView_GetSelection(s_tree)) == s_recent;
    HPATHL selection = selected ? _Sidebar_ItemPath(TreeView_GetSelection(s_tree)) : NULL;
    _Sidebar_ClearChildren(s_recent);
    s_recentCount = 0;
    for (int i = 0, count = MRU_Count(Globals.pFileMRU); i < count && s_recentCount < 4; ++i) {
        HPATHL path = Path_Allocate(NULL);
        LPWSTR const buffer = Path_WriteAccessBuf(path, PATHLONG_MAX_CCH);
        MRU_Enum(Globals.pFileMRU, i, buffer, (int)Path_GetBufCount(path));
        Path_Sanitize(path);
        Path_UnQuoteSpaces(path);
        Path_AbsoluteFromApp(path, true);
        if (Path_IsExistingFile(path)) {
            Path_GetLongPathNameEx(path);
            if (!_Sidebar_IsRecentHidden(Path_Get(path))) {
                HTREEITEM const item = _Sidebar_Insert(s_recent, Path_Get(path), false);
                TVITEMW value = { 0 };
                value.mask = TVIF_PARAM;
                value.hItem = item;
                value.lParam = SIDEBAR_RECENT_ITEM | (++s_recentCount << SIDEBAR_COUNT_SHIFT);
                TreeView_SetItem(s_tree, &value);
                if (Path_StrgComparePath(path, Paths.CurrentFile, Paths.WorkingDirectory, true) == 0) { s_recentCurrentItem = item; }
                if (selection && Path_StrgComparePath(path, selection, Paths.WorkingDirectory, true) == 0) { TreeView_SelectItem(s_tree, item); }
            }
        }
        Path_Release(path);
    }
    if (!s_recentCount) {
        WCHAR empty[256];
        GetLngString(IDS_MUI_SIDEBAR_RECENT_EMPTY, empty, COUNTOF(empty));
        TVITEMW value = { 0 };
        value.mask = TVIF_PARAM;
        value.hItem = _Sidebar_Insert(s_recent, empty, false);
        value.lParam = SIDEBAR_RECENT_EMPTY;
        TreeView_SetItem(s_tree, &value);
    }
    TVITEMW group = { 0 };
    group.mask = TVIF_CHILDREN;
    group.hItem = s_recent;
    group.cChildren = 1; // Even the empty-state row is a real child of this group.
    TreeView_SetItem(s_tree, &group);
    TreeView_Expand(s_tree, s_recent, Settings.SidebarRecentCollapsed ? TVE_COLLAPSE : TVE_EXPAND);
    Path_Release(selection);
    InvalidateRect(s_tree, NULL, FALSE);
}

// Add only the displayed files to the sidebar hide list, then show older eligible
// history (or the empty row). Never delete or reorder the shared recent-file MRU.
static void _Sidebar_ClearRecent(void)
{
    for (HTREEITEM item = TreeView_GetChild(s_tree, s_recent); item; item = TreeView_GetNextSibling(s_tree, item)) {
        HPATHL path = _Sidebar_ItemPath(item);
        if (Path_IsNotEmpty(path)) { _Sidebar_ChangeRecentHidden(Path_Get(path), true); }
        Path_Release(path);
    }
    _Sidebar_RefreshRecent();
}

void Sidebar_SetRoot(const HPATHL folder)
{
    if (!s_tree) {
        return;
    }
    HPATHL normalized = Path_Copy(folder);
    TreeView_DeleteAllItems(s_tree);
    s_recent = s_folders = NULL;
    s_rootDirectories = s_rootFiles = 0;
    _Sidebar_RefreshRecent();
    if (Path_IsExistingDirectory(normalized)) {
        Path_CanonicalizeEx(normalized, Paths.WorkingDirectory);
        // FileLoad expands 8.3 names too; both roots and item paths must use
        // that spelling before current-file containment and selection matching.
        Path_GetLongPathNameEx(normalized);
        Path_Reset(Settings.SidebarFolder, Path_Get(normalized));
        WCHAR title[128];
        GetLngString(IDS_MUI_SIDEBAR_FOLDERS_TITLE, title, COUNTOF(title));
        s_folders = _Sidebar_Insert(TVI_ROOT, title, true);
        s_restoreGroupCollapse = Settings.SidebarFoldersCollapsed;
        if (s_folders && _Sidebar_LoadChildren(s_folders)) {
            if (s_restoreGroupCollapse) { _Sidebar_CollapseDirectory(s_folders); }
            else { TreeView_Expand(s_tree, s_folders, TVE_EXPAND); }
        }
    }
    Path_Release(normalized);
    _Sidebar_UpdateChrome();
}

void Sidebar_SyncToFile(const HPATHL file, bool fileOpened)
{
    if (s_panel) { InvalidateRect(s_panel, NULL, FALSE); }
    if (fileOpened && Path_IsNotEmpty(file) && _Sidebar_IsRecentHidden(Path_Get(file))) { _Sidebar_ChangeRecentHidden(Path_Get(file), false); }
    _Sidebar_RefreshRecent();
    if (!s_tree || !Settings.ShowSidebar || !s_folders) {
        return;
    }
    // Startup's first sync must not undo the persisted group collapse. A later
    // file activation still expands its ancestors through the normal path.
    if (s_restoreGroupCollapse) { s_restoreGroupCollapse = false; return; }
    HTREEITEM const item = Path_IsNotEmpty(file) ? _Sidebar_FindPath(file, true) : NULL;
    s_currentItem = item;
    TreeView_SelectItem(s_tree, item);
    if (item) {
        TreeView_EnsureVisible(s_tree, item);
    }
    InvalidateRect(s_tree, NULL, FALSE);
}

// Temporary refresh state owns its paths only until the tree has been rebuilt.
typedef struct SidebarExpanded {
    HPATHL path;
    struct SidebarExpanded* next;
} SidebarExpanded;

void Sidebar_Refresh(void)
{
    if (!s_tree || !s_folders) {
        return;
    }
    bool const recentSelected = TreeView_GetSelection(s_tree) == s_recent;
    HPATHL selected = _Sidebar_ItemPath(TreeView_GetSelection(s_tree));
    SidebarExpanded* expanded = NULL;
    HTREEITEM const root = s_folders;
    bool const rootExpanded = (TreeView_GetItemState(s_tree, root, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    HTREEITEM item = root;
    while (item) {
        if (TreeView_GetItemState(s_tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) {
            SidebarExpanded* saved = AllocMem(sizeof(*saved), HEAP_ZERO_MEMORY);
            if (!saved) {
                // Leave the existing tree intact when its state cannot be retained.
                while (expanded) {
                    SidebarExpanded* next = expanded->next;
                    Path_Release(expanded->path);
                    FreeMem(expanded);
                    expanded = next;
                }
                Path_Release(selected);
                return;
            }
            saved->path = _Sidebar_ItemPath(item);
            saved->next = expanded;
            expanded = saved;
        }
        HTREEITEM next = TreeView_GetChild(s_tree, item);
        while (!next && item) {
            next = TreeView_GetNextSibling(s_tree, item);
            item = TreeView_GetParent(s_tree, item);
        }
        item = next;
    }
    Sidebar_SetRoot(Settings.SidebarFolder);
    while (expanded) {
        SidebarExpanded* next = expanded->next;
        HTREEITEM const found = _Sidebar_FindPath(expanded->path, false);
        if (found) { TreeView_Expand(s_tree, found, TVE_EXPAND); }
        Path_Release(expanded->path);
        FreeMem(expanded);
        expanded = next;
    }
    s_restoreGroupCollapse = false;
    // Child enumeration already restores the current-file handle where visible;
    // looking up that file here would reopen an intentionally collapsed parent.
    TreeView_SelectItem(s_tree, recentSelected ? s_recent : (Path_IsNotEmpty(selected) ? _Sidebar_FindPath(selected, false) : NULL));
    if (!rootExpanded) {
        _Sidebar_CollapseDirectory(s_folders);
    }
    Path_Release(selected);
}

// Binary targets keep the selected row and report a hint without saving/loading.
// For text, save before replacement and restore selection to the real document.
// False means binary/cancelled/failed/no load. Calls are guarded across dialogs.
static bool _Sidebar_OpenFile(const HPATHL target)
{
    if (_Sidebar_IsBinary(Path_FindFileName(target))) {
        WCHAR hint[MAX_PATH_EXPLICIT];
        GetLngString(IDS_MUI_SIDEBAR_BINARY, hint, COUNTOF(hint));
        _Sidebar_SetHint(hint);
        return false;
    }
    bool loaded = false;
    SET_FCT_GUARD(false)
    bool const same = Path_IsNotEmpty(Paths.CurrentFile) &&
                      Path_StrgComparePath(target, Paths.CurrentFile, Paths.WorkingDirectory, true) == 0;
    switch (_Sidebar_DecideSwitch(same, IsDocumentSaveNeeded(), Path_IsNotEmpty(Paths.CurrentFile))) {
    case SW_SAME_FILE:
        goto sync;
    case SW_SAVE_SILENT:
        if (!AutoSaveDoWork(FSF_None)) {
            goto sync;
        }
        break;
    case SW_ASK_UNTITLED:
        if (!FileSave(FSF_Ask)) {
            goto sync;
        }
        break;
    case SW_NO_SAVE:
        break;
    }
    {
        FileLoadFlags const flags = FLF_DontSave |
                                    (Settings.SkipUnicodeDetection ? FLF_SkipUnicodeDetect : 0) |
                                    (Settings.SkipANSICodePageDetection ? FLF_SkipANSICPDetection : 0);
        loaded = FileLoad(target, flags, 0, 0);
    }
sync:
    Sidebar_SyncToFile(Paths.CurrentFile, same);
    RESET_FCT_GUARD()
    return loaded;
}

// Return the visible row at a client point, including blank space to its right.
// Only visible rows are inspected; outside the tree's client rectangle yields NULL.
static HTREEITEM _Sidebar_RowAt(POINT point)
{
    RECT client;
    GetClientRect(s_tree, &client);
    if (!PtInRect(&client, point)) { return NULL; }
    for (HTREEITEM item = TreeView_GetFirstVisible(s_tree); item; item = TreeView_GetNextVisible(s_tree, item)) {
        RECT row;
        if (!TreeView_GetItemRect(s_tree, item, &row, FALSE)) { break; }
        if (row.top > point.y) { break; }
        if (row.bottom > point.y) { return item; }
    }
    return NULL;
}

// Keep the hover action after the group title at every width/DPI. Return empty
// bounds when it cannot fit, so an invisible action never intercepts title clicks.
// Drawing and activation share these exact bounds.
static RECT _Sidebar_CollapseRect(HDC dc, RECT row)
{
    WCHAR text[128], name[128], title[256];
    GetLngString(IDS_MUI_SIDEBAR_COLLAPSE_ALL, text, COUNTOF(text));
    GetLngString(IDS_MUI_SIDEBAR_FOLDERS_TITLE, name, COUNTOF(name));
    bool const expanded = (TreeView_GetItemState(s_tree, s_folders, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    StringCchPrintf(title, COUNTOF(title), L"%s  %s", name, expanded ? L"\x2212" : L"+");
    int const titleEnd = row.left + _Sidebar_Pixels(10) + SidebarDrawing_Measure(dc, s_drawing, title, SidebarFontSmall) + _Sidebar_Pixels(8);
    row.right -= _Sidebar_Pixels(10);
    row.left = max_i(titleEnd, row.right - SidebarDrawing_Measure(dc, s_drawing, text, SidebarFontSmall));
    if (row.left > row.right) { row.left = row.right; }
    return row;
}

// Match the clear action's drawn width at the right edge of the recent header.
// dc and row use tree-client pixels; callers release their borrowed DC.
static RECT _Sidebar_RecentClearRect(HDC dc, RECT row)
{
    WCHAR text[128];
    GetLngString(IDS_MUI_SIDEBAR_CLEAR_RECENT, text, COUNTOF(text));
    row.right -= _Sidebar_Pixels(10);
    row.left = max_i(_Sidebar_Pixels(10), row.right - SidebarDrawing_Measure(dc, s_drawing, text, SidebarFontSmall));
    return row;
}

// Collapse each top-level directory, retaining the group itself. Lazy reset
// discards all descendants at once, so this does not recurse down deep paths.
static void _Sidebar_CollapseAll(HTREEITEM parent)
{
    for (HTREEITEM child = TreeView_GetChild(s_tree, parent); child; child = TreeView_GetNextSibling(s_tree, child)) {
        WCHAR name[MAX_PATH_EXPLICIT];
        LPARAM kind;
        if (_Sidebar_GetItem(child, name, COUNTOF(name), &kind) && (kind == SIDEBAR_LOADED || kind == SIDEBAR_UNLOADED)) {
            _Sidebar_CollapseDirectory(child);
        }
    }
}

// Activate the hit row on mouse release or Enter. A group is never a filename;
// selection and current document stay separate for directory/group activation.
// A negative point.x denotes keyboard activation and skips the header action.
static void _Sidebar_ActivateRow(HTREEITEM item, POINT point)
{
    WCHAR name[MAX_PATH_EXPLICIT];
    LPARAM kind;
    if (!_Sidebar_GetItem(item, name, COUNTOF(name), &kind)) { return; }
    if (item == s_recent) {
        if (point.x >= 0 && s_recentCount) {
            RECT row, client;
            TreeView_GetItemRect(s_tree, item, &row, FALSE);
            GetClientRect(s_tree, &client);
            row.right = client.right;
            HDC dc = GetDC(s_tree);
            RECT const action = _Sidebar_RecentClearRect(dc, row);
            ReleaseDC(s_tree, dc);
            if (PtInRect(&action, point)) { _Sidebar_ClearRecent(); return; }
        }
        Settings.SidebarRecentCollapsed = !Settings.SidebarRecentCollapsed;
        TreeView_Expand(s_tree, item, Settings.SidebarRecentCollapsed ? TVE_COLLAPSE : TVE_EXPAND);
    } else if (item == s_folders && point.x >= 0) {
        RECT row;
        TreeView_GetItemRect(s_tree, item, &row, FALSE);
        RECT client;
        GetClientRect(s_tree, &client);
        row.right = client.right;
        HDC dc = GetDC(s_tree);
        RECT const action = _Sidebar_CollapseRect(dc, row);
        ReleaseDC(s_tree, dc);
        if (PtInRect(&action, point)) { _Sidebar_CollapseAll(s_folders); }
        else if (TreeView_GetItemState(s_tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) { _Sidebar_CollapseDirectory(item); }
        else if (kind != SIDEBAR_UNLOADED || _Sidebar_LoadChildren(item)) { TreeView_Expand(s_tree, item, TVE_EXPAND); }
    } else if (kind == SIDEBAR_FILE || kind == SIDEBAR_RECENT_ITEM) {
        HPATHL path = _Sidebar_ItemPath(item);
        _Sidebar_OpenFile(path);
        Path_Release(path);
    } else if (kind == SIDEBAR_LOADED || kind == SIDEBAR_UNLOADED) {
        if (TreeView_GetItemState(s_tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) { _Sidebar_CollapseDirectory(item); }
        else if (kind != SIDEBAR_UNLOADED || _Sidebar_LoadChildren(item)) { TreeView_Expand(s_tree, item, TVE_EXPAND); }
    }
    InvalidateRect(s_tree, NULL, FALSE);
}

// Use one extension palette for tree files and recent rows; directory and
// current-file styles are applied by the respective row renderer.
static COLORREF _Sidebar_FileColor(LPCWSTR name)
{
    if (_Sidebar_IsBinary(name)) { return s_palette->binary; }
    if (!_wcsicmp(PathFindExtensionW(name), L".ini")) { return s_palette->ini; }
    if (!_wcsicmp(PathFindExtensionW(name), L".h") || !_wcsicmp(PathFindExtensionW(name), L".c")) { return s_palette->source; }
    return s_palette->text;
}

// Draw a recent file as an aligned numbered row, without the tree's current-file
// capsule. name is its canonical path; text is the clipped tree-client bounds.
// The cached current handle avoids filesystem/path work for current-file checks.
static void _Sidebar_DrawRecentRow(HDC dc, HTREEITEM item, LPCWSTR name, RECT text)
{
    SidebarPalette const* p = s_palette;
    bool const current = item == s_recentCurrentItem;
    RECT marker = text;
    marker.right = marker.left + _Sidebar_Pixels(10);
    if (current) { SidebarDrawing_Text(dc, s_drawing, L"\x25B8", SidebarFontBody, marker, p->accent); }
    TVITEMW value = { 0 };
    value.mask = TVIF_PARAM;
    value.hItem = item;
    TreeView_GetItem(s_tree, &value);
    WCHAR number[16];
    StringCchPrintf(number, COUNTOF(number), L"%u", (unsigned)(value.lParam >> SIDEBAR_COUNT_SHIFT));
    RECT index = text;
    index.left = marker.right + _Sidebar_Pixels(10);
    index.right = index.left + SidebarDrawing_Measure(dc, s_drawing, L"4", SidebarFontBody);
    SidebarDrawing_Text(dc, s_drawing, number, SidebarFontBody, index, p->weak);
    text.left = index.right + _Sidebar_Pixels(10);
    RECT client;
    GetClientRect(s_panel, &client);
    if (_Sidebar_WidthTier(MulDiv(client.right, 96, s_dpi)) == 0) {
        HPATHL folder = Path_Allocate(name);
        Path_RemoveFileSpec(folder);
        WCHAR where[MAX_PATH_EXPLICIT];
        if (_Sidebar_AbbreviateFolder(Path_Get(Settings.SidebarFolder), Path_Get(folder), where, COUNTOF(where))) {
            RECT column = text;
            int const available = max_i(0, text.right - text.left);
            column.left = text.right - min_i(available / 2, SidebarDrawing_Measure(dc, s_drawing, where, SidebarFontSmall));
            SidebarDrawing_Text(dc, s_drawing, where, SidebarFontSmall, column, p->weak);
            text.right = max_i(text.left, column.left - _Sidebar_Pixels(10));
        }
        Path_Release(folder);
    }
    SidebarDrawing_Text(dc, s_drawing, PathFindFileNameW(name), current ? SidebarFontBold : SidebarFontBody,
                        text, current ? p->accent : _Sidebar_FileColor(name));
}

// Paint one complete row using cached metadata only. Names remain native item
// text for navigation/accessibility; prefixes, counts and states are presentation.
static void _Sidebar_DrawRow(NMTVCUSTOMDRAW* draw)
{
    HTREEITEM const item = (HTREEITEM)draw->nmcd.dwItemSpec;
    WCHAR name[MAX_PATH_EXPLICIT];
    LPARAM kind;
    if (!s_drawing || !_Sidebar_GetItem(item, name, COUNTOF(name), &kind)) { return; }
    RECT row, client;
    GetClientRect(s_tree, &client);
    if (!TreeView_GetItemRect(s_tree, item, &row, FALSE)) { return; }
    row.left = 0;
    row.right = client.right;
    HDC const dc = draw->nmcd.hdc;
    int const saved = SaveDC(dc);
    IntersectClipRect(dc, row.left, row.top, row.right, row.bottom);
    SidebarPalette const* p = s_palette;
    SetDCBrushColor(dc, p->island);
    FillRect(dc, &row, GetStockObject(DC_BRUSH));
    bool const current = item == s_currentItem;
    bool const group = item == s_recent || item == s_folders;
    bool const expanded = (TreeView_GetItemState(s_tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    COLORREF const background = current ? RGB(126,231,135) :
                                (item == TreeView_GetSelection(s_tree) && !group ? p->selected : (item == s_hotItem && !group ? p->rowHot : CLR_INVALID));
    if (background != CLR_INVALID) {
        SidebarDrawing_RoundRect(dc, row, _Sidebar_Pixels(7), Sidebar_GetStrokeWidth(s_dpi), background, CLR_INVALID);
    }
    RECT text = row;
    text.left += _Sidebar_Pixels(10);
    text.right -= _Sidebar_Pixels(10);
    if (group) {
        WCHAR title[256];
        bool const collapsed = item == s_recent ? Settings.SidebarRecentCollapsed : !expanded;
        StringCchPrintf(title, COUNTOF(title), L"%s  %s", name, collapsed ? L"+" : L"\x2212");
        SidebarDrawing_Text(dc, s_drawing, title, SidebarFontSmall, text, p->secondary);
        if (item == s_recent && item == s_hotItem && s_recentCount) {
            WCHAR clear[128];
            GetLngString(IDS_MUI_SIDEBAR_CLEAR_RECENT, clear, COUNTOF(clear));
            SidebarDrawing_Text(dc, s_drawing, clear, SidebarFontSmall, _Sidebar_RecentClearRect(dc, row), p->accent);
        } else if (item == s_folders) {
            WCHAR right[128];
            if (item == s_hotItem) { GetLngString(IDS_MUI_SIDEBAR_COLLAPSE_ALL, right, COUNTOF(right)); }
            else {
                WCHAR format[128];
                GetLngString(IDS_MUI_SIDEBAR_COUNTS, format, COUNTOF(format));
                StringCchPrintf(right, COUNTOF(right), format, s_rootDirectories, s_rootFiles);
            }
            if (item == s_hotItem) { text = _Sidebar_CollapseRect(dc, row); }
            else {
                int const titleWidth = SidebarDrawing_Measure(dc, s_drawing, title, SidebarFontSmall);
                text.left = max_i(text.left + titleWidth + _Sidebar_Pixels(8), text.right - SidebarDrawing_Measure(dc, s_drawing, right, SidebarFontSmall));
            }
            SidebarDrawing_Text(dc, s_drawing, right, SidebarFontSmall, text, item == s_hotItem ? p->accent : p->weak);
        }
    } else if (kind == SIDEBAR_RECENT_ITEM) {
        HPATHL path = _Sidebar_ItemPath(item);
        _Sidebar_DrawRecentRow(dc, item, Path_Get(path), text);
        Path_Release(path);
    } else if (kind == SIDEBAR_RECENT_EMPTY) {
        SidebarDrawing_Text(dc, s_drawing, name, SidebarFontBody, text, p->secondary);
    } else {
        // The 32768-character Windows path limit bounds depth as well. Allocate
        // only while drawing the row; no per-node path/prefix copies are retained.
        size_t depth = 0;
        HTREEITEM parent = TreeView_GetParent(s_tree, item);
        for (HTREEITEM node = parent; node && node != s_folders; node = TreeView_GetParent(s_tree, node)) { ++depth; }
        bool* ancestors = depth ? AllocMem(depth * sizeof(*ancestors), 0) : NULL;
        WCHAR* prefix = AllocMem((depth * 3 + 4) * sizeof(*prefix), 0);
        if (prefix && (!depth || ancestors)) {
            size_t index = depth;
            for (HTREEITEM node = parent; index; node = TreeView_GetParent(s_tree, node)) {
                ancestors[--index] = TreeView_GetNextSibling(s_tree, node) == NULL;
            }
            if (_Sidebar_BuildPrefix(ancestors, depth, TreeView_GetNextSibling(s_tree, item) == NULL, prefix, depth * 3 + 4)) {
                int const prefixWidth = SidebarDrawing_Measure(dc, s_drawing, prefix, SidebarFontBody);
                RECT prefixBounds = text;
                prefixBounds.right = prefixBounds.left + prefixWidth;
                // Preserve the connector geometry at deep levels: the row clip
                // trims overflow, while filenames alone receive an ellipsis.
                SidebarDrawing_Text(dc, s_drawing, prefix, SidebarFontBody, prefixBounds, current ? RGB(68,123,72) : p->prefix);
                text.left += prefixWidth;
            }
        }
        FreeMem(prefix);
        FreeMem(ancestors);
        COLORREF color = _Sidebar_FileColor(name);
        bool const directory = kind == SIDEBAR_UNLOADED || kind == SIDEBAR_LOADED;
        if (directory) { color = p->folder; }
        else if (kind == SIDEBAR_EMPTY) { color = p->weak; }
        if (current) {
            WCHAR editing[128];
            GetLngString(IDS_MUI_SIDEBAR_EDITING, editing, COUNTOF(editing));
            RECT state = text;
            state.left = max_i(text.left, text.right - SidebarDrawing_Measure(dc, s_drawing, editing, SidebarFontSmall));
            SidebarDrawing_Text(dc, s_drawing, editing, SidebarFontSmall, state, RGB(11,12,11));
            text.right = max_i(text.left, state.left - _Sidebar_Pixels(8));
            color = RGB(11,12,11);
        }
        WCHAR label[MAX_PATH_EXPLICIT + 2];
        StringCchPrintf(label, COUNTOF(label), L"%s%s", name, directory ? L"/" : L"");
        SidebarFont const font = directory || current ? SidebarFontBold : SidebarFontBody;
        SidebarDrawing_Text(dc, s_drawing, label, font, text, color);
        if (directory && !expanded) {
            TVITEMW data = { 0 };
            data.mask = TVIF_PARAM;
            data.hItem = item;
            TreeView_GetItem(s_tree, &data);
            int const count = (int)(data.lParam >> SIDEBAR_COUNT_SHIFT) - 1;
            if (count >= 0) {
                WCHAR suffix[128], format[128];
                GetLngString(count == 0 ? IDS_MUI_SIDEBAR_EMPTY_FOLDER : (count >= SIDEBAR_COUNT_LIMIT ? IDS_MUI_SIDEBAR_CHILD_COUNT_MORE : IDS_MUI_SIDEBAR_CHILD_COUNT), format, COUNTOF(format));
                StringCchPrintf(suffix, COUNTOF(suffix), format, min_i(count, SIDEBAR_COUNT_LIMIT - 1));
                text.left += SidebarDrawing_Measure(dc, s_drawing, label, font);
                SidebarDrawing_Text(dc, s_drawing, suffix, SidebarFontBody, text, p->weak);
            }
        }
    }
    RestoreDC(dc, saved);
}

// Handle drawing, expansion and deletion on the panel's UI thread. Expansion
// errors cancel the native operation; every deleted handle loses cached state.
static LRESULT _Sidebar_OnNotify(const NMHDR* notification)
{
    if (notification->hwndFrom != s_tree) {
        return 0;
    }
    if (notification->code == NM_CUSTOMDRAW) {
        NMTVCUSTOMDRAW* draw = (NMTVCUSTOMDRAW*)notification;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) { return CDRF_NOTIFYITEMDRAW; }
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            _Sidebar_DrawRow(draw);
            return CDRF_SKIPDEFAULT;
        }
    } else if (notification->code == TVN_DELETEITEMW) {
        HTREEITEM const item = ((NMTREEVIEWW const*)notification)->itemOld.hItem;
        if (s_currentItem == item) { s_currentItem = NULL; }
        if (s_recentCurrentItem == item) { s_recentCurrentItem = NULL; }
        if (s_hotItem == item) { s_hotItem = NULL; }
        if (s_pressedItem == item) { s_pressedItem = NULL; }
    } else if (notification->code == TVN_ITEMEXPANDINGW) {
        NMTREEVIEWW const* event = (NMTREEVIEWW const*)notification;
        if ((event->action & TVE_EXPAND) && (event->itemNew.lParam & SIDEBAR_KIND_MASK) == SIDEBAR_UNLOADED) {
            return _Sidebar_LoadChildren(event->itemNew.hItem) ? FALSE : TRUE;
        }
    } else if (notification->code == TVN_ITEMEXPANDEDW) {
        NMTREEVIEWW const* event = (NMTREEVIEWW const*)notification;
        if (event->itemNew.hItem == s_recent) {
            Settings.SidebarRecentCollapsed = !(event->action & TVE_EXPAND);
            return 0;
        }
        if (event->itemNew.hItem == s_folders) { Settings.SidebarFoldersCollapsed = !(event->action & TVE_EXPAND); }
        if (event->action == TVE_COLLAPSE) {
            _Sidebar_CollapseDirectory(event->itemNew.hItem);
        }
    }
    return 0;
}

// Scale fixed design dimensions from 96-DPI logical pixels.
static int _Sidebar_Pixels(int logical)
{
    return MulDiv(logical, s_dpi, 96);
}

// Place children wholly inside the island; minimum-height windows may hide the
// tree, but never pass negative dimensions to native controls.
static void _Sidebar_LayoutChildren(void)
{
    if (!s_panel) { return; }
    RECT client;
    GetClientRect(s_panel, &client);
    int const pad = _Sidebar_Pixels(12);
    int const diameter = _Sidebar_Pixels(30);
    s_refreshRect = (RECT){ client.right - pad - diameter, _Sidebar_Pixels(22), client.right - pad, _Sidebar_Pixels(52) };
    s_newRect = s_refreshRect;
    OffsetRect(&s_newRect, -diameter - _Sidebar_Pixels(6), 0);
    s_filterRect = (RECT){ pad, _Sidebar_Pixels(72), max_i(pad, client.right - pad), _Sidebar_Pixels(106) };
    int const statusTop = max_i(0, client.bottom - _Sidebar_Pixels(30));
    WCHAR sort[128];
    GetLngString(_Sidebar_SortLabelId(), sort, COUNTOF(sort));
    HDC const dc = GetDC(s_panel);
    int const sortWidth = max_i(_Sidebar_Pixels(38), SidebarDrawing_Measure(dc, s_drawing, sort, SidebarFontSmall) + _Sidebar_Pixels(8));
    ReleaseDC(s_panel, dc);
    s_sortRect = (RECT){ client.right - _Sidebar_Pixels(8) - sortWidth, statusTop + _Sidebar_Pixels(4), client.right - _Sidebar_Pixels(8), client.bottom - _Sidebar_Pixels(4) };
    // Hidden has a fixed width before the variable sort label. The same bounds
    // serve painting, hover, tooltips and clicks; adjacent edges never overlap.
    s_hiddenRect = (RECT){ s_sortRect.left - _Sidebar_Pixels(30), s_sortRect.top, s_sortRect.left, s_sortRect.bottom };
    if (s_filter) {
        int const editLeft = s_filterRect.left + _Sidebar_Pixels(32);
        int const editHeight = max_i(0, min_i(_Sidebar_Pixels(22), statusTop - _Sidebar_Pixels(78)));
        SetWindowPos(s_filter, NULL, editLeft, _Sidebar_Pixels(78),
                     max_i(0, s_filterRect.right - editLeft - _Sidebar_Pixels(12)), editHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (s_tree) {
        int const treeTop = _Sidebar_Pixels(116);
        SetWindowPos(s_tree, NULL, _Sidebar_Pixels(8), treeTop, max_i(0, client.right - _Sidebar_Pixels(16)),
                     max_i(0, statusTop - treeTop - _Sidebar_Pixels(8)), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (s_tooltip) {
        TOOLINFOW tool = { sizeof(tool) };
        tool.hwnd = s_panel;
        tool.uId = 1;
        tool.rect = (RECT){ _Sidebar_Pixels(12), _Sidebar_Pixels(12), max_i(0, s_newRect.left - _Sidebar_Pixels(6)), _Sidebar_Pixels(62) };
        SendMessage(s_tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&tool);
        tool.uId = 2;
        tool.rect = s_hiddenRect;
        SendMessage(s_tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&tool);
        tool.uId = 3;
        tool.rect = s_sortRect;
        SendMessage(s_tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&tool);
    }
    InvalidateRect(s_panel, NULL, FALSE);
}

// Handle a footer button at a panel-client point. Reuse refresh so surviving
// paths retain expansion/selection; existing settings persistence saves the mode.
// Return false outside both buttons, leaving other panel actions to their owner.
static bool _Sidebar_ToggleFooter(POINT point)
{
    if (PtInRect(&s_hiddenRect, point)) { Settings.SidebarShowHidden = !Settings.SidebarShowHidden; }
    else if (PtInRect(&s_sortRect, point)) { Settings.SidebarSortMode = (Settings.SidebarSortMode + 1) % 3; }
    else { return false; }
    Sidebar_Refresh();
    _Sidebar_LayoutChildren();
    return true;
}

// Root-dependent cue and availability use existing resources. T7 owns matching;
// the EDIT remains a native text editor until that logic is integrated.
static void _Sidebar_UpdateChrome(void)
{
    if (!s_panel || !s_filter) { return; }
    bool const hasRoot = Path_IsExistingDirectory(Settings.SidebarFolder);
    WCHAR hint[128];
    GetLngString(hasRoot ? IDS_MUI_SIDEBAR_FILTER_HINT : IDS_MUI_SIDEBAR_FILTER_NO_ROOT, hint, COUNTOF(hint));
    SendMessage(s_filter, EM_SETCUEBANNER, FALSE, (LPARAM)hint);
    EnableWindow(s_filter, hasRoot);
    InvalidateRect(s_panel, NULL, FALSE);
}

// Draw one complete panel into a memory DC. The status clip shares the island's
// lower corners; no child or rectangular footer can cover the antialiased edge.
static void _Sidebar_Paint(HDC dc)
{
    RECT client;
    GetClientRect(s_panel, &client);
    SetDCBrushColor(dc, _Sidebar_Background());
    FillRect(dc, &client, GetStockObject(DC_BRUSH));
    if (!s_drawing || IsRectEmpty(&client)) { return; }
    SidebarPalette const* p = s_palette;
    int const strokeWidth = Sidebar_GetStrokeWidth(s_dpi);
    SidebarDrawing_RoundRect(dc, client, _Sidebar_Pixels(16), strokeWidth, p->island, CLR_INVALID);
    int const statusTop = max_i(0, client.bottom - _Sidebar_Pixels(30));
    int const saved = SaveDC(dc);
    // Inset the footer clip by the scaled stroke on every edge.
    int const innerDiameter = max_i(1, _Sidebar_Pixels(32) - 2 * strokeWidth);
    HRGN const interior = CreateRoundRectRgn(strokeWidth, strokeWidth, client.right - strokeWidth,
                                           client.bottom - strokeWidth, innerDiameter, innerDiameter);
    if (interior) {
        ExtSelectClipRgn(dc, interior, RGN_AND);
        DeleteObject(interior);
    }
    RECT footer = { 0, statusTop, client.right, client.bottom };
    SetDCBrushColor(dc, p->status);
    FillRect(dc, &footer, GetStockObject(DC_BRUSH));
    RECT line = { 0, statusTop, client.right, statusTop + strokeWidth };
    SetDCBrushColor(dc, p->divider);
    FillRect(dc, &line, GetStockObject(DC_BRUSH));
    WCHAR label[128], sort[128], hidden[32];
    GetLngString(IDS_MUI_SIDEBAR_LABEL, label, COUNTOF(label));
    GetLngString(_Sidebar_SortLabelId(), sort, COUNTOF(sort));
    GetLngString(IDS_MUI_SIDEBAR_TOGGLE_HIDDEN, hidden, COUNTOF(hidden));
    int const badgeWidth = SidebarDrawing_Measure(dc, s_drawing, label, SidebarFontBold) + _Sidebar_Pixels(26);
    RECT badge = { 0, statusTop + strokeWidth, min_i(badgeWidth, client.right), client.bottom };
    SetDCBrushColor(dc, RGB(126,231,135));
    FillRect(dc, &badge, GetStockObject(DC_BRUSH));
    badge.left += _Sidebar_Pixels(16);
    SidebarDrawing_Text(dc, s_drawing, label, SidebarFontBold, badge, RGB(11,12,11));
    if (s_hover == 4) { SidebarDrawing_RoundRect(dc, s_hiddenRect, _Sidebar_Pixels(4), strokeWidth, p->textHot, CLR_INVALID); }
    if (s_hover == 5) { SidebarDrawing_RoundRect(dc, s_sortRect, _Sidebar_Pixels(4), strokeWidth, p->textHot, CLR_INVALID); }
    SidebarDrawing_Text(dc, s_drawing, hidden, SidebarFontSmall, s_hiddenRect, Settings.SidebarShowHidden ? p->accent : p->weak);
    SidebarDrawing_Text(dc, s_drawing, sort, SidebarFontSmall, s_sortRect, p->secondary);
    // Empty path handles can own a NULL buffer during startup, before FileLoad.
    LPCWSTR current = Path_IsNotEmpty(Paths.CurrentFile) ? Path_Get(Paths.CurrentFile) : L"";
    LPCWSTR const relative = Path_IsNotEmpty(Settings.SidebarFolder) ? _Sidebar_RelativePath(Path_Get(Settings.SidebarFolder), current) : NULL;
    if (relative) { current = relative; }
    RECT pathRect = { badgeWidth + _Sidebar_Pixels(8), statusTop, s_hiddenRect.left - _Sidebar_Pixels(8), client.bottom };
    SidebarDrawing_Text(dc, s_drawing, *s_hint ? s_hint : current, SidebarFontSmall, pathRect, *s_hint ? p->hint : p->secondary);
    RestoreDC(dc, saved);

    SidebarDrawing_RoundRect(dc, client, _Sidebar_Pixels(16), strokeWidth, CLR_INVALID, p->border);

    HPATHL parent = Path_Copy(Settings.SidebarFolder);
    LPCWSTR name = Path_FindFileName(parent);
    WCHAR empty[128];
    if (Path_IsEmpty(parent)) {
        GetLngString(IDS_MUI_SIDEBAR_NO_FOLDER, empty, COUNTOF(empty));
        name = empty;
    } else if (!*name) {
        name = Path_Get(parent);
    }
    RECT arrow = { _Sidebar_Pixels(16), _Sidebar_Pixels(16), _Sidebar_Pixels(34), _Sidebar_Pixels(38) };
    SidebarDrawing_Text(dc, s_drawing, L"\x276f", SidebarFontTitle, arrow, p->accent);
    RECT title = { _Sidebar_Pixels(35), _Sidebar_Pixels(16), s_newRect.left - _Sidebar_Pixels(6), _Sidebar_Pixels(38) };
    SidebarDrawing_Text(dc, s_drawing, name, SidebarFontTitle, title, p->title);
    Path_RemoveFileSpec(parent);
    LPCWSTR parentText = Path_Get(parent);
    if (Path_IsEmpty(Settings.SidebarFolder)) {
        GetLngString(IDS_MUI_SIDEBAR_CHOOSE_HINT, empty, COUNTOF(empty));
        parentText = empty;
    }
    RECT subTitle = { _Sidebar_Pixels(35), _Sidebar_Pixels(42), title.right, _Sidebar_Pixels(58) };
    SidebarDrawing_Text(dc, s_drawing, parentText, SidebarFontSmall, subTitle, p->secondary);
    Path_Release(parent);
    SidebarDrawing_RoundRect(dc, s_newRect, _Sidebar_Pixels(15), strokeWidth, s_hover == 1 ? p->buttonHot : p->button, CLR_INVALID);
    SidebarDrawing_RoundRect(dc, s_refreshRect, _Sidebar_Pixels(15), strokeWidth, s_hover == 2 ? p->buttonHot : p->button, CLR_INVALID);
    SidebarDrawing_Icon(dc, s_newRect, false, p->secondary, s_dpi);
    SidebarDrawing_Icon(dc, s_refreshRect, true, p->secondary, s_dpi);
    bool const focused = GetFocus() == s_filter;
    SidebarDrawing_RoundRect(dc, s_filterRect, _Sidebar_Pixels(17), strokeWidth, focused ? p->filterFocus : p->filter,
                            focused ? p->filterFocusBorder : (s_hover == 3 ? p->filterHotBorder : CLR_INVALID));
    RECT slash = s_filterRect;
    slash.left += _Sidebar_Pixels(14);
    slash.right = slash.left + _Sidebar_Pixels(12);
    SidebarDrawing_Text(dc, s_drawing, L"/", SidebarFontBold, slash, p->accent);
}

// Panel messages terminate tree notifications here, before the main window's
// editor notification guard. Resource release occurs after child destruction.
static LRESULT CALLBACK _Sidebar_PanelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    _Sidebar_ClearHintForInput(message);
    switch (message) {
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC const dc = BeginPaint(hwnd, &paint);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC const memory = CreateCompatibleDC(dc);
        HBITMAP const bitmap = CreateCompatibleBitmap(dc, max_i(1, client.right), max_i(1, client.bottom));
        if (memory && bitmap) {
            HGDIOBJ const previous = SelectObject(memory, bitmap);
            _Sidebar_Paint(memory);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, previous);
        } else {
            _Sidebar_Paint(dc);
        }
        if (bitmap) { DeleteObject(bitmap); }
        if (memory) { DeleteDC(memory); }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        _Sidebar_Paint((HDC)wParam);
        return 0;
    case WM_SIZE:
        _Sidebar_LayoutChildren();
        return 0;
    case WM_NOTIFY:
        if (((NMHDR*)lParam)->hwndFrom == s_tooltip && ((NMHDR*)lParam)->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* info = (NMTTDISPINFOW*)lParam;
            static WCHAR tip[256];
            if (info->hdr.idFrom == 2 || info->hdr.idFrom == 3) {
                GetLngString(info->hdr.idFrom == 2 ? IDS_MUI_SIDEBAR_HIDDEN_HINT : IDS_MUI_SIDEBAR_SORT_HINT, tip, COUNTOF(tip));
                info->lpszText = tip;
            } else {
                info->lpszText = (LPWSTR)(Path_IsNotEmpty(Settings.SidebarFolder) ? Path_Get(Settings.SidebarFolder) : L"");
            }
            return 0;
        }
        return _Sidebar_OnNotify((NMHDR*)lParam);
    case WM_COMMAND:
        if ((HWND)lParam == s_filter && HIWORD(wParam) == EN_CHANGE && *s_hint) { _Sidebar_SetHint(NULL); }
        if ((HWND)lParam == s_filter && (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)) {
            InvalidateRect(hwnd, &s_filterRect, FALSE);
            InvalidateRect(s_filter, NULL, TRUE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if ((HWND)lParam == s_filter) {
            SetTextColor((HDC)wParam, s_palette->text);
            SetBkColor((HDC)wParam, GetFocus() == s_filter ? s_palette->filterFocus : s_palette->filter);
            // DC_BRUSH avoids a focus-only brush allocation and keeps the EDIT opaque.
            SetDCBrushColor((HDC)wParam, GetFocus() == s_filter ? s_palette->filterFocus : s_palette->filter);
            return (LRESULT)GetStockObject(DC_BRUSH);
        }
        break;
    case WM_MOUSEMOVE: {
        POINT const point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int const hover = PtInRect(&s_newRect, point) ? 1 : PtInRect(&s_refreshRect, point) ? 2 :
                          PtInRect(&s_filterRect, point) ? 3 : PtInRect(&s_hiddenRect, point) ? 4 : PtInRect(&s_sortRect, point) ? 5 : 0;
        if (hover != s_hover) { s_hover = hover; InvalidateRect(hwnd, NULL, FALSE); }
        TRACKMOUSEEVENT tracking = { sizeof(tracking), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        s_hover = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT const point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (_Sidebar_ToggleFooter(point)) { return 0; }
        if (PtInRect(&s_filterRect, point) && IsWindowEnabled(s_filter)) { SetFocus(s_filter); }
        return 0;
    }
    case WM_NCDESTROY:
        s_panel = s_filter = s_tooltip = NULL;
        SidebarDrawing_Destroy(s_drawing);
        s_drawing = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

// The EDIT covers the middle of the capsule, so it must also track hover. Install
// this subclass once; theme and DPI updates only replace drawing resources.
static LRESULT CALLBACK _Sidebar_FilterProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(data);
    _Sidebar_ClearHintForInput(message);
    if (message == WM_MOUSEMOVE) {
        if (s_hover != 3) { s_hover = 3; InvalidateRect(s_panel, &s_filterRect, FALSE); }
        TRACKMOUSEEVENT tracking = { sizeof(tracking), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE) {
        s_hover = 0;
        InvalidateRect(s_panel, &s_filterRect, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, _Sidebar_FilterProc, 0);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

bool Sidebar_Create(HWND parent)
{
    WNDCLASSW panelClass = { 0 };
    panelClass.lpfnWndProc = _Sidebar_PanelProc;
    panelClass.hInstance = Globals.hInstance;
    panelClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    panelClass.lpszClassName = L"Notepad3Sidebar";
    if (!RegisterClassW(&panelClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { return false; }
    s_dpi = Scintilla_GetWindowDPI(parent);
    s_drawing = SidebarDrawing_Create(s_dpi);
    if (!s_drawing) { return false; }
    s_panel = CreateWindowEx(0, panelClass.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, parent, NULL, Globals.hInstance, NULL);
    if (!s_panel) {
        SidebarDrawing_Destroy(s_drawing);
        s_drawing = NULL;
        return false;
    }
    // Create the original tree first so existing SysTreeView321 lookups remain valid.
    s_tree = CreateWindowEx(0, WC_TREEVIEW, NULL,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TVS_FULLROWSELECT | TVS_TRACKSELECT |
                           TVS_NONEVENHEIGHT | TVS_NOHSCROLL | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                           0, 0, 0, 0, s_panel, (HMENU)IDC_SIDEBAR_TREE, Globals.hInstance, NULL);
    s_filter = CreateWindowEx(0, WC_EDIT, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             0, 0, 0, 0, s_panel, (HMENU)1, Globals.hInstance, NULL);
    s_tooltip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s_panel, NULL, Globals.hInstance, NULL);
    s_splitter = CreateWindowEx(0, WC_STATIC, NULL, WS_CHILD | WS_CLIPSIBLINGS,
                               0, 0, 0, 0, parent, (HMENU)IDC_SIDEBAR_SPLIT, Globals.hInstance, NULL);
    if (!s_tree || !s_filter || !s_tooltip || !s_splitter || !SetWindowSubclass(s_splitter, _Sidebar_SplitterProc, 0, 0) ||
        !SetWindowSubclass(s_tree, _Sidebar_TreeProc, 0, 0) || !SetWindowSubclass(s_filter, _Sidebar_FilterProc, 0, 0)) {
        DestroyWindow(s_panel);
        if (s_splitter) { DestroyWindow(s_splitter); }
        s_tree = s_splitter = NULL;
        return false;
    }
    TOOLINFOW tool = { sizeof(tool) };
    tool.uFlags = TTF_SUBCLASS;
    tool.hwnd = s_panel;
    tool.uId = 1;
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    for (UINT_PTR id = 1; id <= 3; ++id) {
        tool.uId = id;
        if (!SendMessage(s_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool)) {
            DestroyWindow(s_panel);
            DestroyWindow(s_splitter);
            return false;
        }
    }
    SendMessage(s_tooltip, TTM_SETMAXTIPWIDTH, 0, _Sidebar_Pixels(600));
    // InitTreeView owns subclass data until destruction; installing it again leaks it.
    InitWindowCommon(s_tree, true);
    InitTreeView(s_tree);
    TreeView_SetExtendedStyle(s_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    Sidebar_OnThemeChanged();
    // Root restoration is deferred until after startup's FileLoad.
    _Sidebar_UpdateChrome();
    ShowWindow(s_panel, Settings.ShowSidebar ? SW_SHOWNA : SW_HIDE);
    ShowWindow(s_splitter, Settings.ShowSidebar ? SW_SHOWNA : SW_HIDE);
    return true;
}

bool Sidebar_PreTranslateMessage(MSG* message)
{
    // Consumed Enter/F5 never reach the subclasses, so clear before dispatch too.
    if (message->hwnd == s_panel || IsChild(s_panel, message->hwnd)) {
        _Sidebar_ClearHintForInput(message->message);
    }
    if (message->hwnd == s_filter && message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST &&
        GetKeyState(VK_MENU) >= 0) {
        bool const control = GetKeyState(VK_CONTROL) < 0;
        if (!control || message->message == WM_CHAR || message->wParam == 'A' || message->wParam == 'C' || message->wParam == 'V' ||
            message->wParam == 'X' || message->wParam == 'Z') {
            if (!control && message->message == WM_KEYDOWN && (message->wParam == VK_ESCAPE || message->wParam == VK_TAB)) {
                SetFocus(Globals.hwndEdit);
                return true;
            }
            TranslateMessage(message);
            DispatchMessage(message);
            return true;
        }
    }
    if (!s_tree || message->hwnd != s_tree || message->message < WM_KEYFIRST ||
        message->message > WM_KEYLAST || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) {
        return false;
    }
    if (message->message == WM_KEYDOWN) {
        switch (message->wParam) {
        case VK_RETURN: {
            HTREEITEM const item = TreeView_GetSelection(s_tree);
            POINT const keyboard = { -1, -1 };
            if (item) { _Sidebar_ActivateRow(item, keyboard); }
            return true;
        }
        case VK_F5:
            Sidebar_Refresh();
            return true;
        case VK_ESCAPE:
        case VK_TAB:
            SetFocus(Globals.hwndEdit);
            return true;
        }
    }
    // Let TreeView implement arrows, incremental search and other native keys.
    // They must bypass the editor's unmodified accelerators as well.
    TranslateMessage(message);
    DispatchMessage(message);
    return true;
}

void Sidebar_Show(bool visible)
{
    if (!s_tree) {
        return;
    }
    if (!visible && (IsChild(s_panel, GetFocus()) || GetFocus() == s_panel || GetFocus() == s_splitter)) {
        SetFocus(Globals.hwndEdit);
    }
    if (visible && !TreeView_GetRoot(s_tree) && Path_IsExistingDirectory(Settings.SidebarFolder)) {
        Sidebar_SetRoot(Settings.SidebarFolder);
    }
    if (visible) {
        Sidebar_SyncToFile(Paths.CurrentFile, false);
    }
    ShowWindow(s_panel, visible ? SW_SHOWNA : SW_HIDE);
    ShowWindow(s_splitter, visible ? SW_SHOWNA : SW_HIDE);
}

void Sidebar_Layout(RECT* available)
{
    if (!s_tree || !Settings.ShowSidebar) {
        return;
    }
    UINT const dpi = Scintilla_GetWindowDPI(s_panel);
    int const split = MulDiv(8, dpi, 96);
    // Dark-mode outer margins have already been applied to the editor rectangle.
    int const left = available->left + (UseDarkMode() ? 0 : MulDiv(8, dpi, 96));
    int const top = available->top + (UseDarkMode() ? 0 : MulDiv(2, dpi, 96));
    int const bottom = available->bottom - (UseDarkMode() ? 0 : MulDiv(8, dpi, 96));
    int const width = _Sidebar_ClampWidth(Settings.SidebarWidth, available->right - left - split - (UseDarkMode() ? 2 * Sidebar_GetStrokeWidth(dpi) : 0), dpi);
    int const height = max_i(0, bottom - top);
    SetWindowPos(s_panel, NULL, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(s_splitter, NULL, left + width, top, split, height, SWP_NOZORDER | SWP_NOACTIVATE);
    available->left = left + width + split;
}

void Sidebar_OnThemeChanged(void)
{
    if (!s_tree) {
        return;
    }
    s_dpi = Scintilla_GetWindowDPI(s_panel);
    s_palette = UseDarkMode() ? &s_darkPalette : &s_lightPalette;
    SidebarDrawing* replacement = SidebarDrawing_Create(s_dpi);
    if (replacement) {
        SidebarDrawing* previous = s_drawing;
        s_drawing = replacement;
        SendMessage(s_filter, WM_SETFONT, (WPARAM)SidebarDrawing_EditFont(s_drawing), TRUE);
        SendMessage(s_tree, WM_SETFONT, (WPARAM)SidebarDrawing_EditFont(s_drawing), FALSE);
        SidebarDrawing_Destroy(previous);
    }
    // Match the style dialog: let the existing subclass update colors and redraw.
    SendMessage(s_tree, WM_THEMECHANGED, 0, 0);
    TreeView_SetBkColor(s_tree, s_palette->island);
    TreeView_SetTextColor(s_tree, s_palette->text);
    TreeView_SetItemHeight(s_tree, _Sidebar_Pixels(12));
    TreeView_SetIndent(s_tree, 0);
#ifdef D_NP3_WIN10_DARK_MODE
    AllowDarkModeForWindowEx(s_tree, UseDarkMode());
    AllowDarkModeForWindowEx(s_splitter, UseDarkMode());
#endif
    _Sidebar_UpdateChrome();
    _Sidebar_LayoutChildren();
}
