@echo off
setlocal
set "AEGISUB_EXE=%~dp0build-local\aegisub-ready.exe"
if not exist "%AEGISUB_EXE%" goto missing
start "" "%AEGISUB_EXE%"
endlocal
exit /b 0
:missing
echo Aegisub exe not found:
echo %AEGISUB_EXE%
pause
