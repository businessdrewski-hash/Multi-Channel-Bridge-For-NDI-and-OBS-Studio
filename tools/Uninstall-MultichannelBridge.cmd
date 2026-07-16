@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-MultichannelBridge.ps1"
set "ERR=%ERRORLEVEL%"
echo.
if not "%ERR%"=="0" (
  echo Uninstallation failed. Review the error above.
) else (
  echo Uninstallation completed successfully.
)
pause
exit /b %ERR%
