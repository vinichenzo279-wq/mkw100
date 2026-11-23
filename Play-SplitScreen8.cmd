@echo off
if not exist "%~dp0play-ss8\RetroRewind.exe" (
  echo Run Install-MKW100.cmd with MKWii 8P Splitscreen first: Install-MKW100.cmd -SplitScreen8 "D:\path\to\mkwii-8p-splitscreen-1.2.0"
  pause
  exit /b 1
)
cd /d "%~dp0play-ss8"
start "MKW100 8 Player Splitscreen" RetroRewind.exe
