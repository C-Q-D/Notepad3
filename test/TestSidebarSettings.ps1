# Verify sidebar INI persistence through isolated application startups and normal exits.
# The fixture owns its executable, settings and process; no production test API is used.
param([string]$Executable = (Join-Path $PSScriptRoot '..\Bin\Release_x64_v143\Notepad3.exe'))
$ErrorActionPreference = 'Stop'
$temp = Join-Path $env:TEMP ('np3-sidebar-settings-' + [guid]::NewGuid())
$script:failures = 0
$process = $null
$created = 0

Add-Type @'
using System;
using System.Runtime.InteropServices;
// Use only handles belonging to the isolated process when measuring or closing it.
public static class SidebarSettingsWindow {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    private delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumThreadWindows(uint thread, WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, System.Text.StringBuilder name, int capacity);
    // Enumerate direct children by class even across desktops. The explicit
    // parent keeps another pane's TreeView from satisfying this fixture.
    public static IntPtr FindChild(IntPtr parent, string name) {
        IntPtr found = IntPtr.Zero;
        if (parent == IntPtr.Zero) return found;
        EnumChildWindows(parent, (window, parameter) => {
            var actual = new System.Text.StringBuilder(256);
            GetClassName(window, actual, actual.Capacity);
            if (GetParent(window) != parent || actual.ToString() != name) return true;
            found = window;
            return false;
        }, IntPtr.Zero);
        return found;
    }
    // Enumerate the fixture's UI threads, because a PowerShell worker thread
    // can be attached to a different desktop than its newly started process.
    public static IntPtr FindMain(int process) {
        IntPtr found = IntPtr.Zero;
        using (var fixture = System.Diagnostics.Process.GetProcessById(process)) {
            foreach (System.Diagnostics.ProcessThread thread in fixture.Threads) {
                EnumThreadWindows((uint)thread.Id, (window, parameter) => {
                    if (FindChild(window, "Notepad3Sidebar") == IntPtr.Zero) return true;
                    found = window;
                    return false;
                }, IntPtr.Zero);
                if (found != IntPtr.Zero) break;
            }
        }
        return found;
    }
}
'@

<#
.SYNOPSIS
Record an independent assertion and count failures without masking later contracts.
.PARAMETER Pass
Whether the observed fixture state satisfies the contract.
.PARAMETER Name
Contract description printed with PASS or FAIL.
#>
function Assert-Setting([bool]$Pass, [string]$Name) {
    if ($Pass) { "PASS: $Name" }
    else { "FAIL: $Name"; $script:failures++ }
}

<#
.SYNOPSIS
Read only the fixture's Settings section so other sections cannot satisfy an assertion.
.OUTPUTS
A hashtable of saved key/value strings; missing keys are absent.
#>
function Read-FixtureSettings {
    $values = @{}
    $inSettings = $false
    foreach ($line in (Get-Content -LiteralPath (Join-Path $temp 'Notepad3.ini'))) {
        if ($line -match '^\[(.+)\]$') { $inSettings = $Matches[1] -eq 'Settings' }
        elseif ($inSettings -and $line -match '^([^;=]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
    }
    return $values
}

<#
.SYNOPSIS
Start the isolated executable and close it normally to exercise SaveAllSettings.
.OUTPUTS
The visible panel width in 96-DPI logical pixels.
.NOTES
Timeouts fail without killing a process; the caller retains cleanup responsibility.
#>
function Invoke-Fixture {
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $temp 'Notepad3.exe'))
    $info.UseShellExecute = $false
    $info.WorkingDirectory = $temp
    [void]$info.ArgumentList.Add('/n')
    $script:process = [Diagnostics.Process]::Start($info)
    $script:created = $process.StartTime.ToFileTimeUtc()
    "$($process.Id)|$created" | Set-Content -LiteralPath (Join-Path $temp 'process.txt')
    if (!$process.WaitForInputIdle(15000)) { throw 'Fixture UI did not become idle within 15 seconds' }
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($process.HasExited) { throw 'Fixture exited before its main window appeared' }
        $process.Refresh()
        $main = [SidebarSettingsWindow]::FindMain($process.Id)
        $panel = [SidebarSettingsWindow]::FindChild($main, 'Notepad3Sidebar')
        $tree = if ($main -ne [IntPtr]::Zero -and $panel -ne [IntPtr]::Zero) {
            [SidebarSettingsWindow]::FindChild($panel, 'SysTreeView32')
        } else { [IntPtr]::Zero }
        $readyRect = [SidebarSettingsWindow+Rect]::new()
        if ($tree -ne [IntPtr]::Zero -and [SidebarSettingsWindow]::IsWindowVisible($panel) -and
            [SidebarSettingsWindow]::GetWindowRect($panel, [ref]$readyRect) -and $readyRect.Right -gt $readyRect.Left) { break }
        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($tree -eq [IntPtr]::Zero) { throw 'Fixture tree did not appear within 15 seconds' }
    $rect = [SidebarSettingsWindow+Rect]::new()
    if (![SidebarSettingsWindow]::GetWindowRect($panel, [ref]$rect)) { throw 'Cannot measure fixture panel' }
    $dpi = [SidebarSettingsWindow]::GetDpiForWindow($main)
    if (!$dpi) { throw 'Cannot read fixture DPI' }
    $width = ($rect.Right - $rect.Left) * 96.0 / $dpi
    if ($process.StartTime.ToFileTimeUtc() -ne $created -or $process.Path -ne (Join-Path $temp 'Notepad3.exe')) {
        throw 'Fixture process identity changed before normal close'
    }
    if (![SidebarSettingsWindow]::PostMessage($main, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) { throw 'Cannot request normal fixture close' }
    if (!$process.WaitForExit(15000)) { throw 'Fixture did not exit normally within 15 seconds' }
    if ($process.ExitCode -ne 0) { throw "Fixture exit code: $($process.ExitCode)" }
    return $width
}

<#
.SYNOPSIS
Replace only the fixture INI so each case starts with independent input values.
.PARAMETER Values
Additional Settings lines; an empty string tests missing sidebar keys.
#>
function Write-FixtureSettings([string]$Values) {
    @"
[Settings]
SettingsVersion=5
SaveSettings=1
SaveRecentFiles=0
ShowSidebar=1
WinThemeDarkMode=2
$Values
[Settings2]
ReuseWindow=0
SingleFileInstance=0
PreferredLanguageLocaleName=en-US
"@ | Set-Content -LiteralPath (Join-Path $temp 'Notepad3.ini') -Encoding utf8BOM
}

try {
    [void](New-Item -ItemType Directory -Path $temp)
    Copy-Item -LiteralPath $Executable -Destination (Join-Path $temp 'Notepad3.exe')
    $languages = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Executable).Path) 'lng'
    Copy-Item -LiteralPath $languages -Destination (Join-Path $temp 'lng') -Recurse
    $keys = @('SidebarWidth', 'SidebarSortMode', 'SidebarShowHidden', 'SidebarRecentCollapsed', 'SidebarFoldersCollapsed', 'SidebarRecentHidden')

    Write-FixtureSettings ''
    $width = Invoke-Fixture
    Assert-Setting ([Math]::Abs($width - 300) -le 4) "missing keys use 300 logical pixel default width (actual $width)"
    $saved = Read-FixtureSettings
    foreach ($key in $keys) { Assert-Setting (!$saved.ContainsKey($key)) "missing default $key is not written" }

    Write-FixtureSettings @'
SidebarWidth=300
SidebarSortMode=0
SidebarShowHidden=1
SidebarRecentCollapsed=0
SidebarFoldersCollapsed=0
SidebarRecentHidden=
'@
    $null = Invoke-Fixture
    $saved = Read-FixtureSettings
    foreach ($key in $keys) { Assert-Setting (!$saved.ContainsKey($key)) "explicit default $key is removed" }

    $hidden = (Join-Path $temp '中文 file.ini') + '|' + (Join-Path $temp 'other.c')
    Write-FixtureSettings @"
SidebarWidth=240
SidebarSortMode=1
SidebarShowHidden=0
SidebarRecentCollapsed=1
SidebarFoldersCollapsed=1
SidebarRecentHidden=$hidden
"@
    $width = Invoke-Fixture
    Assert-Setting ([Math]::Abs($width - 240) -le 4) "saved 240 logical pixel width is preserved (actual $width)"
    $saved = Read-FixtureSettings
    Assert-Setting ($saved.SidebarWidth -ceq '240') "non-default width is saved (actual $($saved.SidebarWidth))"
    Assert-Setting ($saved.SidebarSortMode -ceq '1') 'extension sort is saved'
    # IniSectionSetBool uses SimpleIni's documented true/false output, not numeric input spelling.
    Assert-Setting ($saved.SidebarShowHidden -ceq 'false') 'hidden-file preference is saved as false'
    Assert-Setting ($saved.SidebarRecentCollapsed -ceq 'true') 'recent group collapsed state is saved as true'
    Assert-Setting ($saved.SidebarFoldersCollapsed -ceq 'true') 'folders group collapsed state is saved as true'
    Assert-Setting ($saved.SidebarRecentHidden -ceq $hidden) 'Unicode hidden recent paths round-trip with pipe separators'
    $null = Invoke-Fixture
    $saved = Read-FixtureSettings
    Assert-Setting ($saved.SidebarSortMode -ceq '1' -and $saved.SidebarShowHidden -ceq 'false' -and
        $saved.SidebarRecentCollapsed -ceq 'true' -and $saved.SidebarFoldersCollapsed -ceq 'true' -and
        $saved.SidebarRecentHidden -ceq $hidden) 'non-default preferences survive a second startup and normal exit'

    Write-FixtureSettings 'SidebarSortMode=2'
    $null = Invoke-Fixture
    $saved = Read-FixtureSettings
    Assert-Setting ($saved.SidebarSortMode -ceq '2') 'modified-time sort is saved'

    Write-FixtureSettings 'SidebarSortMode=99'
    $null = Invoke-Fixture
    $saved = Read-FixtureSettings
    Assert-Setting ($saved.SidebarSortMode -ceq '2') 'sort above range clamps to modified-time sort'

    Write-FixtureSettings 'SidebarSortMode=-1'
    $null = Invoke-Fixture
    $saved = Read-FixtureSettings
    Assert-Setting (!$saved.ContainsKey('SidebarSortMode')) 'sort below range clamps to default and is omitted'

    $longParent = Join-Path $temp ('deep\' * 240)
    $hidden32 = (1..32 | ForEach-Object { Join-Path $longParent "中文-$_.ini" }) -join '|'
    Write-FixtureSettings "SidebarRecentHidden=$hidden32"
    $null = Invoke-Fixture
    $saved = Read-FixtureSettings
    Assert-Setting ($saved.SidebarRecentHidden -ceq $hidden32) '32 long hidden paths survive beyond a single-path buffer'

    Write-FixtureSettings ("SidebarRecentHidden=$hidden32|" + (Join-Path $temp 'overflow.ini'))
    $null = Invoke-Fixture
    $saved = Read-FixtureSettings
    Assert-Setting ($saved.SidebarRecentHidden -ceq $hidden32) 'hidden list retains the first 32 paths without a partial path'
} catch {
    Write-Output $_
    throw
} finally {
    # PID, creation time and executable path must all match before requesting close.
    if ($process -and !$process.HasExited) {
        $process.Refresh()
        if ($process.StartTime.ToFileTimeUtc() -ne $created -or $process.Path -ne (Join-Path $temp 'Notepad3.exe')) {
            throw 'Fixture process identity changed; leave it and its files intact'
        }
        $main = [SidebarSettingsWindow]::FindMain($process.Id)
        if ($main -ne [IntPtr]::Zero) {
            [void][SidebarSettingsWindow]::PostMessage($main, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
        }
        if (!$process.WaitForExit(15000)) { throw "Fixture still running; files retained at $temp" }
    }
    # Delete only the unique directory created by this run, never the shared TEMP root.
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($temp)) -ne [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') -or
        [IO.Path]::GetFileName($temp) -notlike 'np3-sidebar-settings-*') { throw 'Unsafe settings fixture cleanup path' }
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
if ($script:failures) { throw "$script:failures sidebar settings assertions failed" }
