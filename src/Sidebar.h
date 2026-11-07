// Folder sidebar UI. The main window owns its lifetime and calls it on the UI thread.
#pragma once
#include "TypeDefs.h"

// Create hidden child controls for parent. Return false if either control fails.
bool Sidebar_Create(HWND parent);
// Show or hide both controls; hiding returns focus to the editor when needed.
void Sidebar_Show(bool visible);
// Place the sidebar in the available client rectangle and consume its left edge.
// The caller uses the remaining rectangle for the editor; width is in pixels.
void Sidebar_Layout(RECT* available);
// Reapply the native tree theme after a theme or DPI change. Safe before creation.
void Sidebar_OnThemeChanged(void);
// Replace the root with folder (non-null path handle). Missing folders yield an
// empty tree without a dialog. The selected folder setting is updated on success.
void Sidebar_SetRoot(const HPATHL folder);
// Select and reveal file within the root; clear selection outside it or when empty.
// A missing entry in an already loaded parent triggers a local re-enumeration.
void Sidebar_SyncToFile(const HPATHL file);
// Handle notifications from the sidebar tree only; return the TreeView result.
LRESULT Sidebar_OnNotify(const NMHDR* notification);
// Re-enumerate the root and restore expanded paths and selection where they exist.
void Sidebar_Refresh(void);
