@echo off
title Flash Thanks Danny badge
setlocal

rem Builds Badge\Badge.ino and uploads it to the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
rem Only the app is written: the wallpaper and cached pictures in the LittleFS partition stay.

set "ACLI=C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set "FQBN=esp32:esp32:esp32s3:FlashSize=32M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=app5M_little24M_32MB,FlashMode=qio"
set "SKETCH=C:\Users\brain\Documents\Arduino\ThanksDanny\Badge"

echo ============================================
echo   Flash the Thanks Danny badge (v2)
echo ============================================
echo.
echo   Close the Arduino IDE Serial Monitor first - it holds the COM port.
echo.

echo Looking for the board (Espressif USB device)...
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "$d = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_303A' } | Select-Object -First 1; if ($d) { ($d.FriendlyName -replace '.*\((COM\d+)\).*','$1') }"`) do set "PORT=%%p"
if not defined PORT set "PORT=COM13"
echo   using %PORT%
echo.

echo --- building (a minute or two) ---
"%ACLI%" compile --clean --fqbn "%FQBN%" "%SKETCH%"
if errorlevel 1 goto fail

echo.
echo --- flashing ---
"%ACLI%" upload -p %PORT% --fqbn "%FQBN%" "%SKETCH%"
if errorlevel 1 goto fail

echo.
echo   Done. The badge restarts by itself and shows the home screen.
echo   If the screen shows noise or stripes, set LCD_QSPI_HZ to 40000000 in
echo   Badge.ino and run this again.
echo.
echo --- boot log (close this window when you have seen enough) ---
"%ACLI%" monitor -p %PORT% -c baudrate=115200
exit /b 0

:fail
echo.
echo   Something failed - read the messages above this line.
echo   Usual causes: the Serial Monitor is still open, or the board is unplugged.
echo   If upload keeps failing: hold BOOT, tap PWR, release BOOT, run this again.
echo.
pause
exit /b 1
