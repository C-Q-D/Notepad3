// Folder sidebar policies and native TreeView UI. File I/O remains owned by Notepad3.
#include "Helpers.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <stdlib.h>
#include "Sidebar.h"
#include "PathLib.h"
#include "Notepad3.h"
#include "Dialogs.h"
#include "DarkMode/DarkMode.h"

static HWND s_tree = NULL;
static HWND s_splitter = NULL;
static int _Sidebar_ClampWidth(int logicalWidth, int clientWidth, UINT dpi);

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
        // Static controls do not inherit the tree's dark background automatically.
        // Reuse the application's theme brush rather than introducing a palette.
        PAINTSTRUCT paint;
        HDC const dc = BeginPaint(hwnd, &paint);
        HBRUSH const brush = UseDarkMode() && Globals.hbrDarkModeBkgBrush ? Globals.hbrDarkModeBkgBrush : GetSysColorBrush(COLOR_BTNFACE);
        FillRect(dc, &paint.rcPaint, brush);
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
            int const pixels = _Sidebar_ClampWidth(MulDiv(point.x, 96, dpi), client.right - MulDiv(4, dpi, 96), dpi);
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

// Resolve a path below the current root, expanding ancestors only as needed.
// When refreshMissing is true, retry a missing child once in its loaded parent.
static HTREEITEM _Sidebar_FindPath(const HPATHL path, bool refreshMissing)
{
    HTREEITEM parent = TreeView_GetRoot(s_tree);
    LPCWSTR const root = Path_Get(Settings.SidebarFolder);
    LPCWSTR const target = Path_Get(path);
    size_t const length = wcslen(root);
    size_t const targetLength = wcslen(target);
    if (!parent || !length || targetLength < length ||
        CompareStringOrdinal(root, (int)length, target, (int)length, TRUE) != CSTR_EQUAL ||
        (targetLength > length && root[length - 1] != L'\\' && target[length] != L'\\')) {
        return NULL;
    }
    LPCWSTR part = target + length;
    while (*part == L'\\') { ++part; }
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
        Path_Reset(Settings.SidebarFolder, Path_Get(normalized));
        LPCWSTR const name = Path_FindFileName(normalized);
        HTREEITEM const root = _Sidebar_Insert(TVI_ROOT, *name ? name : Path_Get(normalized), true);
        if (root) {
            TreeView_Expand(s_tree, root, TVE_EXPAND);
        }
    }
    Path_Release(normalized);
}

void Sidebar_SyncToFile(const HPATHL file)
{
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

LRESULT Sidebar_OnNotify(const NMHDR* notification)
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

bool Sidebar_Create(HWND parent)
{
    s_tree = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, NULL,
                           WS_CHILD | WS_CLIPSIBLINGS | TVS_HASBUTTONS | TVS_HASLINES |
                           TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                           0, 0, 0, 0, parent, (HMENU)IDC_SIDEBAR_TREE, Globals.hInstance, NULL);
    s_splitter = CreateWindowEx(0, WC_STATIC, NULL, WS_CHILD | WS_CLIPSIBLINGS,
                               0, 0, 0, 0, parent, (HMENU)IDC_SIDEBAR_SPLIT, Globals.hInstance, NULL);
    if (!s_tree || !s_splitter || !SetWindowSubclass(s_splitter, _Sidebar_SplitterProc, 0, 0) ||
        !SetWindowSubclass(s_tree, _Sidebar_TreeProc, 0, 0)) {
        if (s_tree) { DestroyWindow(s_tree); }
        if (s_splitter) { DestroyWindow(s_splitter); }
        s_tree = s_splitter = NULL;
        return false;
    }
    TreeView_SetExtendedStyle(s_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    Sidebar_OnThemeChanged();
    // Root restoration is deferred until after startup's FileLoad.
    ShowWindow(s_tree, Settings.ShowSidebar ? SW_SHOWNA : SW_HIDE);
    ShowWindow(s_splitter, Settings.ShowSidebar ? SW_SHOWNA : SW_HIDE);
    return true;
}

bool Sidebar_PreTranslateMessage(MSG* message)
{
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
    if (!visible && (GetFocus() == s_tree || GetFocus() == s_splitter)) {
        SetFocus(Globals.hwndEdit);
    }
    if (visible && !TreeView_GetRoot(s_tree) && Path_IsExistingDirectory(Settings.SidebarFolder)) {
        Sidebar_SetRoot(Settings.SidebarFolder);
    }
    if (visible) {
        Sidebar_SyncToFile(Paths.CurrentFile);
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
    int const split = MulDiv(4, dpi, 96);
    // The editor reserve must exclude the splitter as well as the tree itself.
    int const width = _Sidebar_ClampWidth(Settings.SidebarWidth, available->right - available->left - split, dpi);
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
}
