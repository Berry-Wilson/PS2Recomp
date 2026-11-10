# ps2x-gs.dll: PCSX2's GS for the runtime

The runtime draws the game with PCSX2's GS, built as a DLL from a PCSX2 source tree. The runner
loads it from `pcsx2-gs\ps2x-gs.dll` next to `ps2EntryRunner.exe` (or `PS2X_PCSX2_GS_DLL`), with its
dependency DLLs beside it and PCSX2's `resources` folder under it.

| File | What it is |
| --- | --- |
| `Ps2xGS.cpp` | The DLL: PCSX2's GS runner (`pcsx2-gsrunner/Main.cpp`) with a C API instead of a GS dump player |
| `ps2x_gs_api.h` | The C API the runtime calls (`gs_pcsx2_backend.cpp`) |
| `CMakeLists.txt` | The `ps2x-gs` target, added to PCSX2's own build |
| `build-dependencies-gs.bat` | A trimmed copy of PCSX2's `build-dependencies.bat`: only what the GS needs |
| `build-ps2x-gs.bat` | Configures and builds the DLL, and copies it with its dependencies into the runner's folder |

## Building (Windows)

You need Visual Studio with the C++ tools and its Clang (clang-cl) component, and Git for Windows.
PCSX2 at commit `1ccd6a8dc` (4 November 2026) is known to work.

1. Clone PCSX2:
   ```
   git clone https://github.com/PCSX2/pcsx2.git C:\src\pcsx2
   ```
2. Add this to PCSX2's root `CMakeLists.txt`, after `add_subdirectory(pcsx2)`:
   ```cmake
   # PS2Recomp: PCSX2's GS as a DLL (ps2xRuntime/pcsx2-gs), when given its folder.
   if(PS2X_GS_DIR)
   	add_subdirectory("${PS2X_GS_DIR}" ps2x-gs)
   endif()
   ```
3. Build PCSX2's dependencies into `C:\src\pcsx2\deps`. The script finds PCSX2's patches next to it,
   so copy it there first:
   ```
   copy build-dependencies-gs.bat C:\src\pcsx2\.github\workflows\scripts\windows\
   C:\src\pcsx2\.github\workflows\scripts\windows\build-dependencies-gs.bat
   ```
4. Build the DLL and copy it into the runner's folder. PCSX2's configure step wants FFmpeg's headers
   and import libraries; the runtime's build has downloaded a copy to
   `out\build\ThirdParty\ffmpeg-prefix\src\ffmpeg_external`:
   ```
   build-ps2x-gs.bat C:\src\pcsx2 <PS2Recomp>\out\build\ThirdParty\ffmpeg-prefix\src\ffmpeg_external <PS2Recomp>\out\build\ps2xRuntime\Release\pcsx2-gs
   ```

After changing `Ps2xGS.cpp`, run step 4 again.
