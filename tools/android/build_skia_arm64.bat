@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_skia_arm64.ps1" %*
exit /b %ERRORLEVEL%
