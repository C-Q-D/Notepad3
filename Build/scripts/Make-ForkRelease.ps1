#Requires -Version 7.0
# Package the fork's x64 portable ZIP from an existing Release x64 build.
# Mirrors Build\make_portable_rel(.zip).cmd for x64 only, without grepWin
# (not built by this fork) and without 7-Zip (uses .NET ZipFile instead).
# Output: Build\Packages\Notepad3_<Version>_x64_Portable.zip; prints its path.
param(
    # Version string used in the file name, e.g. 7.26.1107.1-fork.1.
    [Parameter(Mandatory)][string]$Version,
    # Build output folder that contains Notepad3.exe, minipath.exe, np3encrypt.exe and lng\.
    [string]$BinDir = (Join-Path $PSScriptRoot '..\..\Bin\Release_x64_v143')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$build = Resolve-Path (Join-Path $PSScriptRoot '..')
$bin = Resolve-Path $BinDir
foreach ($exe in 'Notepad3.exe', 'minipath.exe', 'np3encrypt.exe') {
    if (!(Test-Path -LiteralPath (Join-Path $bin $exe))) { throw "Missing $exe in $bin; build Release x64 first" }
}

$packages = Join-Path $build 'Packages'
[void](New-Item -ItemType Directory -Force -Path $packages)
$stage = Join-Path $packages ('stage-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $stage)
try {
    Copy-Item -LiteralPath (Join-Path $bin 'Notepad3.exe'), (Join-Path $bin 'minipath.exe'), (Join-Path $bin 'np3encrypt.exe') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $build 'Notepad3_ptb_x64.ini') -Destination (Join-Path $stage 'Notepad3.ini')
    Copy-Item -LiteralPath (Join-Path $build 'minipath.ini') -Destination $stage
    # Same exclusions as Build\Ignore.txt: build intermediates and the built-in en-US MUI.
    $lng = Join-Path $stage 'lng'
    Copy-Item -LiteralPath (Join-Path $bin 'lng') -Destination $lng -Recurse
    foreach ($skip in 'obj', 'en-US') {
        $path = Join-Path $lng $skip
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    }
    Copy-Item -LiteralPath (Join-Path $build 'Themes') -Destination (Join-Path $stage 'Themes') -Recurse
    Copy-Item -LiteralPath (Join-Path $build 'Docs') -Destination (Join-Path $stage 'Docs') -Recurse
    Copy-Item -LiteralPath (Join-Path $build 'Changes.txt') -Destination (Join-Path $stage 'Docs')
    [void](New-Item -ItemType Directory -Path (Join-Path $stage 'Favorites'))

    $zip = Join-Path $packages "Notepad3_${Version}_x64_Portable.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
    # CreateFromDirectory keeps the empty Favorites folder that the INI points to.
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
    $zip
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
