@echo off
setlocal
cd /d "%~dp0"
echo Army of Two - host an online co-op game
echo.
echo Addresses of this PC - tell your partner the one of your shared network
echo (LAN: 192.168.x.x / 10.x.x.x, Radmin VPN: 26.x.x.x); they enter it in "Join co-op.bat":
echo.
powershell -NoProfile -Command "Get-NetIPAddress -AddressFamily IPv4 -AddressState Preferred -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } | ForEach-Object { '    ' + $_.IPAddress + '    (' + $_.InterfaceAlias + ')' }"
echo.
echo In the game: Campaign Mode - Public Co-op Game - pick map and difficulty,
echo wait until the lobby opens, let your partner join, then Start Game.
echo.
start "" armyoftworecomp.exe --ao2_coop_server=true --ao2_coop_debug=true
pause
