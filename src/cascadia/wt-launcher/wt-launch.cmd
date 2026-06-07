@echo off
setlocal enabledelayedexpansion

:: Generate a unique token for this launcher instance
set "TOKEN=%RANDOM%%RANDOM%"
set "CMDFILE=%TEMP%\wt-launcher-cmd-%TOKEN%.txt"

:: Pass the token via environment so the launcher knows where to write
set "WT_LAUNCHER_CMDFILE=%CMDFILE%"

:: Detect which Terminal exe we're running inside
:: Use the AppX debug build if available, otherwise fall back to wt.exe
set "WT_EXE=wt.exe"
if exist "C:\source\terminal\src\cascadia\CascadiaPackage\bin\x64\Debug\AppX\wtd.exe" (
    set "WT_EXE=C:\source\terminal\src\cascadia\CascadiaPackage\bin\x64\Debug\AppX\wtd.exe"
)

:: Run the TUI launcher
"%~dp0wt-launcher.exe"
set LAUNCHER_EXIT=%ERRORLEVEL%

:: If exit code is 42 (relaunch), read the command file and execute
if "%LAUNCHER_EXIT%"=="42" (
    if not exist "!CMDFILE!" (
        echo Launcher exited with relaunch code but no command file found.
        exit /b 1
    )

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
                :: Additional commands = open as new tabs in the SAME Terminal
                "!WT_EXE!" new-tab -- %%a
            )
        )
    )

    :: Clean up the command file
    del "!CMDFILE!" 2>nul

    :: Change to working directory if specified
    if defined CWD (
        cd /d "!CWD!"
    )

    :: Execute the primary command (replaces this shell process)
    if defined CMD (
        !CMD!
    )
) else (
    exit /b %LAUNCHER_EXIT%
)
