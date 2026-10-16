@echo off
setlocal
cd /d "%~dp0"
echo Army of Two - join an online co-op game, with the lockstep sync trace
echo (sync_trace.log: what each frame simulated, to find where the two PCs part).
echo The host must run with the trace too. The game may run a little slower.
echo.
set "HOSTIP="
if exist "join_host.txt" set /p HOSTIP=<"join_host.txt"
set "NEWIP="
if not defined HOSTIP goto ask
set /p NEWIP=Host IP address [%HOSTIP%]:
goto got
:ask
set /p NEWIP=Host IP address, shown in the host's "Host co-op.bat" window:
:got
if defined NEWIP set "HOSTIP=%NEWIP%"
if not defined HOSTIP goto ask
>"join_host.txt" echo %HOSTIP%
echo.
echo Connecting to %HOSTIP%. When the host is in the lobby:
echo Campaign Mode - Public Co-op Game - any map and difficulty.
echo After a "Connection Lost" keep sync_trace.log and coop_debug.log for the host.
start "" armyoftworecomp.exe --ao2_coop_host=%HOSTIP% --ao2_coop_debug=true --ao2_coop_sync_trace=true --log_to_files=true
