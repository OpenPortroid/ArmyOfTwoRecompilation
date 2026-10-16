@echo off
rem Builds ArmyOfTwoLauncher.exe: one file, needs only .NET Framework 4.x
rem (built into Windows 10/11). Uses the Roslyn compiler from Visual Studio /
rem Build Tools if present, else the C# 5 compiler that ships with Windows.
setlocal
cd /d "%~dp0"

set "CSC="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find MSBuild\**\Roslyn\csc.exe 2^>nul`) do set "CSC=%%i"
if not defined CSC set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"

set "FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319"
if not exist bin mkdir bin

"%CSC%" -nologo -noconfig -nostdlib -target:winexe -platform:anycpu -optimize+ -utf8output ^
  -out:bin\ArmyOfTwoLauncher.exe -win32icon:res\app.ico ^
  -r:"%FW%\mscorlib.dll" -r:"%FW%\System.dll" -r:"%FW%\System.Core.dll" ^
  -r:"%FW%\System.Drawing.dll" -r:"%FW%\System.Windows.Forms.dll" ^
  -resource:res\bg.jpg,AO2Launcher.bg.jpg -resource:res\bg_dim.jpg,AO2Launcher.bg_dim.jpg ^
  -resource:res\logo.png,AO2Launcher.logo.png -resource:res\app.ico,AO2Launcher.app.ico ^
  src\*.cs
if errorlevel 1 exit /b 1
echo Built bin\ArmyOfTwoLauncher.exe
