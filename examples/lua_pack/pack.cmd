@powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pack.ps1" %*
@if not "%~1"=="" exit /b
@pause
