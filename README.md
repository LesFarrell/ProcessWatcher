# ProcessWatcher - Win32 C Program

A lightweight Windows GUI application written in C using the Win32 API to monitor specific processes.

![ProcessWatcher screenshot](screenshot/screenshot.jpg)

## Features

- Add/remove processes to watch by name or select from combo box dropdown
- Combo box shows all currently running processes for easy selection
- Allows typing process names directly for processes not running
- Real-time status display (RUNNING/STOPPED) with memory usage
- Shows PID (Process ID) and memory consumption in MB when running
- Color-coded display (Green = Running, Red = Stopped)
- Auto-refresh mode for continuous monitoring (1-second intervals)
- Case-insensitive process name matching
- Lightweight native Win32 executable

## Compilation

### Prerequisites

- **MinGW** (Minimalist GNU for Windows) - includes gcc compiler
- Download from: https://www.mingw-w64.org/

### Compile

Option 1: Use the batch script
```bash
compile.bat
```

Option 2: Manual compilation with gcc (GUI mode - no console window)
```bash
windres ProcessWatcher.rc -O coff -o ProcessWatcher_res.o
gcc -Wall -Wextra -o ProcessWatcher.exe ProcessWatcher.c ProcessWatcher_res.o -lkernel32 -luser32 -lgdi32 -lpsapi -lcomctl32 -mwindows
```

Option 3: With MSVC (if installed)
```bash
cl ProcessWatcher.c kernel32.lib user32.lib gdi32.lib psapi.lib comctl32.lib /W3 /Fe:ProcessWatcher.exe /SUBSYSTEM:WINDOWS
```

## Usage

Run the executable:
```bash
ProcessWatcher.exe
```

### How to Use

1. **Add Process**: 
   - **Option A**: Click the dropdown arrow in the combo box to see all running processes and select one
   - **Option B**: Type a process name directly (e.g., `notepad.exe`, `chrome.exe`) - useful for processes not currently running
   - Then click "Add Process"
2. **View Status**: The table shows all watched processes with columns:
   - **Process Name**: Name of the watched process
   - **Status**: RUNNING or STOPPED status
   - **PID**: Process ID (shown when running)
   - **Memory**: Memory usage in MB (shown when running)
3. **Refresh**: Click "Refresh" to manually check all process statuses and update memory readings
4. **Auto-Refresh**: Check "Auto-Refresh" to continuously monitor processes (updates every 1 second)
5. **Remove**: Select a process in the table and click "Remove Selected"

## Notes

- Process names are case-insensitive (matching is done using Windows API)
- Saved process names are normalized when loaded, so Windows CRLF line endings do not break matching
- The combo box dropdown shows all currently running processes for easy selection
- Process list in combo box updates automatically when the combo loses focus
- Memory usage shown is the working set size in MB for running processes
- The program uses Win32 API's `CreateToolhelp32Snapshot` for efficient process enumeration
- Memory metrics are retrieved using `GetProcessMemoryInfo`
- Auto-refresh uses the window timer/message loop, avoiding a background thread shutdown race
- The executable icon is embedded from `ProcessWatcher.ico` via `ProcessWatcher.rc`
- No external dependencies required once compiled
- Window size: 520x370 pixels

## Building with MSVC

If you have Visual Studio, you can compile with:
```bash
cl ProcessWatcher.c kernel32.lib user32.lib gdi32.lib psapi.lib comctl32.lib /W3 /Fe:ProcessWatcher.exe /SUBSYSTEM:WINDOWS
```

This will create a native Win32 executable that runs on any Windows system with no dependencies.
