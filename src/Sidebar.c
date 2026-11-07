// Folder sidebar policies and native TreeView UI. File I/O remains owned by Notepad3.
#include "Helpers.h"
#include <shlwapi.h>
#include "Sidebar.h"

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
