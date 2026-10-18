@echo off
rem Usage: build.cmd [mode | preset] [extra args]
rem   build.cmd                  release build (win-amd64-release), then rexruntime from sdk\
rem   build.cmd <preset> [args]  any preset from aot\CMakePresets.json, args go to cmake configure
rem   build.cmd sdk [args]       rexruntime from sdk\ (Release), copied next to the game
rem   build.cmd gpu [args]       rexgpu-xenos plugin from sdk\ (the port build overwrites it - rerun after)
rem   build.cmd pdb              release with debug symbols, then rexruntime from sdk\
rem   build.cmd compat           armyoftworecomp_compat.exe for CPUs without AVX2 (x86-64-v2, out\build\compat)
rem   build.cmd exp              experimental: guest memory without volatile (out\build\novol)
rem   build.cmd pgo-gen          instrumented build for profile-guided optimization (out\build\pgo-gen)
rem   build.cmd pgo-use          release optimized with pgo\ao2.profdata (out\build\pgo-use)
setlocal
set "ROOT=%~dp0"
set "ROOTF=%ROOT:\=/%"
set "GAME=%ROOT%aot\out\build\win-amd64-release"
call :vcvars || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%PATH%"
rem EXTRA = everything after the mode, verbatim (shift/%1 would split -DX=Y at "=").
set "MODE=%~1"
set "EXTRA="
if "%MODE%"=="" set "MODE=win-amd64-release" & goto dispatch
set "ALL=%*"
call set "EXTRA=%%ALL:*%~1=%%"

:dispatch
if /i "%MODE%"=="sdk" goto sdk
if /i "%MODE%"=="gpu" goto gpu
if /i "%MODE%"=="pdb" goto pdb
if /i "%MODE%"=="compat" goto compat
if /i "%MODE%"=="exp" goto exp
if /i "%MODE%"=="pgo-gen" goto pgo_gen
if /i "%MODE%"=="pgo-use" goto pgo_use

rem --- preset ---
cd /d "%ROOT%aot" || exit /b 1
cmake --preset %MODE% %EXTRA% || exit /b 1
cmake --build --preset %MODE% || exit /b 1
rem The build copies the stock rexruntime.dll from aot\rexglue next to the game;
rem it lacks net_title_server_ip (co-op: "no connection"). Put back the one
rem built from sdk\.
if /i "%MODE%"=="win-amd64-release" set "EXTRA=" & call :build_sdk || exit /b 1
exit /b 0

:sdk
call :build_sdk
exit /b %errorlevel%

:gpu
cd /d "%ROOT%sdk" || exit /b 1
cmake --build out\build\win-amd64 --config Release --target rexgpu-xenos %EXTRA% || exit /b 1
copy /y "%ROOT%sdk\out\win-amd64\Release\rexgpu-xenos.dll" "%GAME%\rexgpu-xenos.dll" && echo copied
exit /b %errorlevel%

:pdb
cd /d "%ROOT%aot" || exit /b 1
cmake --preset win-amd64-release "-DCMAKE_EXE_LINKER_FLAGS=-Wl,/debug" || exit /b 1
cmake --build --preset win-amd64-release || exit /b 1
copy /y "%ROOT%sdk\out\win-amd64\Release\rexruntime.dll" "%GAME%\rexruntime.dll" >nul
exit /b %errorlevel%

:compat
call :custom compat "-DAO2_CPU_ARCH=x86-64-v2" || exit /b 1
copy /y "%ROOT%aot\out\build\compat\armyoftworecomp.exe" "%GAME%\armyoftworecomp_compat.exe" && echo copied
exit /b %errorlevel%

:exp
call :custom novol "-DCMAKE_CXX_FLAGS=-fno-strict-aliasing" "-DCMAKE_EXE_LINKER_FLAGS=-Wl,/debug"
exit /b %errorlevel%

:pgo_gen
call :custom pgo-gen "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -DAO2_PGO_GENERATE" "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate"
exit /b %errorlevel%

:pgo_use
call :custom pgo-use "-DCMAKE_CXX_FLAGS=-fprofile-instr-use=%ROOTF%pgo/ao2.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin" "-DCMAKE_EXE_LINKER_FLAGS=-Wl,/debug"
exit /b %errorlevel%

rem :custom <dir> <cxx flags> <linker flags> - Release build of the game in
rem aot\out\build\<dir> with extra flags; takes rexruntime.dll from the main build.
:custom
cd /d "%ROOT%aot" || exit /b 1
cmake -S . -B out\build\%~1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=%ROOTF%aot/rexglue/win-amd64 %2 %3 || exit /b 1
cmake --build out\build\%~1 --target armyoftworecomp || exit /b 1
copy /y "%GAME%\rexruntime.dll" "%ROOT%aot\out\build\%~1\" >nul
exit /b %errorlevel%

:build_sdk
cd /d "%ROOT%sdk" || exit /b 1
if not exist out\build\win-amd64\build.ninja (
  cmake --preset win-amd64 -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build out\build\win-amd64 --config Release --target rexruntime %EXTRA% || exit /b 1
copy /y "%ROOT%sdk\out\win-amd64\Release\rexruntime.dll" "%GAME%\rexruntime.dll" && echo copied
exit /b %errorlevel%

rem Sets up the x64 MSVC environment from any Visual Studio 2022 edition
rem (BuildTools, Community, Professional, Enterprise).
:vcvars
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS for %%e in (BuildTools Community Professional Enterprise) do (
  if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  echo [ERROR] Visual Studio 2022 with C++ tools not found
  exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VCVARS%" >nul || exit /b 1
exit /b 0
