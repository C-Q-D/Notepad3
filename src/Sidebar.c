// Folder sidebar panel, native tree and navigation policies. File I/O remains
// owned by Notepad3; custom tree rows and filtering are separate from this shell.
#include "Helpers.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <stdlib.h>
#include "Sidebar.h"
#include "SidebarDrawing.h"
#include "PathLib.h"
#include "Notepad3.h"
#include "MuiLanguage.h"
#include "Dialogs.h"
#include "DarkMode/DarkMode.h"

static HWND s_tree = NULL;
static HWND s_splitter = NULL;
static HWND s_panel = NULL;
static HWND s_filter = NULL;
static HWND s_tooltip = NULL;
static SidebarDrawing* s_drawing = NULL;
static UINT s_dpi = 96;
static int s_hover = 0;
static RECT s_filterRect, s_newRect, s_refreshRect, s_hiddenRect, s_sortRect;
static int _Sidebar_ClampWidth(int logicalWidth, int clientWidth, UINT dpi);
static void _Sidebar_UpdateChrome(void);

// Opaque colors include the specified alpha blended against each local surface.
typedef struct SidebarPalette {
    COLORREF island, border, text, title, secondary, accent, button, buttonHot;
    COLORREF filter, filterFocus, filterHotBorder, filterFocusBorder, status, divider, textHot;
} SidebarPalette;
static SidebarPalette const s_darkPalette = {
    RGB(18,20,18), RGB(32,34,32), RGB(201,209,201), RGB(238,243,238), RGB(139,148,158), RGB(126,231,135),
    RGB(27,31,27), RGB(37,43,37), RGB(26,30,26), RGB(21,25,21), RGB(38,48,38), RGB(63,92,65),
    RGB(15,17,15), RGB(27,32,27), RGB(28,32,28)
};
static SidebarPalette const s_lightPalette = {
    RGB(251,252,251), RGB(226,227,226), RGB(36,41,47), RGB(31,35,40), RGB(87,96,106), RGB(26,127,55),
    RGB(236,238,236), RGB(227,234,227), RGB(236,238,236), RGB(238,242,238), RGB(196,204,196), RGB(26,127,55),
    RGB(239,242,239), RGB(225,229,225), RGB(227,234,227)
};
static SidebarPalette const* s_palette = &s_lightPalette;

int Sidebar_GetStrokeWidth(UINT dpi)
{
    return max_i(1, MulDiv(1, dpi, 96));
}

// Main-window gaps must share the same background as the panel's outer corners.
static COLORREF _Sidebar_Background(void)
{
    return UseDarkMode() ? RGB(12,13,12) : GetSysColor(COLOR_WINDOW);
}

// Icon indices belong to the shared Shell image list; never destroy that list.
typedef struct SidebarIcon {
    WCHAR extension[MAX_PATH_EXPLICIT];
    int index;
} SidebarIcon;
static SidebarIcon s_icons[256];
static size_t s_iconCount = 0;
static int s_folderIcon = 0;
static POINT s_clickPoint;
static bool s_clickPointValid = false;
static bool s_syncAfterClick = false;

// Keep the client coordinates of the mouse message that produces NM_CLICK.
// Using its lParam also works when the window receives directed input messages.
static LRESULT CALLBACK _Sidebar_TreeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(data);
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        s_clickPoint.x = GET_X_LPARAM(lParam);
        s_clickPoint.y = GET_Y_LPARAM(lParam);
        s_clickPointValid = true;
        LRESULT const result = DefSubclassProc(hwnd, message, wParam, lParam);
        s_clickPointValid = false;
        if (s_syncAfterClick) {
            // Native click selection happens after NM_CLICK; restore only after it finishes.
            s_syncAfterClick = false;
            Sidebar_SyncToFile(Paths.CurrentFile);
        }
        return result;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, _Sidebar_TreeProc, 0);
        s_tree = NULL;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

// lParam encodes only node kind/loading state. Full paths are reconstructed from
// ancestors so large trees do not retain a second path string for every entry.
enum { SIDEBAR_FILE = 0, SIDEBAR_UNLOADED = 1, SIDEBAR_LOADED = 2 };

// Read an existing item's name and kind into MAX_PATH_EXPLICIT WCHARs of storage.
// False means that the item no longer exists; no returned buffer is borrowed.
static bool _Sidebar_GetItem(HTREEITEM item, LPWSTR name, LPARAM* kind)
{
    TVITEMW value = { 0 };
    value.mask = TVIF_TEXT | TVIF_PARAM;
    value.hItem = item;
    value.pszText = name;
    value.cchTextMax = MAX_PATH_EXPLICIT;
    bool const found = TreeView_GetItem(s_tree, &value) != FALSE;
    *kind = value.lParam;
    return found;
}

// Return a new owned path for item, or an empty handle for NULL. The caller releases it.
static HPATHL _Sidebar_ItemPath(HTREEITEM item)
{
    HPATHL result = Path_Allocate(NULL);
    if (!item) {
        return result;
    }
    HSTRINGW relative = StrgCreate(NULL);
    HTREEITEM const root = TreeView_GetRoot(s_tree);
    while (item && item != root) {
        WCHAR name[MAX_PATH_EXPLICIT];
        LPARAM kind;
        if (!_Sidebar_GetItem(item, name, &kind)) {
            StrgDestroy(relative);
            return result;
        }
        if (StrgGetLength(relative)) {
            StrgInsertCh(relative, 0, L'\\');
        }
        StrgInsert(relative, 0, name);
        item = TreeView_GetParent(s_tree, item);
    }
    Path_Reset(result, Path_Get(Settings.SidebarFolder));
    if (StrgGetLength(relative)) {
        Path_Append(result, StrgGet(relative));
    }
    StrgDestroy(relative);
    return result;
}

// Resolve a file-type icon without touching the file. Cache up to 256 extensions;
// uncommon extensions beyond that bound are still resolved using the Shell API.
static int _Sidebar_Icon(LPCWSTR name, bool directory)
{
    if (directory) {
        return s_folderIcon;
    }
    LPCWSTR const extension = PathFindExtensionW(name);
    for (size_t i = 0; i < s_iconCount; ++i) {
        if (CompareStringOrdinal(extension, -1, s_icons[i].extension, -1, TRUE) == CSTR_EQUAL) {
            return s_icons[i].index;
        }
    }
    WCHAR lookup[MAX_PATH_EXPLICIT + 8];
    StringCchPrintf(lookup, COUNTOF(lookup), L"file%s", extension);
    SHFILEINFOW info = { 0 };
    SHGetFileInfoW(lookup, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                  SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (s_iconCount < COUNTOF(s_icons)) {
        StringCchCopy(s_icons[s_iconCount].extension, COUNTOF(s_icons[s_iconCount].extension), extension);
        s_icons[s_iconCount++].index = info.iIcon;
    }
    return info.iIcon;
}

// Insert one enumerated name under parent; directory nodes get a lazy-load marker.
// The returned handle belongs to TreeView and is invalidated by collapse/reset.
static HTREEITEM _Sidebar_Insert(HTREEITEM parent, LPCWSTR name, bool directory)
{
    TVINSERTSTRUCTW entry = { 0 };
    entry.hParent = parent;
    entry.hInsertAfter = TVI_LAST;
    entry.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    entry.item.pszText = (LPWSTR)name;
    entry.item.lParam = directory ? SIDEBAR_UNLOADED : SIDEBAR_FILE;
    entry.item.iImage = entry.item.iSelectedImage = _Sidebar_Icon(name, directory);
    entry.item.cChildren = directory ? 1 : 0;
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
    TVITEMW state = { 0 };
    state.mask = TVIF_PARAM | TVIF_CHILDREN;
    state.hItem = item;
    state.lParam = SIDEBAR_UNLOADED;
    state.cChildren = 1;
    TreeView_SetItem(s_tree, &state);
    TreeView_Expand(s_tree, item, TVE_COLLAPSE | TVE_COLLAPSERESET);
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

// qsort adapter for WIN32_FIND_DATAW entries; names use the tested natural order.
static int __cdecl _Sidebar_CompareFindData(const void* left, const void* right)
{
    WIN32_FIND_DATAW const* a = left;
    WIN32_FIND_DATAW const* b = right;
    return _Sidebar_CompareEntries((a->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, a->cFileName,
                                   (b->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, b->cFileName);
}

// Enumerate one directory synchronously. On allocation or enumeration failure,
// retain the unloaded state so a later expansion can retry; never show a dialog.
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
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
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
        for (size_t i = 0; i < count; ++i) {
            if (!_Sidebar_Insert(parent, entries[i].cFileName, (entries[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
                success = false;
                break;
            }
        }
        TVITEMW state = { 0 };
        state.mask = TVIF_PARAM | TVIF_CHILDREN;
        state.hItem = parent;
        state.lParam = success ? SIDEBAR_LOADED : SIDEBAR_UNLOADED;
        state.cChildren = count ? 1 : 0;
        TreeView_SetItem(s_tree, &state);
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
    HTREEITEM parent = TreeView_GetRoot(s_tree);
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
        if (!_Sidebar_GetItem(parent, name, &kind) || kind == SIDEBAR_FILE) {
            return NULL;
        }
        if (kind == SIDEBAR_UNLOADED && !_Sidebar_LoadChildren(parent)) {
            return NULL;
        }
        TreeView_Expand(s_tree, parent, TVE_EXPAND);
        HTREEITEM match = NULL;
        for (int attempt = 0; attempt < (refreshMissing ? 2 : 1); ++attempt) {
            for (HTREEITEM child = TreeView_GetChild(s_tree, parent); child; child = TreeView_GetNextSibling(s_tree, child)) {
                if (_Sidebar_GetItem(child, name, &kind) && CompareStringOrdinal(component, -1, name, -1, TRUE) == CSTR_EQUAL) {
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

void Sidebar_SetRoot(const HPATHL folder)
{
    if (!s_tree) {
        return;
    }
    HPATHL normalized = Path_Copy(folder);
    TreeView_DeleteAllItems(s_tree);
    if (Path_IsExistingDirectory(normalized)) {
        Path_CanonicalizeEx(normalized, Paths.WorkingDirectory);
        // FileLoad expands 8.3 names too; both roots and item paths must use
        // that spelling before current-file containment and selection matching.
        Path_GetLongPathNameEx(normalized);
        Path_Reset(Settings.SidebarFolder, Path_Get(normalized));
        LPCWSTR const name = Path_FindFileName(normalized);
        HTREEITEM const root = _Sidebar_Insert(TVI_ROOT, *name ? name : Path_Get(normalized), true);
        if (root) {
            TreeView_Expand(s_tree, root, TVE_EXPAND);
        }
    }
    Path_Release(normalized);
    _Sidebar_UpdateChrome();
}

void Sidebar_SyncToFile(const HPATHL file)
{
    if (s_panel) { InvalidateRect(s_panel, NULL, FALSE); }
    if (!s_tree || !Settings.ShowSidebar || !TreeView_GetRoot(s_tree)) {
        return;
    }
    HTREEITEM const item = Path_IsNotEmpty(file) ? _Sidebar_FindPath(file, true) : NULL;
    TreeView_SelectItem(s_tree, item);
    if (item) {
        TreeView_EnsureVisible(s_tree, item);
    }
}

// Temporary refresh state owns its paths only until the tree has been rebuilt.
typedef struct SidebarExpanded {
    HPATHL path;
    struct SidebarExpanded* next;
} SidebarExpanded;

void Sidebar_Refresh(void)
{
    if (!s_tree || !TreeView_GetRoot(s_tree)) {
        return;
    }
    HPATHL selected = _Sidebar_ItemPath(TreeView_GetSelection(s_tree));
    SidebarExpanded* expanded = NULL;
    HTREEITEM const root = TreeView_GetRoot(s_tree);
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
    TreeView_SelectItem(s_tree, Path_IsNotEmpty(selected) ? _Sidebar_FindPath(selected, false) : NULL);
    if (!rootExpanded) {
        _Sidebar_CollapseDirectory(TreeView_GetRoot(s_tree));
    }
    Path_Release(selected);
}

// Save before replacing the document. False means cancelled/failed/no load; all
// paths restore selection to the real document. Calls are guarded across dialogs.
static bool _Sidebar_OpenFile(const HPATHL target)
{
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
    Sidebar_SyncToFile(Paths.CurrentFile);
    RESET_FCT_GUARD()
    return loaded;
}

// Handle tree expansion and file activation on the panel's UI thread. Return the
// TreeView notification result, including cancellation when enumeration fails.
static LRESULT _Sidebar_OnNotify(const NMHDR* notification)
{
    if (notification->hwndFrom != s_tree) {
        return 0;
    }
    if (notification->code == NM_CLICK) {
        TVHITTESTINFO hit = { 0 };
        if (s_clickPointValid) {
            hit.pt = s_clickPoint;
        } else {
            DWORD const position = GetMessagePos();
            hit.pt.x = GET_X_LPARAM(position);
            hit.pt.y = GET_Y_LPARAM(position);
            ScreenToClient(s_tree, &hit.pt);
        }
        HTREEITEM const item = TreeView_HitTest(s_tree, &hit);
        WCHAR name[MAX_PATH_EXPLICIT];
        LPARAM kind;
        if (item && (hit.flags & (TVHT_ONITEMLABEL | TVHT_ONITEMICON)) &&
            _Sidebar_GetItem(item, name, &kind) && kind == SIDEBAR_FILE) {
            HPATHL path = _Sidebar_ItemPath(item);
            _Sidebar_OpenFile(path);
            Path_Release(path);
            s_syncAfterClick = true;
        }
    } else if (notification->code == TVN_ITEMEXPANDINGW) {
        NMTREEVIEWW const* event = (NMTREEVIEWW const*)notification;
        if ((event->action & TVE_EXPAND) && event->itemNew.lParam == SIDEBAR_UNLOADED) {
            return _Sidebar_LoadChildren(event->itemNew.hItem) ? FALSE : TRUE;
        }
    } else if (notification->code == TVN_ITEMEXPANDEDW) {
        NMTREEVIEWW const* event = (NMTREEVIEWW const*)notification;
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
    s_sortRect = (RECT){ client.right - _Sidebar_Pixels(46), statusTop + _Sidebar_Pixels(4), client.right - _Sidebar_Pixels(8), client.bottom - _Sidebar_Pixels(4) };
    s_hiddenRect = s_sortRect;
    OffsetRect(&s_hiddenRect, -_Sidebar_Pixels(30), 0);
    s_hiddenRect.right -= _Sidebar_Pixels(8);
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
    }
    InvalidateRect(s_panel, NULL, FALSE);
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
    WCHAR label[128], sort[32];
    GetLngString(IDS_MUI_SIDEBAR_LABEL, label, COUNTOF(label));
    GetLngString(IDS_MUI_SIDEBAR_SORT_NAME, sort, COUNTOF(sort));
    int const badgeWidth = SidebarDrawing_Measure(dc, s_drawing, label, SidebarFontBold) + _Sidebar_Pixels(26);
    RECT badge = { 0, statusTop + strokeWidth, min_i(badgeWidth, client.right), client.bottom };
    SetDCBrushColor(dc, RGB(126,231,135));
    FillRect(dc, &badge, GetStockObject(DC_BRUSH));
    badge.left += _Sidebar_Pixels(16);
    SidebarDrawing_Text(dc, s_drawing, label, SidebarFontBold, badge, RGB(11,12,11));
    if (s_hover == 4) { SidebarDrawing_RoundRect(dc, s_hiddenRect, _Sidebar_Pixels(4), strokeWidth, p->textHot, CLR_INVALID); }
    if (s_hover == 5) { SidebarDrawing_RoundRect(dc, s_sortRect, _Sidebar_Pixels(4), strokeWidth, p->textHot, CLR_INVALID); }
    SidebarDrawing_Text(dc, s_drawing, L".*", SidebarFontSmall, s_hiddenRect, p->secondary);
    SidebarDrawing_Text(dc, s_drawing, sort, SidebarFontSmall, s_sortRect, p->secondary);
    // Empty path handles can own a NULL buffer during startup, before FileLoad.
    LPCWSTR current = Path_IsNotEmpty(Paths.CurrentFile) ? Path_Get(Paths.CurrentFile) : L"";
    LPCWSTR const relative = Path_IsNotEmpty(Settings.SidebarFolder) ? _Sidebar_RelativePath(Path_Get(Settings.SidebarFolder), current) : NULL;
    if (relative) { current = relative; }
    RECT pathRect = { badgeWidth + _Sidebar_Pixels(8), statusTop, s_hiddenRect.left - _Sidebar_Pixels(8), client.bottom };
    SidebarDrawing_Text(dc, s_drawing, current, SidebarFontSmall, pathRect, p->secondary);
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
            ((NMTTDISPINFOW*)lParam)->lpszText = (LPWSTR)(Path_IsNotEmpty(Settings.SidebarFolder) ? Path_Get(Settings.SidebarFolder) : L"");
            return 0;
        }
        return _Sidebar_OnNotify((NMHDR*)lParam);
    case WM_COMMAND:
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
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TVS_HASBUTTONS | TVS_HASLINES |
                           TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
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
    if (!SendMessage(s_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool)) {
        DestroyWindow(s_panel);
        DestroyWindow(s_splitter);
        return false;
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
            WCHAR name[MAX_PATH_EXPLICIT];
            LPARAM kind;
            if (item && _Sidebar_GetItem(item, name, &kind)) {
                if (kind == SIDEBAR_FILE) {
                    HPATHL path = _Sidebar_ItemPath(item);
                    _Sidebar_OpenFile(path);
                    Path_Release(path);
                } else if (TreeView_GetItemState(s_tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) {
                    _Sidebar_CollapseDirectory(item);
                } else if (kind != SIDEBAR_UNLOADED || _Sidebar_LoadChildren(item)) {
                    // Ensure loading even when TreeView omits repeat notifications.
                    TreeView_Expand(s_tree, item, TVE_EXPAND);
                }
            }
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
        Sidebar_SyncToFile(Paths.CurrentFile);
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
        SidebarDrawing_Destroy(previous);
    }
    // Match the style dialog: let the existing subclass update colors and redraw.
    SendMessage(s_tree, WM_THEMECHANGED, 0, 0);
    TreeView_SetBkColor(s_tree, s_palette->island);
    TreeView_SetTextColor(s_tree, s_palette->text);
    s_iconCount = 0;
    SHFILEINFOW info = { 0 };
    // A synthetic directory name yields the folder icon; a drive root yields a drive icon.
    HIMAGELIST const images = (HIMAGELIST)SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
                                                     SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    TreeView_SetImageList(s_tree, images, TVSIL_NORMAL);
    s_folderIcon = info.iIcon;
#ifdef D_NP3_WIN10_DARK_MODE
    AllowDarkModeForWindowEx(s_tree, UseDarkMode());
    AllowDarkModeForWindowEx(s_splitter, UseDarkMode());
#endif
    _Sidebar_UpdateChrome();
    _Sidebar_LayoutChildren();
}
