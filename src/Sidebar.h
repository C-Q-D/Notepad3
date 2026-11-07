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
