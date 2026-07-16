@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-MultichannelBridge.ps1" -CleanLegacyPluginManager
set "ERR=%ERRORLEVEL%"
echo.
if not "%ERR%"=="0" (
  echo Installation failed. Review the error above.
) else (
  echo Installation completed successfully.
)
pause
exit /b %ERR%
