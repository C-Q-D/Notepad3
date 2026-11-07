# Run only isolated Notepad3 instances and retain no user or fixture state.
# The AHK assertions use messages addressed to the test window, never global input.
param(
    [ValidateSet('A','B','C','D','E','F')][string]$Through = 'F',
    [string]$Executable = (Join-Path $PSScriptRoot '..\Bin\Release_x64_v143\Notepad3.exe')
)
$ErrorActionPreference = 'Stop'
$temp = Join-Path $env:TEMP ('np3-sidebar-L1-gui-' + [guid]::NewGuid())
$runner = $null
$stdoutTask = $null
$stderrTask = $null
[void](New-Item -ItemType Directory -Path $temp)
try {
    Copy-Item -LiteralPath $Executable -Destination (Join-Path $temp 'Notepad3.exe')
    $data = Join-Path $temp 'fixture'
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
    foreach ($argument in @('/ErrorStdOut', (Join-Path $PSScriptRoot 'TestSidebarGui.ahk'), $temp, $Through)) {
        [void]$info.ArgumentList.Add($argument)
    }
    $runner = [Diagnostics.Process]::Start($info)
    $stdoutTask = $runner.StandardOutput.ReadToEndAsync()
    $stderrTask = $runner.StandardError.ReadToEndAsync()
    if (!$runner.WaitForExit(120000)) { throw 'Sidebar GUI runner timed out after 120 seconds' }
    if ($runner.ExitCode -ne 0) { throw "Sidebar GUI assertions failed (exit $($runner.ExitCode))" }
} finally {
    # PID ownership is verified against the unique copied executable before cleanup.
    $pidFile = Join-Path $temp 'pids.txt'
    if (Test-Path -LiteralPath $pidFile) {
        foreach ($testPid in (Get-Content -LiteralPath $pidFile)) {
            $process = Get-Process -Id ([int]$testPid) -ErrorAction SilentlyContinue
            if ($process -and $process.Path -eq (Join-Path $temp 'Notepad3.exe')) {
                [void]$process.CloseMainWindow()
                if (!$process.WaitForExit(2000)) { $process.Kill(); $process.WaitForExit() }
            }
        }
    }
    if ($runner -and !$runner.HasExited) { $runner.Kill(); $runner.WaitForExit() }
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
    Remove-Item -LiteralPath $temp -Recurse -Force
}
