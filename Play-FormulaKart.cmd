@echo off
if not exist "%~dp0play-fkw\RetroRewind.exe" (
  echo Run Install-MKW100.cmd with Formula Kart Wii first: Install-MKW100.cmd -FormulaKart "D:\path\to\fkw"
  pause
  exit /b 1
)
cd /d "%~dp0play-fkw"
start "MKW100 Formula Kart Wii" RetroRewind.exe
