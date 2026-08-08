@echo off
setlocal EnableDelayedExpansion

set "OUT=%~dp0cpu.txt"

:loop
set "RAW="

for /f "skip=1" %%A in ('wmic /namespace:\\root\wmi PATH MSAcpi_ThermalZoneTemperature get CurrentTemperature') do (
    if not defined RAW if not "%%A"=="" set "RAW=%%A"
)

if defined RAW (
    set /a TEMP10=RAW-2732
    set /a WHOLE=TEMP10/10
    set /a DECIMAL=TEMP10%%10
    >"!OUT!" echo !WHOLE!.!DECIMAL!
)

timeout /t 1 /nobreak >nul
goto loop
