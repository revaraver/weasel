@echo off
echo ===== Weasel Pipe Stress Test =====
echo Opens 10 notepads rapidly to simulate multi-process pipe contention.
echo Type Chinese in notepad between rounds to check for lag.
echo Press any key to start. Ctrl+C to stop.
pause

set COUNT=0

:loop
set /a COUNT=%COUNT%+1
echo [%COUNT%] Opening notepad...
start "" notepad.exe
timeout /t 1 /nobreak >nul

if %COUNT% GEQ 10 (
    echo.
    echo --- 10 notepads open. Switch to one and type Chinese. Check for lag. ---
    echo Press any key to kill all notepads and repeat...
    pause
    taskkill /F /IM notepad.exe >nul 2>&1
    set COUNT=0
)
goto loop
