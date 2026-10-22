@echo off
rem ---------------------------------------------------------------------------
rem  Jak and Daxter: The Precursor Legacy - Nintendo Switch port
rem  One-click data preparation.
rem
rem  Drag your ISO onto this file, or double-click it and paste the ISO path.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
title Jak 1 on Switch - data preparation
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare-sd.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
  echo Something went wrong. Copy the messages above if you need help.
) else (
  echo Done.
)
pause
exit /b %RC%
