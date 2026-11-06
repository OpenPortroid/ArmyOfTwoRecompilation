@echo off
rem Builds and runs what needs no game files (tools\ci\CMakeLists.txt). Needs sdk\: sdk_changes\apply.ps1.
rem Everything goes to out\ci, nothing of a normal build is touched.
rem   tools\ci\cpp_tests.cmd [parallel jobs]
setlocal
set "ROOT=%~dp0..\.."
for %%i in ("%ROOT%") do set "ROOT=%%~fi"
set "ROOTF=%ROOT:\=/%"
set "BUILD=%ROOT%\out\ci"
set "BUILDF=%ROOTF%/out/ci"
set "PARALLEL="
if not "%~1"=="" set "PARALLEL=-j %~1"
set "VCVARS="
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`"%VSINSTALLER%\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
  echo [ERROR] Visual Studio with C++ tools not found
  exit /b 1
)
set "PATH=%VSINSTALLER%;%PATH%"
call "%VCVARS%" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
clang++ --version || exit /b 1
set "CLANG=-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"

rem The SDK with the port's changes and the flags of its win-amd64 preset. Its outputs are redirected: by default they
rem land in sdk\out.
set "OUT=%BUILDF%/sdk-out"
cmake -S "%ROOT%\sdk" -B "%BUILD%\sdk" %CLANG% -DCMAKE_CXX_STANDARD=23 -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 "-DCMAKE_INSTALL_PREFIX=%BUILDF%/sdk-install" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=%OUT%" "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE=%OUT%" "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE=%OUT%" || exit /b 1
cmake --build "%BUILD%\sdk" %PARALLEL% || exit /b 1
cmake --install "%BUILD%\sdk" || exit /b 1

rem The checks.
cmake -S "%~dp0." -B "%BUILD%\tests" %CLANG% "-DCMAKE_PREFIX_PATH=%BUILDF%/sdk-install" || exit /b 1
cmake --build "%BUILD%\tests" --target ci_all %PARALLEL% || exit /b 1
copy /y "%BUILD%\sdk-out\rexruntime.dll" "%BUILD%\tests\bin\" >nul || exit /b 1

"%BUILD%\tests\bin\invite_checks_test.exe" || exit /b 1
"%BUILD%\tests\bin\texture_loader_test.exe" || exit /b 1
rem Fails on a shader the translator cannot compile; tools\shaders holds the game's own microcode.
"%BUILD%\tests\bin\xenos_hlsl_test.exe" "%ROOT%\tools\shaders" "%BUILD%\shaders_failed" --no-cache || exit /b 1
if not exist "%BUILD%\tests\bin\ao2_lobby_server.exe" exit /b 1
echo C++ checks passed
