# CouchSync client (M0: libmpv in Qt Quick)

## Windows build

1. Install Visual Studio 2022 Build Tools with the "Desktop development with C++" workload, plus CMake (>= 3.21) and 7-Zip.
2. Install Qt 6.8 (online installer) with the **MSVC 2022 64-bit** kit.
3. Open an "x64 Native Tools Command Prompt for VS 2022" (or a VS Developer PowerShell with x64) and fetch libmpv:
   ```
   powershell -ExecutionPolicy Bypass -File client\tools\fetch-mpv.ps1
   ```
   This downloads a pinned shinchiro build into `client/third_party/mpv` (gitignored) and generates `mpv.lib`.
4. Configure and build (from `client/`):
   ```
   cmake --preset windows-msvc -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
   cmake --build --preset windows-msvc-release
   ```
   The post-build step copies `libmpv-2.dll` and runs `windeployqt` (disable with `-DCOUCHSYNC_DEPLOY=OFF`).
5. Run:
   ```
   build\windows-msvc\Release\couchsync.exe path\to\file.mkv
   ```

Space toggles pause. The console shows mpv logs and a `hwdec-current:` line so you can verify hardware decoding.

Linux/macOS: install libmpv development files (pkg-config `mpv`) and Qt 6.5+, then configure with plain CMake.
