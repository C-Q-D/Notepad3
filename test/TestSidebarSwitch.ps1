# Compile the sidebar's production policy functions without the window or editor.
# Expected values come from the sidebar contract; no test-only production API is used.
param([string]$SidebarSource = (Join-Path $PSScriptRoot '..\src\Sidebar.c'))
$ErrorActionPreference = 'Stop'
$temp = Join-Path $env:TEMP ('np3-sidebar-L1-switch-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $temp)
try {
    $source = Get-Content -LiteralPath $SidebarSource -Raw
    $parts = foreach ($name in @('_Sidebar_DecideSwitch', '_Sidebar_CompareEntries', '_Sidebar_ClampWidth')) {
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
'@ + "`n" + ($parts -join "`n") + @'

static int failures = 0;
static void check(const char* name, bool pass) {
    printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}
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
    Remove-Item -LiteralPath $temp -Recurse -Force
}
