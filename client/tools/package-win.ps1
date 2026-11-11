<#
  Stages the Windows runtime files into CouchSync-<version>-win64/, zips them and writes SHA256SUMS.txt.
  Used by .github/workflows/release.yml; can be run locally on a Release build:
    ./client/tools/package-win.ps1 -Version 0.2.0 -OutDir some\dir
  Exits non-zero if the staged exe reports no TLS support (--check-tls) or the staging is incomplete.
#>
param(
  [Parameter(Mandatory)][string]$Version,
  [Parameter(Mandatory)][string]$OutDir,
  [string]$BuildDir,
  [string]$RepoRoot
)
$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $scriptDir '..\build\windows-msvc\Release' }
if (-not $RepoRoot) { $RepoRoot = Join-Path $scriptDir '..\..' }

$BuildDir = (Resolve-Path $BuildDir).Path
$RepoRoot = (Resolve-Path $RepoRoot).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$name  = "CouchSync-$Version-win64"
$stage = Join-Path $OutDir $name
$zip   = Join-Path $OutDir "$name.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip)   { Remove-Item -Force $zip }

# Runtime files only: no import libs, debug info, test/sim exes, qml debugger plugins, or generated sources.
robocopy $BuildDir $stage /E /NFL /NDL /NJH /NJS /NP `
  /XD CMakeFiles qmltooling `
  /XF *.lib *.pdb *.ilk *.exp *.obj *.qmltypes *.cpp *.h *.json *.mkv *.mp4 couchsync_sim.exe 'test_*.exe' | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
$global:LASTEXITCODE = 0

foreach ($req in 'couchsync.exe', 'libmpv-2.dll', 'tls\qschannelbackend.dll', 'platforms\qwindows.dll', 'qml') {
  if (-not (Test-Path (Join-Path $stage $req))) { throw "staged output is missing $req" }
}

Copy-Item (Join-Path $RepoRoot 'LICENSE') $stage

$redistNote = ''
if (Test-Path (Join-Path $stage 'vc_redist.x64.exe')) {
  $redistNote = @'

If the program does not start (missing VCRUNTIME140.dll or similar), run vc_redist.x64.exe once.
'@
}

@"
CouchSync $Version (Windows x64) - EARLY TEST BUILD
=====================================================

CouchSync is a media player with watch-together sync. This is an early test
build: expect rough edges, and please report problems at
https://github.com/Cleverdee/CouchSync/issues

Run
---
1. Unzip the whole folder somewhere (do not run it from inside the zip).
2. Double-click couchsync.exe. Open a video with Ctrl+O, the Open button,
   or by dragging a file onto the window.$redistNote

Join a room
-----------
Everyone needs their own copy of the SAME video file.
1. Click "Room" in the controls bar ("Watch together" dialog).
2. Enter your name and the server address the host gave you (for example
   wss://something.trycloudflare.com/ws), choose "Join room", and enter the
   room code - or paste the couchsync:// invite link instead.
3. Open the same video file; playback then stays in sync for everyone.
To host, enter the server address and choose "Create room", then share the
invite link. Rooms need a running CouchSync room server reachable over wss://.

Troubleshooting: run  couchsync.exe --check-tls  from a terminal; it should
print "supportsSsl: true".

License and third-party notices
-------------------------------
CouchSync is free software under the GNU GPL v3 (see LICENSE).
Source code: https://github.com/Cleverdee/CouchSync

This package bundles libmpv (libmpv-2.dll), which is licensed under the
GPL v2+ / LGPL v2.1+ depending on build options (the bundled build is
GPL-enabled). mpv source: https://github.com/mpv-player/mpv
The binary is an unmodified build from shinchiro's mpv-winbuild-cmake:
https://github.com/shinchiro/mpv-winbuild-cmake (releases:
https://github.com/shinchiro/mpv-winbuild-cmake/releases). Its build
scripts and the source of all bundled libraries are available there.

This package also bundles Qt 6 (LGPL v3 / GPL), https://www.qt.io, as
dynamically linked DLLs. Qt source: https://download.qt.io/official_releases/qt/
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'README.txt')

# Sanity check: TLS must work from the staged copy, not just the build tree.
$exe = Join-Path $stage 'couchsync.exe'
$out = & $exe --check-tls
$rc = $LASTEXITCODE
$out | ForEach-Object { Write-Host "  $_" }
if ($rc -ne 0) { throw "couchsync.exe --check-tls failed (exit code $rc): TLS is not supported in the staged package" }

Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content -Encoding ascii (Join-Path $OutDir 'SHA256SUMS.txt')

$files = Get-ChildItem $stage -Recurse -File
Write-Host ("Staged {0} files ({1:N1} MB); zip {2:N1} MB; sha256 {3}" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), ((Get-Item $zip).Length / 1MB), $hash)
