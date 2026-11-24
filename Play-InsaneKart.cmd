@echo off
if not exist "%~dp0play-ikw\RetroRewind.exe" (
  echo Run Install-MKW100.cmd with Insane Kart Wii first: Install-MKW100.cmd -InsaneKart "D:\path\to\Insane Kart Wii"
  pause
  exit /b 1
)
cd /d "%~dp0play-ikw"
start "MKW100 Insane Kart Wii" RetroRewind.exe
