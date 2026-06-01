@echo off
REM Compile with MinGW
REM Usage: compile.bat

if not exist "ProcessWatcher.c" (
    echo Error: ProcessWatcher.c not found!
    exit /b 1
)

REM Check if MinGW gcc is available
where gcc >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Error: gcc not found. Please install MinGW and add it to PATH.
    exit /b 1
)

echo Compiling ProcessWatcher.c as Windows GUI application...
windres ProcessWatcher.rc -O coff -o ProcessWatcher_res.o
if %ERRORLEVEL% NEQ 0 (
    echo Resource compilation failed!
    exit /b 1
)

gcc -Wall -Wextra -o ProcessWatcher.exe ProcessWatcher.c ProcessWatcher_res.o -lkernel32 -luser32 -lgdi32 -lpsapi -lcomctl32 -mwindows

if %ERRORLEVEL% EQU 0 (
    echo Compilation successful!
    echo Run: ProcessWatcher.exe
) else (
    echo Compilation failed!
    exit /b 1
)
