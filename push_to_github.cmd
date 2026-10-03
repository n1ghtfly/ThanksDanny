@echo off
title Push Thanks Danny to GitHub
setlocal
rem Publishes this folder to https://github.com/n1ghtfly/ThanksDanny (public).
rem Safe to run again later: it commits whatever changed and pushes it.
rem What is left out on purpose is listed in .gitignore (flash images, third-party wallpapers).

cd /d "%~dp0"
set "REMOTE=https://github.com/n1ghtfly/ThanksDanny.git"

where git >nul 2>nul
if errorlevel 1 (
  echo.
  echo   Git is not installed. Get it from https://git-scm.com/download/win and run this again.
  echo.
  pause
  exit /b 1
)

if not exist ".git" (
  echo --- creating the local repository ---
  git init -b main
  if errorlevel 1 goto fail
  git remote add origin %REMOTE%
)

rem A commit needs a name and e-mail. If none is set on this PC, use the GitHub no-reply
rem address for this repository only, so no personal e-mail ends up in a public history.
git config user.name >nul 2>nul || git config user.name "n1ghtfly"
git config user.email >nul 2>nul || git config user.email "n1ghtfly@users.noreply.github.com"

rem The pictures site is its own repository (danny-pics-7f3c9a): link it as a submodule
rem instead of copying it in.
if exist "github-pics\.git" (
  git submodule status github-pics >nul 2>nul || git submodule add https://github.com/n1ghtfly/danny-pics-7f3c9a.git github-pics
)

echo.
echo --- getting any changes already on GitHub (e.g. pushed by Claude) ---
git pull --rebase --autostash origin main
if errorlevel 1 goto fail

echo.
echo --- committing ---
git add -A
git diff --cached --quiet && (echo   nothing new to commit) || git commit -q -m "Update from the PC"
if errorlevel 1 goto fail
git log --oneline -1

echo.
echo --- pushing to GitHub (a browser window may ask you to sign in the first time) ---
git push -u origin main
if errorlevel 1 goto fail

echo.
echo   Done: https://github.com/n1ghtfly/ThanksDanny
echo.
pause
exit /b 0

:fail
echo.
echo   Something failed - read the messages above this line.
echo   If GitHub refused the push, check you are signed in as n1ghtfly.
echo.
pause
exit /b 1
