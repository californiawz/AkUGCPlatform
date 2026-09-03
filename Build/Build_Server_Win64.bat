@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "TOOL_DIR=%SCRIPT_DIR%..\..\UnrealGameBuilerTool"

set "PY="
where py >nul 2>nul && set "PY=py -3"
if "%PY%"=="" where python3 >nul 2>nul && set "PY=python3"
if "%PY%"=="" set "PY=python"

%PY% "%TOOL_DIR%\BuilderEntry.py" build --config "%SCRIPT_DIR%BuildConfig.json" --profile win64-server %*
exit /b %ERRORLEVEL%
