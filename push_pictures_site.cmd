@echo off
title Publish the pictures + rumours site
setlocal
rem Commits and pushes github-pics\ (the GitHub Pages site the badge downloads from:
rem https://n1ghtfly.github.io/danny-pics-7f3c9a/). GitHub takes a minute or two to publish.

cd /d "%~dp0github-pics"
where git >nul 2>nul || (echo   Git is not installed: https://git-scm.com/download/win & pause & exit /b 1)
git config user.name >nul 2>nul || git config user.name "n1ghtfly"
git config user.email >nul 2>nul || git config user.email "n1ghtfly@users.noreply.github.com"

git add -A
git diff --cached --quiet && (echo   nothing new to publish) || git commit -q -m "Update the badge pictures and rumours"
if errorlevel 1 goto fail
git push
if errorlevel 1 goto fail
echo.
echo   Published. Check: https://raw.githubusercontent.com/n1ghtfly/danny-pics-7f3c9a/main/list.txt
echo   (up to 5 minutes), then restart the badge: it fetches new pictures
echo   and rumours at start-up and deletes the ones you removed.
echo.
pause
exit /b 0
:fail
echo.
echo   Something failed - read the messages above this line.
echo.
pause
exit /b 1
