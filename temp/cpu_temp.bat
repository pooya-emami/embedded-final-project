@echo off
setlocal EnableDelayedExpansion

set "OUT=%~dp0cpu.txt"

:loop
set "RAW="

for /f "skip=1" %%A in ('wmic /namespace:\\root\OpenHardwareMonitor PATH Sensor WHERE "SensorType='Temperature' AND Name='CPU Package'" get Value') do (
    if not defined RAW if not "%%A"=="" set "RAW=%%A"
)

if defined RAW (
    >"!OUT!" echo !RAW!
    echo CPU Temperature: !RAW! C
)

timeout /t 1 /nobreak >nul
goto loop okay then please fix this for me and if you can somehow make libre in background and make it more lightweight even