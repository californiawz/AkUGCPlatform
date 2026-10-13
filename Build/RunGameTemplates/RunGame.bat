@echo off
setlocal
set "RUN_DIR=%~dp0"
set "CLIENT_EXE=%RUN_DIR%__CLIENT_EXE__.exe"
if not exist "%CLIENT_EXE%" set "CLIENT_EXE=%RUN_DIR%__PROJECT__\Binaries\Win64\__CLIENT_EXE__.exe"
if not exist "%CLIENT_EXE%" set "CLIENT_EXE=%RUN_DIR%__CLIENT_FOLDER__\__CLIENT_EXE__.exe"
if not exist "%CLIENT_EXE%" set "CLIENT_EXE=%RUN_DIR%__CLIENT_FOLDER__\__PROJECT__\Binaries\Win64\__CLIENT_EXE__.exe"
if not exist "%CLIENT_EXE%" (
    echo [RunGame] Client executable not found under "%RUN_DIR%".
    exit /b 1
)
echo [RunGame] Client: "%CLIENT_EXE%"
if /I "%~1"=="--check" exit /b 0
start "" "%CLIENT_EXE%" -windowed -ResX=1280 -ResY=720 %*
exit /b 0
