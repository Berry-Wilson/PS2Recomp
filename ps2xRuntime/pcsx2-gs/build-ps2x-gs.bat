@echo off
setlocal enabledelayedexpansion
rem Builds ps2x-gs.dll inside a PCSX2 source tree.
rem   build-ps2x-gs.bat <pcsx2 source dir> <ffmpeg dir> [output dir]
rem With an output dir (the runner's pcsx2-gs folder, ps2xRuntime\Release\pcsx2-gs in the build), it
rem also copies there the DLL, the dependency DLLs it loads and PCSX2's resources. See README.md.
rem The PCSX2 tree needs its dependencies built first (build-dependencies-gs.bat, which leaves them in
rem <pcsx2>\deps) and the PS2X_GS_DIR hook in its root CMakeLists.txt. FFmpeg 7.1 is only needed to
rem satisfy PCSX2's configure step; the PS2Recomp build already has a copy.

set "PCSX2=%~1"
set "FFMPEG=%~2"
set "OUTDIR=%~3"
set "GSDIR=%~dp0"
set "GSDIR=%GSDIR:~0,-1%"
if "%PCSX2%"=="" goto usage
if "%FFMPEG%"=="" goto usage

for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VSINSTPATH=%%i"
call "!VSINSTPATH!\VC\Auxiliary\Build\vcvars64.bat" || goto error
set "PATH=!VSINSTPATH!\VC\Tools\Llvm\x64\bin;!VSINSTPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

rem PCSX2 reads FFmpeg's version from libavutil/ffversion.h, which in a build from git holds a commit
rem hash. A copy of the headers with the release version written in satisfies its check.
set "FFINC=%PCSX2%\deps-ffmpeg\include"
if exist "%FFINC%" rmdir /S /Q "%FFINC%"
xcopy "%FFMPEG%\include" "%FFINC%\" /E /I /Q /Y > nul || goto error
for /f "tokens=3" %%v in ('findstr /c:"define LIBAVCODEC_VERSION_MAJOR" "%FFMPEG%\include\libavcodec\version_major.h"') do set "AVCODEC_MAJOR=%%v"
set "FFVERSION=7.1"
if "!AVCODEC_MAJOR!"=="62" set "FFVERSION=8.0"
> "%FFINC%\libavutil\ffversion.h" (
  echo #ifndef AVUTIL_FFVERSION_H
  echo #define AVUTIL_FFVERSION_H
  echo #define FFMPEG_VERSION "!FFVERSION!"
  echo #endif
)
set "FFOPTS="
for %%c in (avcodec avformat avutil swresample swscale) do (
  set "FFOPTS=!FFOPTS! -DFFMPEG_%%c_INCLUDE_DIR=%FFINC% -DFFMPEG_%%c_LIBRARY=%FFMPEG%\bin\%%c.lib"
)

cd /d "%PCSX2%" || goto error
cmake -B build-gs -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_PREFIX_PATH="%PCSX2%\deps" ^
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DDISABLE_ADVANCE_SIMD=ON ^
  -DPS2X_GS_DIR="%GSDIR%" !FFOPTS! || goto error
cmake --build build-gs --target ps2x-gs || goto error
echo Built ps2x-gs.
if "%OUTDIR%"=="" exit /B 0

if not exist "%OUTDIR%" mkdir "%OUTDIR%" || goto error
copy /Y "build-gs\ps2x-gs\ps2x-gs.dll" "%OUTDIR%\" > nul || goto error
copy /Y "deps\bin\*.dll" "%OUTDIR%\" > nul || goto error
xcopy "bin\resources" "%OUTDIR%\resources\" /E /I /Q /Y > nul || goto error
echo Copied ps2x-gs.dll, its dependencies and resources to %OUTDIR%.
exit /B 0

:usage
echo usage: build-ps2x-gs.bat ^<pcsx2 source dir^> ^<ffmpeg dir^> [output dir]
exit /B 2

:error
echo Failed with error #%errorlevel%.
exit /B 1
