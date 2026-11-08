// Folder sidebar UI. The main window owns its lifetime and calls it on the UI thread.
#pragma once
#include "TypeDefs.h"

// Persisted widths use 96-DPI logical pixels; layout may further limit available space.
enum { SIDEBAR_MIN_WIDTH = 100, SIDEBAR_MAX_WIDTH = 4000 };

// INI values are stable: alphabetical name, extension, then newest modification.
enum { SIDEBAR_SORT_NAME = 0, SIDEBAR_SORT_EXTENSION = 1, SIDEBAR_SORT_MODIFIED = 2 };
enum { SIDEBAR_MAX_RECENT_HIDDEN = 32 };

// Create child controls for parent using the configured visibility; root restoration
// follows startup FileLoad. Return false if either control or subclass fails.
bool Sidebar_Create(HWND parent);
// Show or hide both controls; hiding returns focus to the editor when needed.
void Sidebar_Show(bool visible);
// Place the sidebar in the available client rectangle and consume its left edge.
// The caller uses the remaining rectangle for the editor; width is in pixels.
void Sidebar_Layout(RECT* available);
// Reapply the native tree theme after a theme or DPI change. Safe before creation.
void Sidebar_OnThemeChanged(void);
// Replace the root with folder (non-null path handle). Missing folders yield an
// empty tree without a dialog. On success the selected folder setting stores the
// canonical long-name path, matching FileLoad's normalization of current files.
void Sidebar_SetRoot(const HPATHL folder);
// Select and reveal file within the root; clear selection outside it or when empty.
// A missing entry in an already loaded parent triggers a local re-enumeration.
void Sidebar_SyncToFile(const HPATHL file);
// Handle notifications from the sidebar tree only; return the TreeView result.
LRESULT Sidebar_OnNotify(const NMHDR* notification);
// Re-enumerate the root and restore expanded paths and selection where they exist.
void Sidebar_Refresh(void);
// Consume unmodified tree keyboard messages before main accelerators can turn
// Enter into an editor newline or F5 into Revert. Ctrl/Alt combinations pass on.
bool Sidebar_PreTranslateMessage(MSG* message);
