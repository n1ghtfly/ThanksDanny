@echo off
title Flash the badge filesystem (wallpaper)
setlocal

rem Builds the LittleFS image from data\ (the wallpaper) with tools\make_fs_image.py and writes it to
rem the filesystem partition at 0x910000. Needed ONCE per badge (a new board, or after a full erase);
rem tools\flash_badge.cmd only writes the app and leaves this partition alone.
rem It also wipes the cached pictures and rumours - the badge downloads them again when it is online.

set "ROOT=%~dp0.."
set "ESPTOOL="
for /d %%d in ("%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\*") do if exist "%%d\esptool.exe" set "ESPTOOL=%%d\esptool.exe"
if not defined ESPTOOL (
  echo   esptool.exe not found under %LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py
  goto fail
)

echo ============================================
echo   Flash the badge filesystem (wallpaper)
echo ============================================
echo.
echo   Close the Serial Monitor / flash_badge window first - it holds the COM port.
echo.

echo Looking for the board (Espressif USB device)...
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "$d = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_303A' } | Select-Object -First 1; if ($d) { ($d.FriendlyName -replace '.*\((COM\d+)\).*','$1') }"`) do set "PORT=%%p"
if not defined PORT set "PORT=COM13"
echo   using %PORT%
echo.

echo --- building the image ---
python "%ROOT%\tools\make_fs_image.py"
if errorlevel 1 goto fail

echo.
echo --- writing it (about 3 minutes) ---
"%ESPTOOL%" -p %PORT% -c auto write-flash 0x910000 "%ROOT%\build\wallpaper.littlefs.bin"
if errorlevel 1 goto fail

echo.
echo   Done. The badge restarts with its wallpaper; pictures and rumours download again once it is online.
echo.
pause
exit /b 0

:fail
echo.
echo   Something failed - read the messages above this line.
echo   If writing keeps failing: hold BOOT, tap PWR, release BOOT, run this again.
echo.
pause
exit /b 1
