@echo off
setlocal enabledelayedexpansion
rem Trimmed copy of build-dependencies.bat for building only PCSX2's GS as a library (Drakengard PC
rem port): no Qt, KDDockWidgets, FFmpeg or video encoders (FFmpeg comes from the PS2Recomp build),
rem release libraries only, Windows' own tar instead of 7-Zip, and no pause on error.

echo Setting environment...
for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VSINSTPATH=%%i"
if not defined VSINSTPATH (
  echo Visual Studio not found.
  goto error
)
call "!VSINSTPATH!\VC\Auxiliary\Build\vcvars64.bat" || goto error
set "PATH=%PATH%;!VSINSTPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

set PATCH="C:\Program Files\Git\usr\bin\patch.exe"

pushd %~dp0
set "SCRIPTDIR=%CD%"
cd ..\..\..\..
mkdir deps-build
cd deps-build || goto error
set "BUILDDIR=%CD%"
cd ..
mkdir deps
cd deps || goto error
set "INSTALLDIR=%CD%"
popd

echo SCRIPTDIR=%SCRIPTDIR%
echo BUILDDIR=%BUILDDIR%
echo INSTALLDIR=%INSTALLDIR%

set "PATH=%PATH%;%INSTALLDIR%\bin"
cd "%BUILDDIR%"

set FREETYPE=2.14.3
set HARFBUZZ=14.2.0
set SDLMINOR=3.4.18
set SDL=SDL3-%SDLMINOR%
set LIBJPEGTURBO=3.2.0
set LIBPNG=1658
set LIBPNGLONG=1.6.58
set LZ4=1.10.0
set WEBP=1.6.0
set ZLIB=1.3.2
set ZLIBSHORT=132
set ZSTD=1.5.7
set PLUTOVG=1.3.3
set PLUTOSVG=0.0.8
set RAPIDYAML=0.12.1

set SHADERC=2026.2
set SHADERC_GLSLANG=275822a6261ee689aadb1da5f09a0ec2f058685c
set SHADERC_SPIRVHEADERS=58006c901d1d5c37dece6b6610e9af87fa951375
set SHADERC_SPIRVTOOLS=6337eb62cadd7d124ac6789bf39c0f71148f0a73

set AGILITYSDK=1.619.5
set DXHEADERS=1.619.5
set DXC=1.9.2609.5

call :downloadfile "freetype-%FREETYPE%.tar.gz" https://sourceforge.net/projects/freetype/files/freetype2/%FREETYPE%/freetype-%FREETYPE%.tar.gz/download e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1 || goto error
call :downloadfile "harfbuzz-%HARFBUZZ%.zip" https://github.com/harfbuzz/harfbuzz/archive/refs/tags/%HARFBUZZ%.zip bb2f83255706b1c92d731541c7cefaf98bb5b93e8f76d16f6deda05225ff20ee || goto error
call :downloadfile "lpng%LIBPNG%.zip" https://download.sourceforge.net/libpng/lpng1658.zip b32f170855dbbe3e6d9e645af40b538137041773672c3ba3e02db5816c82d376 || goto error
call :downloadfile "lpng%LIBPNG%-apng.patch.gz" https://download.sourceforge.net/libpng-apng/libpng-%LIBPNGLONG%-apng.patch.gz eee7dea22ed502868017971c86c63c4ed1e6085de0baebfdcc3d3322f00f3eb0 || goto error
call :downloadfile "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/%LIBJPEGTURBO%/libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e || goto error
call :downloadfile "libwebp-%WEBP%.tar.gz" "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-%WEBP%.tar.gz" e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564 || goto error
call :downloadfile "%SDL%.zip" "https://github.com/libsdl-org/SDL/releases/download/release-%SDLMINOR%/%SDL%.zip" 9cd42377704398796071b8597cd7e21da254a43bad98c4199739647adc13fa6f || goto error
call :downloadfile "lz4-%LZ4%.zip" "https://github.com/lz4/lz4/archive/refs/tags/v%LZ4%.zip" 3224b4c80f351f194984526ef396f6079bd6332dd9825c72ac0d7a37b3cdc565 || goto error
call :downloadfile "zlib%ZLIBSHORT%.zip" "https://github.com/madler/zlib/releases/download/v%ZLIB%/zlib%ZLIBSHORT%.zip" e8bf55f3017aa181690990cb58a994e77885da140609fc8f94abe9b65d2cae28 || goto error
call :downloadfile "zstd-%ZSTD%.zip" "https://github.com/facebook/zstd/archive/refs/tags/v%ZSTD%.zip" 7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4 || goto error
call :downloadfile "plutovg-%PLUTOVG%.zip" "https://github.com/sammycage/plutovg/archive/v%PLUTOVG%.zip" 030b656758a5d48bc82e931caba7e13f6b672345cebb9c2893c09223ac322ecf || goto error
call :downloadfile "plutosvg-%PLUTOSVG%.zip" "https://github.com/sammycage/plutosvg/archive/v%PLUTOSVG%.zip" bf2223c3ae69b2dfc8d69b238e26d2d877a1315a9e355671ea937a970f1cacd0 || goto error
call :downloadfile "agility-sdk-%AGILITYSDK%.nupkg" "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/%AGILITYSDK%" 0e9bcf32aac9a79343ede9b21e4864950ee54577e3d8e19bfcdf002bb4e9bfd6 || goto error
call :downloadfile "DirectX-Headers-%DXHEADERS%.zip" "https://github.com/microsoft/DirectX-Headers/archive/v%DXHEADERS%.zip" e839554c5c14e2fcce85ca99085ffa255626f054b44c2c10683f1062bc30401b || goto error
call :downloadfile "DirectXShaderCompiler-%DXC%.nupkg" "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DXC/%DXC%" 73cfa082245918c2cc1ec6740250eb7377e83a757b3b3a3e16e0d71535bfbfc3 || goto error
call :downloadfile "rapidyaml-%RAPIDYAML%-src.zip" "https://github.com/biojppm/rapidyaml/releases/download/v%RAPIDYAML%/rapidyaml-%RAPIDYAML%-src.zip" 96276f55b9fa7837ac8f3f72fd52965879cbb5d5d2e6af548c69a177fb078304 || goto error
call :downloadfile "shaderc-%SHADERC%.zip" "https://github.com/google/shaderc/archive/refs/tags/v%SHADERC%.zip" f9401cc5cb36c276cd1e072b6595dbd728148e8dba389e50f7339e2d388dbc08 || goto error
call :downloadfile "shaderc-glslang-%SHADERC_GLSLANG%.zip" "https://github.com/KhronosGroup/glslang/archive/%SHADERC_GLSLANG%.zip" 2b63189efad0348d88d410a5e12ec550a612e0b6ceef64624b8f45491269fb9c || goto error
call :downloadfile "shaderc-spirv-headers-%SHADERC_SPIRVHEADERS%.zip" "https://github.com/KhronosGroup/SPIRV-Headers/archive/%SHADERC_SPIRVHEADERS%.zip" d2f071e94c081f5a4606559770ebf1f7d1eac92a1def0c3e10609844aa8b69b2 || goto error
call :downloadfile "shaderc-spirv-tools-%SHADERC_SPIRVTOOLS%.zip" "https://github.com/KhronosGroup/SPIRV-Tools/archive/%SHADERC_SPIRVTOOLS%.zip" 4011be89aa73e3461c9deef73936a62c79a3097590c5135d058041cc9fb99c6f || goto error

echo Building release libraries...
set FORCEPDB=-DCMAKE_SHARED_LINKER_FLAGS_RELEASE="/DEBUG" -DCMAKE_MODULE_LINKER_FLAGS_RELEASE="/DEBUG"

echo Building Zlib...
rmdir /S /Q "zlib-%ZLIB%"
tar -xf "zlib%ZLIBSHORT%.zip" || goto error
cd "zlib-%ZLIB%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZLIB_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libpng...
rmdir /S /Q "lpng%LIBPNG%"
tar -xf "lpng%LIBPNG%.zip" || goto error
python -c "import gzip,shutil; shutil.copyfileobj(gzip.open('lpng%LIBPNG%-apng.patch.gz','rb'), open('libpng-%LIBPNGLONG%-apng.patch','wb'))" || goto error
cd "lpng%LIBPNG%" || goto error
%PATCH% -p1 < "../libpng-%LIBPNGLONG%-apng.patch" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libjpegturbo...
rmdir /S /Q "libjpeg-turbo-%LIBJPEGTURBO%"
tar -xf "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" || goto error
cd "libjpeg-turbo-%LIBJPEGTURBO%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building LZ4...
rmdir /S /Q "lz4"
tar -xf "lz4-%LZ4%.zip" || goto error
rename "lz4-%LZ4%" "lz4" || goto error
cd "lz4" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -DCMAKE_C_FLAGS="/wd4711 /wd5045" -B build-dir -G Ninja build/cmake || goto error
cmake --build build-dir --parallel || goto error
ninja -C build-dir install || goto error
cd ..

echo Building FreeType without HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building HarfBuzz...
rmdir /S /Q "harfbuzz-%HARFBUZZ%"
tar -xf "harfbuzz-%HARFBUZZ%.zip" --exclude "harfbuzz-%HARFBUZZ%/README" --exclude "harfbuzz-%HARFBUZZ%/CLAUDE.md" || goto error
cd "harfbuzz-%HARFBUZZ%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DHB_BUILD_UTILS=OFF -DHB_BUILD_GPU=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building FreeType with HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_REQUIRE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building Zstandard...
rmdir /S /Q "zstd-%ZSTD%"
tar -xf "zstd-%ZSTD%.zip" --exclude "zstd-%ZSTD%/tests/cli-tests/bin" || goto error
cd "zstd-%ZSTD%"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building WebP...
rmdir /S /Q "libwebp-%WEBP%"
tar -xf "libwebp-%WEBP%.tar.gz" || goto error
cd "libwebp-%WEBP%" || goto error
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building SDL...
rmdir /S /Q "%SDL%"
tar -xf "%SDL%.zip" || goto error
cd "%SDL%" || goto error
cmake -B build -DCMAKE_BUILD_TYPE=Release %FORCEPDB% -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_VIDEO=OFF -DSDL_POWER=OFF -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_TEST_LIBRARY=OFF -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
copy build\SDL3.pdb "%INSTALLDIR%\bin" || goto error
cd .. || goto error

echo "Building PlutoVG..."
rmdir /S /Q "plutovg-%PLUTOVG%"
tar -xf "plutovg-%PLUTOVG%.zip" || goto error
cd "plutovg-%PLUTOVG%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPLUTOVG_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo "Building PlutoSVG..."
rmdir /S /Q "plutosvg-%PLUTOSVG%"
tar -xf "plutosvg-%PLUTOSVG%.zip" || goto error
cd "plutosvg-%PLUTOSVG%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo "Building RapidYAML..."
rmdir /S /Q "rapidyaml-%RAPIDYAML%-src"
tar -xf "rapidyaml-%RAPIDYAML%-src.zip" --exclude "*/doc/img/*" || goto error
cd "rapidyaml-%RAPIDYAML%-src" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Unpacking Agility SDK
rmdir /S /Q "agility-sdk-%AGILITYSDK%"
mkdir "agility-sdk-%AGILITYSDK%" || goto error
tar -xf "agility-sdk-%AGILITYSDK%.nupkg" -C "agility-sdk-%AGILITYSDK%" || goto error
cd "agility-sdk-%AGILITYSDK%" || goto error
if not exist "%INSTALLDIR%\bin\D3D12" (
  mkdir "%INSTALLDIR%\bin\D3D12" || goto error
)
copy "build\native\bin\x64\D3D12Core.dll" "%INSTALLDIR%\bin\D3D12\D3D12Core.dll" || goto error
cd .. || goto error

echo Unpacking DirectX Headers
rmdir /S /Q "DirectX-Headers-%DXHEADERS%"
tar -xf "DirectX-Headers-%DXHEADERS%.zip" || goto error
cd "DirectX-Headers-%DXHEADERS%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DDXHEADERS_BUILD_TEST=OFF -DDXHEADERS_BUILD_GOOGLE_TEST=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Unpacking DirectX Shader Compiler
rmdir /S /Q "DirectXShaderCompiler-%DXC%"
mkdir "DirectXShaderCompiler-%DXC%" || goto error
tar -xf "DirectXShaderCompiler-%DXC%.nupkg" -C "DirectXShaderCompiler-%DXC%" || goto error
cd "DirectXShaderCompiler-%DXC%" || goto error
copy "build\native\lib\x64\dxcompiler.lib" "%INSTALLDIR%\lib\dxcompiler.lib" || goto error
copy "build\native\bin\x64\dxcompiler.dll" "%INSTALLDIR%\bin\dxcompiler.dll" || goto error
cd .. || goto error

echo Building shaderc...
rmdir /S /Q "shaderc-%SHADERC%"
tar -xf "shaderc-%SHADERC%.zip" || goto error
cd "shaderc-%SHADERC%" || goto error
cd third_party || goto error
tar -xf "..\..\shaderc-glslang-%SHADERC_GLSLANG%.zip" || goto error
rename "glslang-%SHADERC_GLSLANG%" "glslang" || goto error
tar -xf "..\..\shaderc-spirv-headers-%SHADERC_SPIRVHEADERS%.zip" || goto error
rename "SPIRV-Headers-%SHADERC_SPIRVHEADERS%" "spirv-headers" || goto error
tar -xf "..\..\shaderc-spirv-tools-%SHADERC_SPIRVTOOLS%.zip" || goto error
rename "SPIRV-Tools-%SHADERC_SPIRVTOOLS%" "spirv-tools" || goto error
cd .. || goto error
%PATCH% -p1 < "%SCRIPTDIR%\..\common\shaderc-changes.patch" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_SHARED_CRT=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Exiting with success.
exit /B 0

:error
echo Failed with error #%errorlevel%.
exit /B 1

:downloadfile
if not exist "%~1" (
  echo Downloading %~1 from %~2...
  curl -L -o "%~1" "%~2" || goto error
)
set idx=0
for /f %%F in ('certutil -hashfile "%~1" SHA256') do (
    set "out!idx!=%%F"
    set /a idx += 1
)
set filechecksum=%out1%
if /i %~3==%filechecksum% (
    echo Validated %~1.
    exit /B 0
) else (
    echo Expected %~3 got %filechecksum%.
    exit /B 1
)
