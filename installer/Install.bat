@echo off
rem SlormAPI installer - double-click to install, update or remove the mods.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer.ps1" %*
