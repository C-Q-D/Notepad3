# Run only isolated Notepad3 instances and retain no user or fixture state.
# The AHK assertions use messages addressed to the test window, never global input.
param(
    [ValidateSet('A','B','C','D','E','F')][string]$Through = 'F',
    [ValidateSet('All','C','D','E','F','Groups','Toggles','LongPath','ShortPath','OpenFolder','Recent','Create','Filter','Edges','Rework','Rework2')][string]$Scenario = 'All',
    [ValidateSet('Light','Dark')][string]$Theme = 'Light',
    [string]$ScreenshotDirectory = '',
    [string]$Executable = (Join-Path $PSScriptRoot '..\Bin\Release_x64_v143\Notepad3.exe'),
    [string]$SidebarSource = (Join-Path $PSScriptRoot '..\src\Sidebar.c')
)
$ErrorActionPreference = 'Stop'

# Exercise the actual marker synchronization against a native tree. Its private
# cached handle is paint-only; extraction avoids adding a production test hook or
# asserting pixels. The loader is forbidden so a marker update cannot do I/O.
function Test-FilterCurrentMarker([string]$Directory, [string]$Source) {
    $source = Get-Content -LiteralPath $Source -Raw
    $parts = foreach ($name in @('_Sidebar_RelativePath', '_Sidebar_GetItem', '_Sidebar_FindPath', 'Sidebar_SyncToFile')) {
        $match = [regex]::Match($source, '(?ms)^(?:static )?[^\r\n]+ ' + $name + '\([^\r\n]*\)\r?\n\{.*?^}')
        if (!$match.Success) { throw "Missing production marker function $name" }
        $match.Value
    }
    $harness = @'
#include <windows.h>
#include <commctrl.h>
#include <strsafe.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>
#define MAX_PATH_EXPLICIT 32768
#define COUNTOF(value) _countof(value)
typedef LPCWSTR HPATHL;
#define Path_Get(path) (path)
#define Path_IsNotEmpty(path) ((path) && *(path))
enum { SIDEBAR_FILE = 0, SIDEBAR_UNLOADED = 1, SIDEBAR_LOADED = 2, SIDEBAR_KIND_MASK = 15 };
static struct { LPCWSTR SidebarFolder; bool ShowSidebar; } Settings = { L"C:\\fixture", true };
static HWND s_tree, s_panel, s_results;
static HTREEITEM s_folders, s_currentItem;
static WCHAR s_query[32] = L"txt";
static bool s_restoreGroupCollapse;
static int loads, failures;
// Loading a directory is an observable failure, even if the final handle matches.
static bool _Sidebar_LoadChildren(HTREEITEM item) { (void)item; ++loads; return false; }
// The recent-files group is outside this contract; Sidebar_SyncToFile refreshes it first.
static bool _Sidebar_IsRecentHidden(LPCWSTR path) { (void)path; return false; }
static void _Sidebar_ChangeRecentHidden(LPCWSTR path, bool hidden) { (void)path; (void)hidden; }
static void _Sidebar_RefreshRecent(void) {}
'@ + "`n" + ($parts -join "`n") + @'

// Create actual native rows; expected handles do not come from the path walker.
static HTREEITEM insert(HTREEITEM parent, LPWSTR name, LPARAM kind) {
    TVINSERTSTRUCTW value = { 0 };
    value.hParent = parent; value.hInsertAfter = TVI_LAST;
    value.item.mask = TVIF_TEXT | TVIF_PARAM;
    value.item.pszText = name; value.item.lParam = kind;
    return TreeView_InsertItem(s_tree, &value);
}
// Report independent marker contracts and return a failing process exit on error.
static void check(const char* name, bool pass) {
    printf("%s: Filter marker %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}
// Keep the expected handles and native tree state independent of the path walker.
int main(void) {
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_TREEVIEW_CLASSES };
    InitCommonControlsEx(&controls);
    s_panel = CreateWindowW(L"STATIC", L"", WS_OVERLAPPED, 0, 0, 500, 500, NULL, NULL, NULL, NULL);
    s_tree = CreateWindowW(WC_TREEVIEWW, L"", WS_CHILD, 0, 0, 400, 400, s_panel, NULL, NULL, NULL);
    s_results = CreateWindowW(WC_TREEVIEWW, L"", WS_CHILD, 0, 0, 400, 400, s_panel, NULL, NULL, NULL);
    if (!s_panel || !s_tree || !s_results) { fprintf(stderr, "Native marker fixture failed\n"); return 2; }
    s_folders = insert(TVI_ROOT, L"Folders", SIDEBAR_LOADED);
    HTREEITEM start = insert(s_folders, L"start.txt", SIDEBAR_FILE);
    HTREEITEM closed = insert(s_folders, L"closed", SIDEBAR_UNLOADED);
    HTREEITEM kept = insert(s_folders, L"kept", SIDEBAR_LOADED);
    HTREEITEM target = insert(kept, L"target.txt", SIDEBAR_FILE);
    TreeView_Expand(s_tree, s_folders, TVE_EXPAND);
    TreeView_Expand(s_tree, kept, TVE_EXPAND);
    TreeView_SelectItem(s_tree, start);
    UINT const keptState = TreeView_GetItemState(s_tree, kept, TVIS_EXPANDED);
    UINT const count = TreeView_GetCount(s_tree);
    s_currentItem = start;
    Sidebar_SyncToFile(L"C:\\fixture\\closed\\Inner.txt", false);
    check("unloaded result clears old Editing handle", s_currentItem == NULL);
    s_currentItem = start;
    Sidebar_SyncToFile(L"c:\\FIXTURE\\kept\\TARGET.txt", false);
    check("loaded result moves Editing handle", s_currentItem == target);
    Sidebar_SyncToFile(L"C:\\outside\\else.txt", false);
    check("outside result clears Editing handle", s_currentItem == NULL);
    check("sync preserves selection, expansion and loaded nodes", loads == 0 && TreeView_GetSelection(s_tree) == start &&
        TreeView_GetItemState(s_tree, kept, TVIS_EXPANDED) == keptState &&
        TreeView_GetItemState(s_tree, closed, TVIS_EXPANDED) == 0 && TreeView_GetCount(s_tree) == count);
    DestroyWindow(s_panel);
    return failures ? 1 : 0;
}
'@
    $file = Join-Path $Directory 'FilterMarker.c'
    Set-Content -LiteralPath $file -Value $harness -Encoding utf8
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $env:PATH = (Split-Path -Parent $vswhere) + [IO.Path]::PathSeparator + $env:PATH
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $exe = Join-Path $Directory 'FilterMarker.exe'
    & cl.exe /nologo /W4 /WX /std:c11 /utf-8 /DUNICODE /D_UNICODE $file "/Fo$Directory\" "/Fe$exe" /link user32.lib comctl32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Filter marker harness compilation failed' }
    & $exe
    return $LASTEXITCODE -ne 0
}

# Exercise the production result-row renderer against a native TreeView. Record
# its text output boundary, not pixels: a long relative folder must reach drawing
# intact before that API applies ellipsis. Allocation stubs preserve real lengths.
function Test-ResultFolderDrawing([string]$Directory, [string]$Source) {
    $source = Get-Content -LiteralPath $Source -Raw
    $parts = foreach ($name in @('_Sidebar_RelativePath', '_Sidebar_AbbreviateFolder', '_Sidebar_DrawResultRow')) {
        $match = [regex]::Match($source, '(?ms)^(?:static )?[^\r\n]+ ' + $name + '\([^\r\n]*\)\r?\n\{.*?^}')
        if (!$match.Success) { throw "Missing production drawing function $name" }
        $match.Value
    }
    $harness = @'
#include <windows.h>
#include <commctrl.h>
#include <strsafe.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define MAX_PATH_EXPLICIT 260
#define COUNTOF(value) _countof(value)
#define max_i(a,b) (((a) > (b)) ? (a) : (b))
#define min_i(a,b) (((a) < (b)) ? (a) : (b))
typedef LPCWSTR HPATHL;
#define Path_Get(path) (path)
#define Path_Copy(path) _wcsdup(path)
#define Path_GetLength(path) wcslen(path)
#define Path_Release(path) free((void*)(path))
// The renderer only needs a mutable copy and its parent, with no filesystem I/O.
static void Path_RemoveFileSpec(HPATHL path) { LPWSTR tail = wcsrchr((LPWSTR)path, L'\\'); if (tail) *tail = 0; }
typedef struct String { LPWSTR text; size_t capacity; } *HSTRINGW;
static HSTRINGW StrgCreate(LPCWSTR value) { (void)value; return calloc(1, sizeof(struct String)); }
static LPWSTR StrgWriteAccessBuf(HSTRINGW value, size_t length) { value->capacity = length; value->text = calloc(length, sizeof(WCHAR)); return value->text; }
static size_t StrgGetAllocLength(HSTRINGW value) { return value->capacity; }
static void StrgDestroy(HSTRINGW value) { free(value->text); free(value); }
enum { SIDEBAR_FILE, SIDEBAR_RESULT = 7, SIDEBAR_RESULT_CLEAR = 10, SIDEBAR_KIND_MASK = 15, SIDEBAR_COUNT_SHIFT = 4 };
enum { SidebarFontSmall, SidebarFontBody };
typedef struct { COLORREF island, selected, rowHot, accent, weak, secondary; } SidebarPalette;
static SidebarPalette palette = { 1, 2, 3, 4, 5, 6 };
static SidebarPalette const* s_palette = &palette;
static struct { LPCWSTR SidebarFolder; } Settings = { L"C:\\fixture" };
static HWND s_results;
static HTREEITEM s_resultHot;
static void* s_drawing = (void*)1;
static UINT s_dpi = 96, s_resultCount = 1;
static HPATHL s_resultPaths[1];
static WCHAR s_query[] = L"target", drawnFolder[32768];
static int _Sidebar_Pixels(int value) { return value; }
static int Sidebar_GetStrokeWidth(UINT dpi) { (void)dpi; return 1; }
static COLORREF _Sidebar_NameColor(LPCWSTR name, LPARAM kind) { (void)name; (void)kind; return 0; }
static int _Sidebar_MatchName(LPCWSTR name, LPCWSTR query) { (void)name; (void)query; return -1; }
// Text measurement and surfaces are irrelevant to this text-preservation
// contract. Capture the actual weak folder column passed by the real renderer.
static int SidebarDrawing_Measure(HDC dc, void* drawing, LPCWSTR text, int font) { (void)dc; (void)drawing; (void)font; return (int)wcslen(text); }
static void SidebarDrawing_RoundRect(HDC dc, RECT bounds, int radius, int stroke, COLORREF fill, COLORREF border) { (void)dc; (void)bounds; (void)radius; (void)stroke; (void)fill; (void)border; }
static void SidebarDrawing_Text(HDC dc, void* drawing, LPCWSTR text, int font, RECT bounds, COLORREF color) {
    (void)dc; (void)drawing; (void)bounds;
    if (font == SidebarFontSmall && color == palette.weak) StringCchCopy(drawnFolder, COUNTOF(drawnFolder), text);
}
__SOURCE__
// Three valid 100-character components put the relative folder above MAX_PATH;
// the expected text is built from the fixture, independently of abbreviation.
int main(void) {
    WCHAR path[1024] = L"C:\\fixture", expected[1024] = L"";
    for (int i = 0; i < 3; ++i) {
        WCHAR component[101];
        for (int j = 0; j < 100; ++j) component[j] = (WCHAR)(L'a' + i);
        component[100] = 0;
        StringCchCat(path, COUNTOF(path), L"\\"); StringCchCat(path, COUNTOF(path), component);
        StringCchCat(expected, COUNTOF(expected), component); StringCchCat(expected, COUNTOF(expected), L"\\");
    }
    StringCchCat(path, COUNTOF(path), L"\\target.txt"); s_resultPaths[0] = path;
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_TREEVIEW_CLASSES }; InitCommonControlsEx(&controls);
    HWND parent = CreateWindowW(L"STATIC", L"", WS_OVERLAPPED, 0, 0, 500, 500, NULL, NULL, NULL, NULL);
    s_results = CreateWindowW(WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 300, 300, parent, NULL, NULL, NULL);
    TVINSERTSTRUCTW insert = { 0 }; insert.hParent = TVI_ROOT; insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM; insert.item.pszText = L"target.txt"; insert.item.lParam = SIDEBAR_RESULT;
    HTREEITEM item = TreeView_InsertItem(s_results, &insert);
    if (!parent || !s_results || !item) { fprintf(stderr, "Native drawing fixture failed\n"); return 2; }
    NMTVCUSTOMDRAW draw = { 0 }; draw.nmcd.dwItemSpec = (DWORD_PTR)item; draw.nmcd.hdc = GetDC(s_results);
    _Sidebar_DrawResultRow(&draw);
    bool const pass = wcscmp(drawnFolder, expected) == 0;
    printf("%s: F6 result renderer preserves long relative folder column (%zu characters)\n", pass ? "PASS" : "FAIL", wcslen(expected));
    ReleaseDC(s_results, draw.nmcd.hdc); DestroyWindow(parent); return pass ? 0 : 1;
}
'@
    $file = Join-Path $Directory 'ResultFolder.c'
    Set-Content -LiteralPath $file -Value $harness.Replace('__SOURCE__', ($parts -join "`r`n")) -Encoding utf8
    $exe = Join-Path $Directory 'ResultFolder.exe'
    & cl.exe /nologo /W4 /WX /std:c11 /utf-8 /DUNICODE /D_UNICODE $file "/Fo$Directory\" "/Fe$exe" /link user32.lib comctl32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Result folder harness compilation failed' }
    & $exe
    return $LASTEXITCODE -ne 0
}

$temp = Join-Path $env:TEMP ('np3-sidebar-L1-gui-' + [guid]::NewGuid())
$runner = $null
$runnerCreated = 0
$stdoutTask = $null
$stderrTask = $null
[void](New-Item -ItemType Directory -Path $temp)
try {
    $markerFailed = $false
    if ($Scenario -in @('All', 'Filter', 'Rework')) {
        # Stream assertion output separately from the returned failure flag.
        $markerOutput = @(Test-FilterCurrentMarker $temp $SidebarSource)
        $markerFailed = [bool]$markerOutput[-1]
        $markerOutput | Select-Object -SkipLast 1 | Write-Output
        $drawingOutput = @(Test-ResultFolderDrawing $temp $SidebarSource)
        $markerFailed = [bool]$drawingOutput[-1] -or $markerFailed
        $drawingOutput | Select-Object -SkipLast 1 | Write-Output
    }
    Copy-Item -LiteralPath $Executable -Destination (Join-Path $temp 'Notepad3.exe')
    # Keep the isolated instance's menu and dialog strings with its built executable.
    $languages = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Executable).Path) 'lng'
    if (!(Test-Path -LiteralPath $languages -PathType Container)) { throw "Language DLL directory not found: $languages" }
    Copy-Item -LiteralPath $languages -Destination (Join-Path $temp 'lng') -Recurse
    $data = Join-Path $temp 'fixture'
    $themeValue = if ($Theme -eq 'Dark') { 1 } else { 2 }
    [void](New-Item -ItemType Directory -Path (Join-Path $data 'sub'))
    foreach ($file in @('a.txt','b2.txt','b10.txt','sub\inner.txt')) {
        Set-Content -LiteralPath (Join-Path $data $file) -Value "original $file" -Encoding utf8
    }
    @"
[Settings]
SettingsVersion=5
SaveSettings=1
SaveRecentFiles=0
ShowSidebar=1
SidebarWidth=300
SidebarFolder=$data
ReuseWindow=0
SingleFileInstance=0
WinThemeDarkMode=$themeValue
[Settings2]
PreferredLanguageLocaleName=en-US
"@ | Set-Content -LiteralPath (Join-Path $temp 'Notepad3.ini') -Encoding utf8BOM
    $ahk = 'C:\Program Files\AutoHotkey\v2\AutoHotkey64.exe'
    if (!(Test-Path -LiteralPath $ahk)) { throw "AutoHotkey v2 not found: $ahk" }
    $info = [Diagnostics.ProcessStartInfo]::new($ahk)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in @('/ErrorStdOut', (Join-Path $PSScriptRoot 'TestSidebarGui.ahk'), $temp, $Through, $Scenario, $ScreenshotDirectory, $Theme)) {
        [void]$info.ArgumentList.Add($argument)
    }
    $runner = [Diagnostics.Process]::Start($info)
    $runnerCreated = $runner.StartTime.ToFileTimeUtc()
    $stdoutTask = $runner.StandardOutput.ReadToEndAsync()
    $stderrTask = $runner.StandardError.ReadToEndAsync()
    if (!$runner.WaitForExit(120000)) { throw 'Sidebar GUI runner timed out after 120 seconds' }
    if ($runner.ExitCode -ne 0) { throw "Sidebar GUI assertions failed (exit $($runner.ExitCode))" }
    if ($markerFailed) { throw 'Filter current-file marker assertions failed' }
} finally {
    # Verify the PID, creation FILETIME and unique executable path before cleanup.
    $pidFile = Join-Path $temp 'pids.txt'
    if (Test-Path -LiteralPath $pidFile) {
        foreach ($identity in (Get-Content -LiteralPath $pidFile)) {
            $testPid, $created = $identity.Split('|')
            $process = Get-Process -Id ([int]$testPid) -ErrorAction SilentlyContinue
            if ($process -and $process.StartTime.ToFileTimeUtc() -eq [long]$created -and
                $process.Path -eq (Join-Path $temp 'Notepad3.exe')) {
                [void]$process.CloseMainWindow()
                if (!$process.WaitForExit(2000)) {
                    $process.Refresh()
                    if ($process.StartTime.ToFileTimeUtc() -ne [long]$created) { throw 'Test process identity changed before kill' }
                    $process.Kill()
                    $process.WaitForExit()
                }
            }
        }
    }
    if ($runner -and !$runner.HasExited) {
        if ($runner.StartTime.ToFileTimeUtc() -ne $runnerCreated) { throw 'AHK runner identity changed before kill' }
        $runner.Kill()
        $runner.WaitForExit()
    }
    if ($stdoutTask) { $stdoutTask.GetAwaiter().GetResult() }
    if ($stderrTask) { $stderrTask.GetAwaiter().GetResult() }
    if (Test-Path -LiteralPath (Join-Path $temp 'gui.log')) {
        Get-Content -LiteralPath (Join-Path $temp 'gui.log')
    }
    if ($runner -and $runner.HasExited -and $runner.ExitCode -ne 0) {
        Get-Content -LiteralPath (Join-Path $temp 'Notepad3.ini') -TotalCount 35
    }
    $roFile = Join-Path $temp 'fixture\ro.txt'
    if (Test-Path -LiteralPath $roFile) { (Get-Item -LiteralPath $roFile).IsReadOnly = $false }
    # The recursive target must remain the unique L1 fixture directly below TEMP.
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($temp)) -ne [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') -or
        [IO.Path]::GetFileName($temp) -notlike 'np3-sidebar-L1-gui-*') { throw 'Unsafe sidebar GUI cleanup path' }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
