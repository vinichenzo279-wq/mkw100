@echo off
if not exist "%~dp0play-if\RetroRewind.exe" (
  echo Run Install-MKW100.cmd with Item Fever first: Install-MKW100.cmd -ItemFever "D:\path\to\item-fever"
  pause
  exit /b 1
)
cd /d "%~dp0play-if"
start "MKW100 Item Fever" RetroRewind.exe
