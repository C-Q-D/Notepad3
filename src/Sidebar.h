// Folder sidebar UI. The main window owns its lifetime and calls it on the UI thread.
#pragma once
#include "TypeDefs.h"

// Persisted widths use 96-DPI logical pixels; layout may further limit available space.
enum { SIDEBAR_MIN_WIDTH = 100, SIDEBAR_MAX_WIDTH = 4000 };

// INI values are stable: alphabetical name, extension, then newest modification.
enum { SIDEBAR_SORT_NAME = 0, SIDEBAR_SORT_EXTENSION = 1, SIDEBAR_SORT_MODIFIED = 2 };
enum { SIDEBAR_MAX_RECENT_HIDDEN = 32 };

// Scale the shared 1-logical-pixel panel/editor stroke to dpi, at least one pixel.
int Sidebar_GetStrokeWidth(UINT dpi);

// Create the panel, folder/result trees, EDIT and sibling drag gap for parent. Root
// restoration follows startup FileLoad. Return false on resource/control failure.
bool Sidebar_Create(HWND parent);
// Show or hide the panel and drag gap; hiding returns focus from any panel child.
void Sidebar_Show(bool visible);
// Stop pending search/navigation before FileSave can enter a closing modal loop.
// Retain the text; if closing is cancelled, search again without the old action.
void Sidebar_CancelPending(void);
// Place the sidebar in the available client rectangle and consume its left edge.
// The caller uses the remaining rectangle for the editor; width is in pixels.
void Sidebar_Layout(RECT* available);
// Rebuild panel fonts, sizes and palette, and reapply the native tree theme after
// theme/DPI changes. Safe before creation; never installs another tree subclass.
void Sidebar_OnThemeChanged(void);
// Replace the groups with folder (non-null path handle) as the directory group's
// path anchor. Unset/missing roots retain recent files and show recovery rows.
// Missing paths remain configured for Retry. On success
// persist the canonical long-name root and restore both group collapse settings.
// An active filter is cancelled and searched again against the replacement root.
void Sidebar_SetRoot(const HPATHL folder);
// Select and reveal file within the root; clear selection outside it or when empty.
// A missing entry in an already loaded parent triggers a local re-enumeration.
// The first sync after rebuilding honors the persisted directory-group collapse.
// Refresh recent files independently of the root and visibility. fileOpened is
// true only for a successful open/explicit same-file activation: remove that
// nonempty file from sidebar hides. Reloads (including automatic file watching),
// layout, saves and root changes pass false. While filtering, update the
// current-file paint mark only in already loaded rows; preserve the hidden tree's
// nodes, selection and expansion state.
void Sidebar_SyncToFile(const HPATHL file, bool fileOpened);
// Re-enumerate the root and restore expanded paths/selection; rerun active filters.
void Sidebar_Refresh(void);
// Consume tree navigation, including Shift combinations, before main accelerators turn
// Enter into an editor newline or F5 into Revert. Filter editing keys belong to
// EDIT; other Ctrl/Alt combinations pass on to the main-window accelerators.
bool Sidebar_PreTranslateMessage(MSG* message);
