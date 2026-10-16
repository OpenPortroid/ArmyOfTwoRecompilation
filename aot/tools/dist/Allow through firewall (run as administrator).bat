@echo off
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 goto noadmin
netsh advfirewall firewall delete rule name="Army of Two (recomp)" >nul 2>&1
netsh advfirewall firewall add rule name="Army of Two (recomp)" dir=in action=allow program="%~dp0armyoftworecomp.exe" enable=yes profile=any
netsh advfirewall firewall add rule name="Army of Two (recomp)" dir=in action=allow program="%~dp0ao2_lobby_server.exe" enable=yes profile=any
echo.
echo Done: the game and the lobby server may accept connections on all networks.
pause
exit /b 0
:noadmin
echo Run this file as administrator: right click - Run as administrator.
pause
exit /b 1
