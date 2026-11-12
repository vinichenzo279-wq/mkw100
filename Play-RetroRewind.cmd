@echo off
if not exist "%~dp0play-rr\RetroRewind.exe" (
  echo Run Install-MKW100.cmd with Retro Rewind first: Install-MKW100.cmd -RetroRewind auto
  pause
  exit /b 1
)
cd /d "%~dp0play-rr"
start "MKW100 Retro Rewind" RetroRewind.exe
