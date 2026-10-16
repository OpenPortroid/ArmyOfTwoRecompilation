@echo off
cd /d "%~dp0"

echo === git pull ===
git pull
if errorlevel 1 (
    echo git pull failed
    pause
    exit /b 1
)

echo.
echo === build.cmd ===
call .\build.cmd
if errorlevel 1 (
    echo build.cmd failed
    pause
    exit /b 1
)

echo.
echo === Join co-op.bat ===
call ".\aot\out\build\win-amd64-release\Join co-op.bat"

echo.
echo Done.
pause