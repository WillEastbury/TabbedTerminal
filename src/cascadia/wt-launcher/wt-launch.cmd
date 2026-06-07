@echo off
setlocal enabledelayedexpansion

:: Generate a unique token for this launcher instance
set "TOKEN=%RANDOM%%RANDOM%"
set "CMDFILE=%TEMP%\wt-launcher-cmd-%TOKEN%.txt"

:: Pass the token via environment so the launcher knows where to write
set "WT_LAUNCHER_CMDFILE=%CMDFILE%"

:: Run the TUI launcher
"%~dp0wt-launcher.exe" 2>nul
set LAUNCHER_EXIT=%ERRORLEVEL%

:: If exit code is not 42, just exit cleanly
if not "%LAUNCHER_EXIT%"=="42" exit /b 0

:: Check command file exists
if not exist "!CMDFILE!" exit /b 0

:: Read lines: first = primary command, CWD= prefix = working dir, others = multi-launch
set "CMD="
set "CWD="
for /f "usebackq delims=" %%a in ("!CMDFILE!") do (
    if not defined CMD (
        set "CMD=%%a"
    ) else (
        set "LINE=%%a"
        if "!LINE:~0,4!"=="CWD=" (
            set "CWD=!LINE:~4!"
        ) else (
            :: Additional commands = launch directly as new console processes
            start "" "%%a" 2>nul
        )
    )
)

:: Clean up the command file
del "!CMDFILE!" 2>nul

:: Change to working directory if specified
if defined CWD cd /d "!CWD!" 2>nul

:: Execute the primary command (replaces this shell process)
if defined CMD !CMD!
