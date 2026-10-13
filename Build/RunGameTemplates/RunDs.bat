@echo off
setlocal
set "RUN_DIR=%~dp0"
set "SERVER_EXE=%RUN_DIR%__PROJECT__\Binaries\Win64\__SERVER_EXE__.exe"
if not exist "%SERVER_EXE%" set "SERVER_EXE=%RUN_DIR%__SERVER_EXE__.exe"
if not exist "%SERVER_EXE%" set "SERVER_EXE=%RUN_DIR%__SERVER_FOLDER__\__PROJECT__\Binaries\Win64\__SERVER_EXE__.exe"
if not exist "%SERVER_EXE%" set "SERVER_EXE=%RUN_DIR%__SERVER_FOLDER__\__SERVER_EXE__.exe"
if not exist "%SERVER_EXE%" (
    echo [RunDs] Server executable not found under "%RUN_DIR%".
    exit /b 1
)
if not defined DS_PORT set "DS_PORT=17777"
if not defined DS_MAP set "DS_MAP=/Game/Maps/Phase0"
echo [RunDs] Server: "%SERVER_EXE%" port=%DS_PORT% map=%DS_MAP%
if /I "%~1"=="--check" exit /b 0
start "" "%SERVER_EXE%" %DS_MAP% -server -log -port=%DS_PORT% %*
exit /b 0
