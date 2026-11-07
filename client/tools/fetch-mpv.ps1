<#
.SYNOPSIS
  Downloads a pinned libmpv dev build (shinchiro) into client/third_party/mpv and builds an MSVC import lib.

.DESCRIPTION
  Run from a Visual Studio x64 developer shell (needs dumpbin.exe and lib.exe on PATH) and with
  7z.exe available (on PATH or in C:\Program Files\7-Zip). The archive uses BCJ2, which Windows tar
  cannot extract. Result: third_party/mpv/{include/mpv/*.h, libmpv-2.dll, libmpv.dll.a, mpv.def, mpv.lib}.
  The archive ships no .def/.lib for MSVC, so mpv.def is generated from the DLL's export table.
#>
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Pinned: shinchiro/mpv-winbuild-cmake release 20261107, non-v3 x86_64 dev package.
$Url    = 'https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261107/mpv-dev-x86_64-20261107-git-9028187538.7z'
$Sha256 = 'df7c433a2d481fdaf6163b201f26abbb0eff0c215c51368397088c6d8f7a001b'

$Dest  = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\third_party\mpv'))
$Stamp = Join-Path $Dest '.pinned-sha256'

if ((Test-Path (Join-Path $Dest 'mpv.lib')) -and (Test-Path $Stamp) -and
    ((Get-Content $Stamp -Raw).Trim() -eq $Sha256)) {
    Write-Host "libmpv already present in $Dest"
    return
}

foreach ($tool in 'dumpbin', 'lib') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool.exe not found. Run this from a VS x64 Native Tools / Developer PowerShell."
    }
}

$sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
if (-not $sevenZip) {
    $candidate = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
    if (Test-Path $candidate) { $sevenZip = $candidate }
}
if (-not $sevenZip) { throw '7z.exe not found. Install 7-Zip (winget install 7zip.7zip).' }

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("couchsync-mpv-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $archive = Join-Path $work 'mpv-dev.7z'
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $archive -UseBasicParsing

    $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "SHA256 mismatch: expected $Sha256, got $actual" }

    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    New-Item -ItemType Directory -Path $Dest | Out-Null
    & $sevenZip x $archive "-o$Dest" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z extraction failed ($LASTEXITCODE)" }

    $dll = Join-Path $Dest 'libmpv-2.dll'
    if (-not (Test-Path $dll)) { throw "libmpv-2.dll missing after extraction" }

    # Generate mpv.def from the DLL exports, then build the MSVC import library.
    $names = foreach ($line in (& dumpbin /exports $dll)) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $Matches[1] }
    }
    if (-not ($names -contains 'mpv_create')) { throw 'dumpbin output parsing failed (mpv_create not found)' }
    $def = Join-Path $Dest 'mpv.def'
    (@('LIBRARY libmpv-2.dll', 'EXPORTS') + $names) | Set-Content -Path $def -Encoding ascii

    & lib "/def:$def" /machine:x64 "/out:$(Join-Path $Dest 'mpv.lib')" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib.exe failed ($LASTEXITCODE)" }

    Set-Content -Path $Stamp -Value $Sha256 -Encoding ascii
    Write-Host "libmpv ready in $Dest ($($names.Count) exports)"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
