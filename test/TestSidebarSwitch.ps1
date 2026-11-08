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
    $sortModes = [regex]::Match($header, 'enum \{ SIDEBAR_SORT_NAME = \d+, SIDEBAR_SORT_EXTENSION = \d+, SIDEBAR_SORT_MODIFIED = \d+ \};')
    if (!$sortModes.Success) { throw 'Missing production sort modes' }
    $parts = foreach ($name in @('_Sidebar_DecideSwitch', '_Sidebar_CompareEntries', '_Sidebar_ClampWidth', '_Sidebar_RelativePath',
            '_Sidebar_CompareSortedEntries', '_Sidebar_BuildPrefix', '_Sidebar_IsBinary', '_Sidebar_MatchName', '_Sidebar_WidthTier', '_Sidebar_AbbreviateFolder')) {
        $match = [regex]::Match($source, '(?ms)^static [^\r\n]+ ' + $name + '\([^\r\n]*\)\r?\n\{.*?^}')
        if (!$match.Success) { throw "Missing production function $name" }
        $match.Value
    }
    $harness = @'
#include <windows.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
typedef enum { SW_SAME_FILE, SW_SAVE_SILENT, SW_ASK_UNTITLED, SW_NO_SAVE } SwitchAction;
'@ + "`n" + $widthRange.Value + "`n" + $sortModes.Value + "`n" + ($parts -join "`n") + @'

static int failures = 0;
// Record an independent contract assertion; main returns failure if any check fails.
static void check(const char* name, bool pass) {
    printf("%s: %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}
// Compare successful output with a contract literal and print both strings on failure.
static void check_text(const char* name, bool success, LPCWSTR actual, LPCWSTR expected) {
    bool const pass = success && wcscmp(actual, expected) == 0;
    check(name, pass);
    if (!pass) fwprintf(stderr, L"  expected: [%ls]; actual: [%ls]; success: %d\n", expected, actual, success);
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

    WCHAR prefix[16];
    bool ancestors[] = { false, true };
    check_text("prefix root non-last", _Sidebar_BuildPrefix(NULL, 0, false, prefix, 16), prefix, L"├─ ");
    check_text("prefix root last", _Sidebar_BuildPrefix(NULL, 0, true, prefix, 16), prefix, L"└─ ");
    check_text("prefix ancestor continuation and completed branch", _Sidebar_BuildPrefix(ancestors, 2, false, prefix, 16), prefix, L"│     ├─ ");
    bool completed[] = { true, true };
    check_text("prefix completed ancestors leave spaces", _Sidebar_BuildPrefix(completed, 2, true, prefix, 16), prefix, L"      └─ ");
    bool continuing[] = { false, false };
    check_text("prefix nested continuations", _Sidebar_BuildPrefix(continuing, 2, true, prefix, 16), prefix, L"│  │  └─ ");
    WCHAR exact[4];
    check_text("prefix exact capacity includes terminator", _Sidebar_BuildPrefix(NULL, 0, true, exact, 4), exact, L"└─ ");
    check("prefix short buffer returns empty", !_Sidebar_BuildPrefix(NULL, 0, true, prefix, 3) && !prefix[0]);
    check("prefix huge depth cannot overflow capacity", !_Sidebar_BuildPrefix(ancestors, SIZE_MAX, true, prefix, 16) && !prefix[0]);

    // This membership table comes from the approved binary policy, not the classifier.
    static const struct { const char* label; LPCWSTR name; } binaryCases[] = {
        { "binary EXE", L"tool.EXE" }, { "binary DLL", L"tool.DLL" }, { "binary MUI", L"tool.MUI" },
        { "binary SYS", L"tool.SYS" }, { "binary OBJ", L"tool.OBJ" }, { "binary LIB", L"tool.LIB" },
        { "binary PDB", L"tool.PDB" }, { "binary OCX", L"tool.OCX" }, { "binary CPL", L"tool.CPL" },
        { "binary DRV", L"tool.DRV" }, { "binary SCR", L"tool.SCR" }, { "binary COM", L"tool.COM" },
        { "binary BIN", L"tool.BIN" }, { "binary PNG", L"image.PNG" }, { "binary JPG", L"image.JPG" },
        { "binary JPEG", L"image.JPEG" }, { "binary GIF", L"image.GIF" }, { "binary BMP", L"image.BMP" },
        { "binary ICO", L"image.ICO" }, { "binary WEBP", L"image.WEBP" }, { "binary TIF", L"image.TIF" },
        { "binary TIFF", L"image.TIFF" }, { "binary ZIP", L"archive.ZIP" }, { "binary 7Z", L"archive.7Z" },
        { "binary RAR", L"archive.RAR" }, { "binary GZ", L"archive.GZ" }, { "binary TAR", L"archive.TAR" },
        { "binary XZ", L"archive.XZ" }, { "binary CAB", L"archive.CAB" }, { "binary MSI", L"archive.MSI" },
        { "binary ISO", L"archive.ISO" }
    };
    for (size_t i = 0; i < sizeof(binaryCases) / sizeof(binaryCases[0]); ++i) {
        check(binaryCases[i].label, _Sidebar_IsBinary(binaryCases[i].name));
    }
    check("binary mixed case", _Sidebar_IsBinary(L"tool.eXe"));
    check("binary lowercase", _Sidebar_IsBinary(L"photo.webp"));
    check("binary no extension stays text", !_Sidebar_IsBinary(L"exe"));
    check("binary dot alone stays text", !_Sidebar_IsBinary(L"."));
    check("binary trailing dot stays text", !_Sidebar_IsBinary(L"notes."));
    check("binary final text extension wins", !_Sidebar_IsBinary(L"tool.exe.txt"));
    check("binary partial extension does not match", !_Sidebar_IsBinary(L"tool.exe2"));
    check("binary empty filename stays text", !_Sidebar_IsBinary(L""));

    check("match ASCII first occurrence", _Sidebar_MatchName(L"ReadME-readme.txt", L"READme") == 0);
    check("match returns substring offset", _Sidebar_MatchName(L"main.CONFIG.ini", L"config") == 5);
    check("match absent returns minus one", _Sidebar_MatchName(L"main.c", L".ini") == -1);
    check("match Chinese substring", _Sidebar_MatchName(L"模块说明.txt", L"说明") == 2);
    check("match accented Unicode case", _Sidebar_MatchName(L"文档Résumé.txt", L"RÉSUMÉ") == 2);
    check("match preserves accent distinctions", _Sidebar_MatchName(L"café.txt", L"CAFE") == -1);
    check("match empty query starts at zero", _Sidebar_MatchName(L"main.c", L"") == 0);
    check("match empty name misses nonempty query", _Sidebar_MatchName(L"", L"c") == -1);
    check("match offset counts UTF16 units", _Sidebar_MatchName(L"\U0001F4C1Résumé.txt", L"résumé") == 2);

    WCHAR folder[256];
    check_text("folder inside root", _Sidebar_AbbreviateFolder(L"C:\\app", L"C:\\app\\Themes", folder, 256), folder, L"Themes\\");
    check_text("folder root itself", _Sidebar_AbbreviateFolder(L"C:\\app", L"C:\\app", folder, 256), folder, L".\\");
    check_text("folder outside DOS root", _Sidebar_AbbreviateFolder(L"C:\\app", L"F:\\work\\src", folder, 256), folder, L"F:\\…\\src\\");
    check_text("folder sibling prefix is outside root", _Sidebar_AbbreviateFolder(L"C:\\app", L"C:\\apple\\src", folder, 256), folder, L"C:\\…\\src\\");
    check_text("folder relative extended UNC", _Sidebar_AbbreviateFolder(L"\\\\server\\share\\app", L"\\\\?\\UNC\\SERVER\\SHARE\\app\\Themes", folder, 256), folder, L"Themes\\");
    check_text("folder outside UNC keeps share", _Sidebar_AbbreviateFolder(L"C:\\app", L"\\\\server\\share\\work\\src", folder, 256), folder, L"\\\\server\\share\\…\\src\\");
    check_text("folder different UNC share is not relative", _Sidebar_AbbreviateFolder(L"\\\\server\\share", L"\\\\server\\shared\\src", folder, 256), folder, L"\\\\server\\shared\\src\\");
    check_text("folder extended DOS root matches ordinary", _Sidebar_AbbreviateFolder(L"\\\\?\\C:\\app", L"c:\\APP", folder, 256), folder, L".\\");
    check_text("folder extended DOS display drops prefix", _Sidebar_AbbreviateFolder(L"C:\\app", L"\\\\?\\F:\\work\\src", folder, 256), folder, L"F:\\…\\src\\");
    check_text("folder extended UNC display drops prefix", _Sidebar_AbbreviateFolder(L"C:\\app", L"\\\\?\\UNC\\server\\share\\work\\src", folder, 256), folder, L"\\\\server\\share\\…\\src\\");
    check_text("folder external drive root", _Sidebar_AbbreviateFolder(L"C:\\app", L"F:\\", folder, 256), folder, L"F:\\");
    check_text("folder external UNC share root", _Sidebar_AbbreviateFolder(L"C:\\app", L"\\\\server\\share\\", folder, 256), folder, L"\\\\server\\share\\");
    check_text("folder direct child omits no components", _Sidebar_AbbreviateFolder(L"C:\\app", L"F:\\src", folder, 256), folder, L"F:\\src\\");
    check_text("folder trailing separator is not duplicated", _Sidebar_AbbreviateFolder(L"C:\\app", L"C:\\app\\Themes\\", folder, 256), folder, L"Themes\\");
    check_text("folder empty root still abbreviates absolute", _Sidebar_AbbreviateFolder(L"", L"F:\\work\\src", folder, 256), folder, L"F:\\…\\src\\");
    check("folder relative input is rejected", !_Sidebar_AbbreviateFolder(L"C:\\app", L"relative\\src", folder, 256) && !folder[0]);
    check("folder insufficient buffer returns empty", !_Sidebar_AbbreviateFolder(L"C:\\app", L"C:\\app", folder, 2) && !folder[0]);

    check("sort name ignores extension and time", _Sidebar_CompareSortedEntries(false, L"b2.z", 1, false, L"b10.c", 99, 0) < 0);
    check("sort extension overrides name", _Sidebar_CompareSortedEntries(false, L"z.c", 1, false, L"a.h", 99, 1) < 0);
    check("sort extension tie uses natural name ignoring case", _Sidebar_CompareSortedEntries(false, L"b10.TXT", 1, false, L"b2.txt", 99, 1) > 0);
    check("sort absent extension comes first", _Sidebar_CompareSortedEntries(false, L"z", 0, false, L"a.c", 0, 1) < 0);
    check("sort newest time overrides name", _Sidebar_CompareSortedEntries(false, L"z.txt", 200, false, L"a.txt", 100, 2) < 0);
    check("sort older time comes later", _Sidebar_CompareSortedEntries(false, L"a.txt", 100, false, L"z.txt", 200, 2) > 0);
    check("sort time tie uses natural name", _Sidebar_CompareSortedEntries(false, L"b2.txt", 100, false, L"b10.txt", 100, 2) < 0);
    check("sort timestamp extremes do not overflow", _Sidebar_CompareSortedEntries(false, L"z", ~(ULONGLONG)0, false, L"a", 0, 2) < 0);
    check("sort folders precede files in extension mode", _Sidebar_CompareSortedEntries(true, L"z.h", 0, false, L"a.c", 100, 1) < 0);
    check("sort folders precede newer files in time mode", _Sidebar_CompareSortedEntries(true, L"z", 0, false, L"a", 100, 2) < 0);
    check("sort unknown mode falls back to name", _Sidebar_CompareSortedEntries(false, L"b2.z", 0, false, L"b10.c", 100, 99) < 0);

    check("width 219 hides detail columns", _Sidebar_WidthTier(219) == 1);
    check("width 220 shows detail columns", _Sidebar_WidthTier(220) == 0);
    check("width 159 also hides actions", _Sidebar_WidthTier(159) == 2);
    check("width 160 retains actions", _Sidebar_WidthTier(160) == 1);
    check("width below minimum uses narrow tier", _Sidebar_WidthTier(0) == 2);
    check("width large uses full tier", _Sidebar_WidthTier(4000) == 0);
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
    $compileArgs = @('/nologo', '/W4', '/WX', '/std:c11', '/utf-8', '/DUNICODE', '/D_UNICODE', $test, "/Fo$temp\", "/Fe$exe", '/link', 'shlwapi.lib', 'user32.lib')
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

# T8 appends a separate harness; the original extraction list and checks stay intact.
$nameTempRoot = [IO.Path]::GetFullPath((Join-Path $env:TEMP 'np3-sidebar\L1-tmp'))
$nameTemp = Join-Path $nameTempRoot ('names-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $nameTemp)
try {
    $nameFunction = [regex]::Match($source, '(?ms)^static [^\r\n]+ _Sidebar_IsValidFileName\([^\r\n]*\)\r?\n\{.*?^}')
    if (!$nameFunction.Success) { throw 'Missing production function _Sidebar_IsValidFileName' }
    $nameHarness = @'
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>
'@ + "`n" + $nameFunction.Value + @'

// Check literal Windows component rules; print both verdicts on a mismatch.
int main(void) {
    static const struct { const char* label; LPCWSTR name; bool valid; } cases[] = {
        { "ordinary file", L"notes.txt", true },
        { "extensionless file", L"README", true },
        { "Unicode file", L"说明😀.txt", true },
        { "hidden-style name", L".gitignore", true },
        { "internal spaces and dots", L"my notes.v2.txt", true },
        { "leading space", L" notes.txt", true },
        { "device prefix is not device", L"CONSOLE.txt", true },
        { "device suffix is not device", L"xNUL", true },
        { "COM0 is ordinary", L"COM0.txt", true },
        { "LPT10 is ordinary", L"LPT10.txt", true },
        { "null name", NULL, false }, { "empty name", L"", false },
        { "space only", L" ", false }, { "dot only", L".", false },
        { "parent dot", L"..", false }, { "dots only", L"...", false },
        { "trailing dot", L"note.", false }, { "trailing space", L"note ", false },
        { "backslash", L"sub\\note", false }, { "slash", L"sub/note", false },
        { "colon or alternate stream", L"note:stream", false },
        { "asterisk", L"note*", false }, { "question mark", L"note?", false },
        { "quote", L"note\"", false }, { "less than", L"note<", false },
        { "greater than", L"note>", false }, { "pipe", L"note|", false },
        { "control character", L"note\x1f", false },
        { "CON device", L"CON", false }, { "PRN device extension", L"prn.txt", false },
        { "AUX mixed case", L"AuX", false }, { "NUL multiple extensions", L"NUL.a.txt", false },
        { "CON space before extension", L"CON .txt", false },
        { "COM1 device", L"com1.txt", false }, { "COM9 device", L"COM9", false },
        { "LPT1 device", L"LPT1", false }, { "LPT9 device", L"lpt9.log", false },
        { "COM superscript one", L"COM¹.txt", false },
        { "LPT superscript two", L"LPT²", false },
        { "COM superscript three", L"COM³", false },
        { "console input device", L"CONIN$", false },
        { "console output device", L"CONOUT$.txt", false }
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        bool const actual = _Sidebar_IsValidFileName(cases[i].name);
        printf("%s: filename %s\n", actual == cases[i].valid ? "PASS" : "FAIL", cases[i].label);
        if (actual != cases[i].valid) {
            fwprintf(stderr, L"  input: [%ls]; expected: %d; actual: %d\n", cases[i].name ? cases[i].name : L"(null)", cases[i].valid, actual);
            ++failures;
        }
    }
    WCHAR boundary[257];
    for (int i = 0; i < 256; ++i) { boundary[i] = L'a'; }
    boundary[255] = L'\0';
    bool const valid255 = _Sidebar_IsValidFileName(boundary);
    printf("%s: filename 255 UTF16 units accepted\n", valid255 ? "PASS" : "FAIL");
    failures += !valid255;
    boundary[255] = L'a'; boundary[256] = L'\0';
    bool const valid256 = _Sidebar_IsValidFileName(boundary);
    printf("%s: filename 256 UTF16 units rejected\n", !valid256 ? "PASS" : "FAIL");
    failures += valid256;
    return failures ? 1 : 0;
}
'@
    $nameTest = Join-Path $nameTemp 'SidebarNames.c'
    Set-Content -LiteralPath $nameTest -Value $nameHarness -Encoding utf8
    $nameExe = Join-Path $nameTemp 'SidebarNames.exe'
    $nameArgs = @('/nologo', '/W4', '/WX', '/std:c11', '/utf-8', '/DUNICODE', '/D_UNICODE', $nameTest, "/Fo$nameTemp\", "/Fe$nameExe", '/link', 'kernel32.lib')
    & cl.exe @nameArgs
    if ($LASTEXITCODE -ne 0) { throw 'Sidebar filename harness compilation failed' }
    & $nameExe
    if ($LASTEXITCODE -ne 0) { throw 'Sidebar filename assertions failed' }
} finally {
    # Only this unique filename harness can be removed, never the shared root.
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($nameTemp)) -ne $nameTempRoot -or
        [IO.Path]::GetFileName($nameTemp) -notlike 'names-*') { throw 'Unsafe sidebar filename harness cleanup path' }
    Remove-Item -LiteralPath $nameTemp -Recurse -Force
}
