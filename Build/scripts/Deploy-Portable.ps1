#Requires -Version 7.0
# Install or update a portable ZIP (from Make-ForkRelease.ps1) into a fixed folder.
# Program files are replaced; the user's Notepad3.ini, minipath.ini and Favorites
# are kept when they already exist, so updates never reset settings.
# Fails without changing anything while a Notepad3/minipath from Target is running.
param(
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$Target
)
$ErrorActionPreference = 'Stop'
$zipPath = (Resolve-Path -LiteralPath $Zip).Path
$targetPath = [IO.Path]::GetFullPath($Target).TrimEnd('\')

$running = Get-Process Notepad3, minipath -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and [IO.Path]::GetDirectoryName($_.Path).TrimEnd('\') -ieq $targetPath }
if ($running) { throw "Close Notepad3/minipath running from $targetPath first (PID $($running.Id -join ', '))" }

$stage = Join-Path $env:TEMP ('np3-deploy-' + [guid]::NewGuid())
try {
    Expand-Archive -LiteralPath $zipPath -DestinationPath $stage
    [void](New-Item -ItemType Directory -Force -Path $targetPath)
    $keep = 'Notepad3.ini', 'minipath.ini', 'Favorites'
    foreach ($item in Get-ChildItem -LiteralPath $stage -Force) {
        $dest = Join-Path $targetPath $item.Name
        if ($item.Name -in $keep -and (Test-Path -LiteralPath $dest)) { continue }
        # Replace whole program folders (lng, Themes, Docs) so removed files do not linger.
        if ($item.PSIsContainer -and (Test-Path -LiteralPath $dest)) { Remove-Item -LiteralPath $dest -Recurse -Force }
        Copy-Item -LiteralPath $item.FullName -Destination $dest -Recurse -Force
    }
    Join-Path $targetPath 'Notepad3.exe'
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
