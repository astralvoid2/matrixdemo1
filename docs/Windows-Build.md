# Native Windows build

The application builds as a native x64 Win32 executable. It retains OpenGL
3.3/GLSL, procedural synthesis, WAV playback/export, fullscreen, render scale,
vsync, shader reload, HUD and persisted sync trim. WSL is unnecessary.

## MSVC build

Install or repair Visual Studio 2022's **Desktop development with C++**
workload, with **MSVC v143** and a **Windows SDK**. Existing `cl.exe` files
alone are insufficient: the compiler headers/libraries, `vcvarsall.bat`,
NMake and SDK tools must also be present.

```powershell
.\Build-Windows.ps1 -Test
.\bin\matrixrain.exe
```

The script finds Visual Studio with `vswhere`, checks its environment setup
files, and loads the x64 toolchain. It finds CMake on PATH or in Visual Studio,
or downloads a verified portable copy into `.tools`. Release builds use the
static MSVC C runtime and statically link GLFW.

## Portable build when MSVC is unavailable

```powershell
.\Build-Windows.ps1 -PortableCompiler -Test
```

This uses Zig 0.14.1's bundled Clang and Windows C toolchain plus Ninja 1.12.1.
It emits native Windows executables, not Linux/WSL binaries. The tools live
under `.tools`, with their caches kept there too. No installer or PATH change
is required. The first download is approximately 125 MB across Zig, CMake,
Ninja and GLFW. Each download is pinned and SHA256-checked.

Both build paths produce:

| File | Purpose |
| --- | --- |
| `bin/matrixrain.exe` | GUI application; double-click to launch |
| `bin/matrixrain-console.exe` | Same demo, with console logging and CLI tools |

Use release executables for sharing. Debug builds include diagnostic symbols
and, for MSVC, the debug runtime. The Windows binaries depend on standard OS
DLLs and the installed graphics/audio drivers; there is no GLFW DLL to ship.

## Options and offline source

```powershell
.\Build-Windows.ps1 -Configuration Debug
.\Build-Windows.ps1 -CMakePath 'C:\Tools\CMake\bin\cmake.exe' -GlfwSourceDir 'C:\Sources\glfw-3.4' -Test
```

The offline checkout must be GLFW 3.4 with `deps/glad/gl.h` present. CMake's
normal download pins the GLFW 3.4 archive hash; supplying an external checkout
means you are responsible for that checkout's provenance.

From an **x64 Native Tools Command Prompt**, the equivalent MSVC commands are:

```bat
cmake -S . -B build\windows-manual -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build\windows-manual
ctest --test-dir build\windows-manual --output-on-failure
```

Visual Studio's own CMake support can also open `CMakeLists.txt`. The generated
shader header is regenerated whenever either GLSL source changes.

## Runtime differences

* Settings and GUI logs: `%LOCALAPPDATA%\MatrixRain`.
* Input and output file paths use UTF-8 converted to Windows Unicode paths.
* WASAPI plays 16-bit stereo with Windows resampling to the output's native
  format. The worker owns all COM objects and publishes a protected device
  clock snapshot. Mute advances playback while submitting silence.
* Windows visuals follow the device clock minus user trim. The endpoint
  queue shown in the HUD excludes unreported downstream/display latency.
* Changing the default playback device requires restarting the demo. A lost
  endpoint is reported and the renderer falls back to the wall clock.
* Windows memory counters are working set and private commit; Linux's
  anonymous/file/shared resident breakdown is kept on Linux.
* Select the executable's GPU preference in Windows Graphics settings.
  The Linux PRIME/`execv` relaunch remains guarded behind the Linux build.
* Hot reload searches `src/` under the current directory, then the current
  directory itself. The compiled shaders still work when the EXE is moved.

## Verification

`-Test` runs CTest checks for CLI help, music export, WAV decoding/synthesis,
the x64 GUI/console executable subsystems and embedded manifests, and a
muted live WASAPI clock check. The clock check crosses the 15-second
music loop boundary, compares device-clock progression with elapsed time,
and checks teardown/restart. It reports **skipped** if no usable playback
endpoint is available; a skip is not proof of working audio.

The WAV checks cover mono/stereo PCM and floating point, extensible WAV
headers, truncated data, partial frames, and procedural export/reload.

To check the actual OpenGL pipeline on a machine with a display/driver:

```powershell
.\bin\matrixrain-console.exe --smoke-test --hud
.\bin\matrixrain.exe --smoke-test --hud
```

This creates a hidden window, compiles the embedded shaders, renders both
passes, reads back RGB output, checks GL errors and exits. To include it in
CTest, configure with `-DMATRIXRAIN_TEST_RENDERING=ON`. Run manually as well
to judge audio/visual alignment, fullscreen/HiDPI behavior and shader reload:

```powershell
.\bin\matrixrain-console.exe --hud
.\bin\matrixrain-console.exe --nosound --hud
.\bin\matrixrain-console.exe --export .\track.wav
.\bin\matrixrain-console.exe --wav .\track.wav --bpm 128 --hud
```

## Linux compatibility

The original `make` build still uses system GLFW/OpenGL and ALSA. Its source
list explicitly excludes Windows-only files. The shared WAV/synthesis layer
and platform helpers are included in both builds.

The CMake build also supports Linux with the packages listed in the README:

```sh
cmake -S . -B build/cmake-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake-linux
ctest --test-dir build/cmake-linux --output-on-failure
./bin/matrixrain
```

CMake builds GLFW 3.4 from its pinned source. Install X11/Wayland development
packages required by that GLFW build, in addition to ALSA development headers.
