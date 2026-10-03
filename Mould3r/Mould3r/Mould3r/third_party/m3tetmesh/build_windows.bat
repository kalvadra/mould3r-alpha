@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  Build m3tetmesh.dll - Mould3r's wrapper around fTetWild (3D tet meshing).
rem
rem  1. Open the "x64 Native Tools Command Prompt for VS" (cl, cmake, ninja and
rem     git on PATH) and run this script. The first run downloads fTetWild, its
rem     pinned dependencies (Eigen 3.4 among them) and the GMP 6.3.0 release
rem     (for mini-gmp - see CMakeLists.txt), and takes several minutes.
rem  2. Rebuild Mould3r: the project picks up out\lib\m3tetmesh.lib, and its
rem     post-build step copies out\bin\m3tetmesh.dll next to Mould3r.exe.
rem
rem  Output: out\bin\m3tetmesh.dll, out\lib\m3tetmesh.lib,
rem          out\include\m3tetmesh.h, out\licenses\<dependency>\
rem ===========================================================================

set "HERE=%~dp0"
set "BUILD_DIR=%HERE%build"
set "OUT_DIR=%HERE%out"

where cl >nul 2>nul || (echo [m3tetmesh] cl.exe not found - run from the "x64 Native Tools Command Prompt for VS". & exit /b 1)
where cmake >nul 2>nul || (echo [m3tetmesh] cmake not found on PATH. & exit /b 1)
where ninja >nul 2>nul || (echo [m3tetmesh] ninja not found on PATH ^(it ships with Visual Studio's CMake tools^). & exit /b 1)
where git >nul 2>nul || (echo [m3tetmesh] git not found on PATH. & exit /b 1)


rem Self-contained: no vcpkg paths, so nothing from Mould3r's tree leaks in.
cmake -S "%HERE%." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release || (echo [m3tetmesh] CMake configure failed. & exit /b 1)

cmake --build "%BUILD_DIR%" --target m3tetmesh || (echo [m3tetmesh] Build failed. & exit /b 1)

if not exist "%OUT_DIR%\bin" mkdir "%OUT_DIR%\bin"
if not exist "%OUT_DIR%\lib" mkdir "%OUT_DIR%\lib"
if not exist "%OUT_DIR%\include" mkdir "%OUT_DIR%\include"
copy /y "%BUILD_DIR%\m3tetmesh.dll" "%OUT_DIR%\bin\" >nul || exit /b 1
copy /y "%BUILD_DIR%\m3tetmesh.lib" "%OUT_DIR%\lib\" >nul || exit /b 1
copy /y "%HERE%m3tetmesh.h" "%OUT_DIR%\include\" >nul || exit /b 1
rem License texts of everything the build pulled in (for the installer's
rem licenses\ folder): fTetWild, its dependencies and GMP (mini-gmp).
for /d %%D in ("%BUILD_DIR%\_deps\*-src") do (
    if not exist "%OUT_DIR%\licenses\%%~nxD" mkdir "%OUT_DIR%\licenses\%%~nxD"
    for %%L in ("%%~fD\LICENSE*" "%%~fD\LICENCE*" "%%~fD\COPYING*") do copy /y "%%~fL" "%OUT_DIR%\licenses\%%~nxD\" >nul 2>nul
)

echo.
echo [m3tetmesh] Done. Contents of out\bin:
dir /b "%OUT_DIR%\bin"
echo Rebuild Mould3r to link the mesher and copy these DLLs next to Mould3r.exe.
endlocal
