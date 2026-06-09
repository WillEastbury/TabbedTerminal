@echo off
setlocal enabledelayedexpansion

:: Generate a unique token for this launcher instance
set "TOKEN=%RANDOM%%RANDOM%"
set "CMDFILE=%TEMP%\wt-launcher-cmd-%TOKEN%.txt"

:: Pass the token via environment so the launcher knows where to write
set "WT_LAUNCHER_CMDFILE=%CMDFILE%"

:: Path to TabbedTerminal's wtd.exe for opening special tabs
set "WTD=C:\source\terminal\src\cascadia\CascadiaPackage\bin\x64\Debug\AppX\wtd.exe"

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
set "LINENUM=0"
for /f "usebackq delims=" %%a in ("!CMDFILE!") do (
    set /a LINENUM+=1
    if !LINENUM!==1 (
        set "CMD=%%a"
    ) else (
        set "LINE=%%a"
        if "!LINE:~0,4!"=="CWD=" (
            set "CWD=!LINE:~4!"
        ) else if "!LINE:~0,4!"=="WEB:" (
            :: Web URL: open as WebView tab in TabbedTerminal
            start "" "!WTD!" -w 0 new-tab --commandline "%%a" 2>nul
        ) else if "!LINE:~0,9!"=="REPARENT:" (
            :: Win32 app: open as embedded tab in TabbedTerminal
            start "" "!WTD!" -w 0 new-tab --commandline "%%a" 2>nul
        ) else (
            :: Additional commands = launch directly as new console processes
            start "" cmd /k "%%a" 2>nul
        )
    )
)

:: Clean up the command file
del "!CMDFILE!" 2>nul

:: Change to working directory if specified
if defined CWD cd /d "!CWD!" 2>nul

:: Execute the primary command (handle special prefixes)
if defined CMD (
    if "!CMD:~0,4!"=="WEB:" (
        start "" "!WTD!" -w 0 new-tab --commandline "!CMD!" 2>nul
    ) else if "!CMD:~0,9!"=="REPARENT:" (
        start "" "!WTD!" -w 0 new-tab --commandline "!CMD!" 2>nul
    ) else (
        cmd /k "!CMD!"
    )
)
