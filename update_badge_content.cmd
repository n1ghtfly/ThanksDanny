@echo off
title Update the badge's pictures and rumours
setlocal
rem One click to change what the badges download:
rem   pictures : put them in  Downloads\ThanksDanny\pics   (jpg / png, any size)
rem   rumours  : put them in  Downloads\ThanksDanny\mp3    ("[Speaker name] anything.mp3")
rem Delete a file there to take it off the badges. Then run this script: it converts everything
rem for the badge (baseline JPEGs, 16 kHz MP3s), rebuilds list.txt / rumours.txt and publishes
rem the site. Each badge picks the changes up at its next start-up.
rem Needs Python with Pillow (pip install pillow) and ffmpeg on PATH (for the rumours).

cd /d "%~dp0"
where python >nul 2>nul || (echo   Python is not installed: https://www.python.org/downloads/ & pause & exit /b 1)

echo.
echo === 1/3  Pictures (Downloads\ThanksDanny\pics)
python tools\prep_github_pics.py
if errorlevel 1 goto fail

echo.
echo === 2/3  Rumours (Downloads\ThanksDanny\mp3)
where ffmpeg >nul 2>nul || (echo   ffmpeg is not installed - skipping the rumours, pictures still go out & goto publish)
python tools\prep_rumours.py
if errorlevel 1 goto fail

:publish
echo.
echo === 3/3  Publish
call "%~dp0push_pictures_site.cmd"
exit /b 0

:fail
echo.
echo   Something failed - read the messages above this line. Nothing was published.
echo.
pause
exit /b 1
