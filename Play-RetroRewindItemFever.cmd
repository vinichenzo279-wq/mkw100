@echo off
rem Experimental: Retro Rewind with Item Fever's items (docs\mkw100\rr-item-fever.md). Offline only.
if not exist "%~dp0play-rr-if\RetroRewind.exe" (
  echo Run Install-MKW100.cmd with Retro Rewind and Item Fever's items first:
  echo   Install-MKW100.cmd -RetroRewind auto -ItemFeverItems "D:\path\to\item-fever"
  pause
  exit /b 1
)
cd /d "%~dp0play-rr-if"
start "MKW100 Retro Rewind + Item Fever items" RetroRewind.exe
