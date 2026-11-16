@echo off
setlocal
set "AEGISUB_EXE=%~dp0build-local\aegisub.exe"
if not exist "%AEGISUB_EXE%" set "AEGISUB_EXE=%~dp0aegisub.exe"
if not exist "%AEGISUB_EXE%" goto missing
start "" "%AEGISUB_EXE%"
endlocal
exit /b 0
:missing
echo Aegisub exe not found:
echo %AEGISUB_EXE%
pause
