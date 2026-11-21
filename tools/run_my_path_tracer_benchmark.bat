@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_my_path_tracer_benchmark.ps1" %*
exit /b %errorlevel%
