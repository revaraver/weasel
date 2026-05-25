@echo off
setlocal

set SRC=%~dp0output
set DST=C:\Program Files\Rime\weasel-0.17.4

echo ===== Weasel Dev Deploy =====
echo SRC: %SRC%
echo DST: %DST%
echo.

net session >nul 2>&1
if not %errorlevel% == 0 (
    echo [ERROR] Please run as Administrator!
    pause
    exit /b 1
)

if not exist "%DST%" (
    echo [ERROR] Install dir not found: %DST%
    pause
    exit /b 1
)

echo [1/4] Stopping WeaselServer...
taskkill /F /IM WeaselServer.exe >nul 2>&1
if %errorlevel% == 0 (
    echo       WeaselServer.exe stopped.
) else (
    echo       WeaselServer.exe not running, skip.
)

timeout /t 2 /nobreak >nul

echo [2/4] Copying files...

copy /y "%SRC%\WeaselServer.exe"   "%DST%\WeaselServer.exe"   >nul && echo       WeaselServer.exe   || echo [WARN] WeaselServer.exe failed
copy /y "%SRC%\WeaselDeployer.exe" "%DST%\WeaselDeployer.exe" >nul && echo       WeaselDeployer.exe || echo [WARN] WeaselDeployer.exe failed
copy /y "%SRC%\weasel.dll"         "%DST%\weasel.dll"         >nul && echo       weasel.dll         || echo [WARN] weasel.dll failed - logoff and retry
copy /y "%SRC%\weaselx64.dll"      "%DST%\weaselx64.dll"      >nul && echo       weaselx64.dll      || echo [WARN] weaselx64.dll failed - logoff and retry

if exist "%SRC%\Win32\weasel.dll" (
    if not exist "%DST%\Win32" mkdir "%DST%\Win32"
    copy /y "%SRC%\Win32\weasel.dll" "%DST%\Win32\weasel.dll" >nul && echo       Win32\weasel.dll || echo [WARN] Win32\weasel.dll failed
)

echo [3/4] Starting WeaselServer...
start "" "%DST%\WeaselServer.exe"
timeout /t 1 /nobreak >nul

echo [4/4] Done!
echo Note: if DLL copy failed, logoff Windows then run this script again before typing anything.
pause
exit /b 0
