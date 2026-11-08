# Compile the sidebar's production policy functions without the window or editor.
# Expected values come from the sidebar contract; no test-only production API is used.
param([string]$SidebarSource = (Join-Path $PSScriptRoot '..\src\Sidebar.c'))
$ErrorActionPreference = 'Stop'
$tempRoot = [IO.Path]::GetFullPath((Join-Path $env:TEMP 'np3-sidebar\L1-tmp'))
$temp = Join-Path $tempRoot ('switch-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $temp)
try {
    $source = Get-Content -LiteralPath $SidebarSource -Raw
    $header = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\src\Sidebar.h') -Raw
    $widthRange = [regex]::Match($header, 'enum \{ SIDEBAR_MIN_WIDTH = \d+, SIDEBAR_MAX_WIDTH = \d+ \};')
    if (!$widthRange.Success) { throw 'Missing production logical width range' }
    $parts = foreach ($name in @('_Sidebar_DecideSwitch', '_Sidebar_CompareEntries', '_Sidebar_ClampWidth', '_Sidebar_RelativePath')) {
        $match = [regex]::Match($source, '(?ms)^static [^\r\n]+ ' + $name + '\([^\r\n]*\)\r?\n\{.*?^}')
        if (!$match.Success) { throw "Missing production function $name" }
        $match.Value
    }
    $harness = @'
#include <windows.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <stdio.h>
typedef enum { SW_SAME_FILE, SW_SAVE_SILENT, SW_ASK_UNTITLED, SW_NO_SAVE } SwitchAction;
'@ + "`n" + $widthRange.Value + "`n" + ($parts -join "`n") + @'

static int failures = 0;
// Record an independent contract assertion; main returns failure if any check fails.
static void check(const char* name, bool pass) {
    printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}
// Exercise decision combinations, ordering and DPI boundaries without a UI fixture.
int main(void) {
    check("same file keeps current document", _Sidebar_DecideSwitch(true, true, true) == SW_SAME_FILE);
    check("clean named document needs no save", _Sidebar_DecideSwitch(false, false, true) == SW_NO_SAVE);
    check("clean untitled document needs no save", _Sidebar_DecideSwitch(false, false, false) == SW_NO_SAVE);
    check("modified named document saves silently", _Sidebar_DecideSwitch(false, true, true) == SW_SAVE_SILENT);
    check("modified untitled document asks", _Sidebar_DecideSwitch(false, true, false) == SW_ASK_UNTITLED);
    check("folder sorts before file", _Sidebar_CompareEntries(true, L"z", false, L"a") < 0);
    check("file sorts after folder", _Sidebar_CompareEntries(false, L"a", true, L"z") > 0);
    check("natural numeric order", _Sidebar_CompareEntries(false, L"b2.txt", false, L"b10.txt") < 0);
    check("case insensitive order", _Sidebar_CompareEntries(false, L"A.txt", false, L"a.TXT") == 0);
    check("minimum width", _Sidebar_ClampWidth(50, 1000, 96) == 100);
    check("maximum width reserves editor", _Sidebar_ClampWidth(900, 1000, 96) == 800);
    check("normal width", _Sidebar_ClampWidth(300, 1000, 96) == 300);
    check("144 DPI scales requested width", _Sidebar_ClampWidth(300, 1500, 144) == 450);
    check("144 DPI scales minimum", _Sidebar_ClampWidth(50, 1500, 144) == 150);
    check("144 DPI scales editor reserve", _Sidebar_ClampWidth(1000, 1500, 144) == 1200);
    check("large client caps logical width at 4000", _Sidebar_ClampWidth(5000, 6000, 96) == 4000);
    check("144 DPI caps logical width at 4000", _Sidebar_ClampWidth(5000, 9000, 144) == 6000);
    check("4000 logical pixels remain allowed", _Sidebar_ClampWidth(4000, 6000, 96) == 4000);
    check("editor reserve overrides 4000 cap", _Sidebar_ClampWidth(5000, 6000, 144) == 5700);
    WCHAR longTarget[512] = L"\\\\?\\C:\\root\\";
    for (int i = 12; i < 300; ++i) { longTarget[i] = (i == 92 || i == 173 || i == 254) ? L'\\' : L'x'; }
    longTarget[300] = L'\0';
    LPCWSTR relative = _Sidebar_RelativePath(L"C:\\root", longTarget);
    check("extended target beyond 260 stays inside short DOS root", relative && wcslen(relative) == 288);
    relative = _Sidebar_RelativePath(L"\\\\?\\C:\\root", L"c:\\ROOT\\file.txt");
    check("extended DOS root matches ordinary target", relative && wcscmp(relative, L"file.txt") == 0);
    relative = _Sidebar_RelativePath(L"\\\\server\\share\\root", L"\\\\?\\UNC\\SERVER\\SHARE\\root\\file.txt");
    check("extended UNC target matches ordinary UNC root", relative && wcscmp(relative, L"file.txt") == 0);
    relative = _Sidebar_RelativePath(L"\\\\?\\UNC\\server\\share\\root", L"\\\\server\\share\\root\\file.txt");
    check("extended UNC root matches ordinary UNC target", relative && wcscmp(relative, L"file.txt") == 0);
    check("DOS sibling prefix is outside root", !_Sidebar_RelativePath(L"C:\\a", L"\\\\?\\C:\\ab\\x"));
    check("UNC sibling prefix is outside root", !_Sidebar_RelativePath(L"\\\\server\\share\\a", L"\\\\?\\UNC\\server\\share\\ab\\x"));
    relative = _Sidebar_RelativePath(L"C:\\", L"\\\\?\\C:\\file.txt");
    check("drive root accepts child", relative && wcscmp(relative, L"file.txt") == 0);
    relative = _Sidebar_RelativePath(L"C:\\root", L"\\\\?\\C:\\ROOT");
    check("same directory yields empty suffix", relative && !*relative);
    check("different UNC share is outside root", !_Sidebar_RelativePath(L"\\\\server\\share", L"\\\\?\\UNC\\server\\other\\x"));
    return failures ? 1 : 0;
}
'@
    $test = Join-Path $temp 'SidebarSwitch.c'
    Set-Content -LiteralPath $test -Value $harness -Encoding utf8
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsArgs = @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
    $vs = & $vswhere @vsArgs
    if (!$vs) { throw 'Visual C++ build tools not found' }
    $env:PATH = (Split-Path -Parent $vswhere) + [IO.Path]::PathSeparator + $env:PATH
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $exe = Join-Path $temp 'SidebarSwitch.exe'
    $compileArgs = @('/nologo', '/W4', '/WX', '/std:c11', $test, "/Fo$temp\", "/Fe$exe", '/link', 'shlwapi.lib', 'user32.lib')
    & cl.exe @compileArgs
    if ($LASTEXITCODE -ne 0) { throw 'Sidebar policy harness compilation failed' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Sidebar policy assertions failed' }
} finally {
    # Check the absolute target before recursive cleanup; never remove the shared root.
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($temp)) -ne $tempRoot -or
        [IO.Path]::GetFileName($temp) -notlike 'switch-*') { throw 'Unsafe sidebar harness cleanup path' }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
