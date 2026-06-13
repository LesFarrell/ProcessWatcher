#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <wchar.h>
#include <winternl.h>

#define IDC_PROCESS_NAME 1001
#define IDC_ADD_BUTTON 1002
#define IDC_REMOVE_BUTTON 1003
#define IDC_LISTBOX 1004
#define IDC_STATUS_BAR 1005
#define IDC_EVENT_LOG_EDIT 1006
#define IDC_PROCESS_BROWSER_LIST 1007
#define IDC_PICK_WINDOW_BUTTON 1008
#define IDC_PROCESS_BROWSER_BUTTON 1009
#define MAX_PROCESSES 100
#define COLUMN_COUNT 6
#define ID_REFRESH_TIMER 1
#define ID_COMBO_FILTER_TIMER 2
#define ID_WINDOW_PICKER_TIMER 3
#define COMBO_FILTER_DELAY_MS 300
#define WINDOW_PICKER_TIMER_MS 75
#define ID_CONTEXT_REMOVE_PROCESS 40001
#define ID_CONTEXT_END_PROCESS 40002
#define ID_CONTEXT_TOGGLE_TOPMOST 40003
#define ID_CONTEXT_OPEN_TASK_MANAGER 40004
#define ID_CONTEXT_OPEN_FILE_LOCATION 40005
#define ID_CONTEXT_START_PROCESS 40011
#define ID_OPTIONS_AUTO_REFRESH 40006
#define ID_OPTIONS_START_WITH_WINDOWS 40007
#define ID_FORCE_REFRESH 40008
#define ID_FILE_EXIT 40009
#define ID_OPTIONS_NOTIFY_ON_STOP 40010
#define ID_OPTIONS_CREATE_STOP_LOGS 40012
#define ID_FILE_OPEN_LOG 40013
#define ID_FILE_OPEN_LOG_FOLDER 40014
#define ID_FILE_OPEN_EVENT_LOG_WINDOW 40015
#define ID_FILE_LAUNCH_NEW_PROCESS 40017
#define ID_HELP_WATCH_SYNTAX 40018
#define ID_FILE_PROCESS_BROWSER 40019
#define IDI_APP_ICON 101
#define NOTIFICATION_ICON_ID 1
#define SETTINGS_FILE_NAME "ProcessWatcher.ini"
#define RUN_REGISTRY_PATH "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_REGISTRY_VALUE "ProcessWatcher"
#define DEFAULT_REFRESH_INTERVAL_MS 1000
#define DEFAULT_STOP_LOG_MAX_BYTES (10ULL * 1024ULL * 1024ULL)
#define DEFAULT_STOP_LOG_MAX_ROTATIONS 5
#define EVENT_LOG_VIEW_TAIL_BYTES (64 * 1024)
#define EVENT_LOG_COLUMN_COUNT 9
#define PROCESS_BROWSER_COLUMN_COUNT 3
#define LOG_FILE_HEADER "Timestamp,Event,Target,PID,CPU,Memory,Path,Command Line,Details\r\n"

#define WATCH_MODE_PROCESS_NAME 0
#define WATCH_MODE_COMMAND_LINE 1

typedef struct
{
    char name[256];
    int watchMode;
    char matchPattern[1024];
    DWORD pid;
    BOOL running;
    DWORD memoryMB;
    double cpuPercent;
    ULONGLONG lastCpuTime;
    ULONGLONG lastSampleTime;
    DWORD lastSamplePid;
    SYSTEMTIME lastSeenLocalTime;
    BOOL hasLastSeen;
    char executablePath[MAX_PATH];
    char commandLine[4096];
    HANDLE trackedProcessHandle;
    DWORD trackedProcessPid;
    DWORD lastStopExitCode;
    BOOL hasLastStopExitCode;
    char lastStopReason[160];
    char lastStopDetails[512];
} WatchedProcess;

typedef struct
{
    WatchedProcess processes[MAX_PROCESSES];
    int count;
    HWND hwndProcessCombo;
    HWND hwndAddButton;
    HWND hwndPickWindowButton;
    HWND hwndProcessBrowserButton;
    HWND hwndRemoveButton;
    HWND hwndListView;
    HWND hwndStatusBar;
    HWND hwndEventLogWindow;
    HWND hwndEventLogEdit;
    HWND hwndProcessBrowserWindow;
    HWND hwndProcessBrowserList;
    BOOL bUpdatingComboText;
    BOOL autoRefreshEnabled;
    BOOL startWithWindows;
    BOOL notifyOnStop;
    BOOL createStopLogs;
    BOOL notificationIconAdded;
    SYSTEMTIME lastRefreshLocalTime;
    BOOL hasLastRefresh;
    int sortColumn;
    BOOL sortAscending;
    int logSortColumn;
    BOOL logSortAscending;
    int processBrowserSortColumn;
    BOOL processBrowserSortAscending;
    BOOL windowPickerActive;
    BOOL windowPickerAwaitingButtonRelease;
    DWORD windowPickerPid;
    char windowPickerProcessName[256];
    RECT savedWindowRect;
    BOOL hasSavedWindowRect;
    int savedColumnWidths[COLUMN_COUNT];
} AppData;

typedef struct
{
    char name[256];
    DWORD pid;
    int matchRank;
} ComboProcessEntry;

typedef struct
{
    DWORD pid;
    char processName[256];
    char commandLine[4096];
} ProcessBrowserEntry;

AppData g_AppData = {0};

LRESULT CALLBACK ComboOpenOnClickProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK EventLogWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
int CALLBACK CompareEventLogItems(LPARAM lParamItem1, LPARAM lParamItem2, LPARAM lParamSort);
void UpdateEventLogSortIndicators(void);
int FindListViewItemByParam(HWND hwndListView, LPARAM itemParam);
int CompareEventLogColumnValues(int columnIndex, const char *left, const char *right);
int CompareProcessBrowserEntries(const void *left, const void *right);
void UpdateProcessBrowserSortIndicators(void);
BOOL IsWindowTopmost(HWND hwnd);
void ToggleWatcherTopmost(HWND hwndOwner, BOOL makeTopmost);
BOOL SaveSettingsToIni(HWND hwndOwner);
void LoadSettingsFromIni(HWND hwndOwner);
void ShowSettingsSaveError(HWND hwndOwner);
void SaveSettingsWithFeedback(HWND hwndOwner);
BOOL GetProcessExecutablePathByPid(DWORD pid, char *processPath, size_t processPathSize);
DWORD GetProcessMemoryMB(DWORD pid);
void UpdateStatusBar(void);
void ToggleWindowPicker(HWND hwndOwner);
void ApplyAutoRefreshTimer(HWND hwnd);
BOOL SetStartWithWindowsEnabled(BOOL enabled);
BOOL IsStartWithWindowsEnabled(void);
void ApplySavedWindowPlacement(HWND hwnd);
HMENU CreateMainWindowMenu(void);
void UpdateOptionsMenuState(HWND hwnd);
void UpdateRemoveButtonState(void);
void UpdateListSortIndicators(void);
BOOL EnsureNotificationIcon(HWND hwnd);
void RemoveNotificationIcon(HWND hwnd);
void ShowStopNotification(HWND hwnd, const char *message);
void AddProcess(const char *processName);
BOOL GetKnownProcessExecutablePath(const WatchedProcess *process, char *processPath, size_t processPathSize);
void StartSelectedProcess(HWND hwndOwner);
void LaunchNewProcess(HWND hwndOwner);
BOOL GetProcessCommandLineByPid(DWORD pid, char *commandLine, size_t commandLineSize);
BOOL TryGetProcessExecutableNameFromSnapshot(DWORD pid, char *processName, size_t processNameSize);
BOOL ParseWatchInput(const char *input, int *watchMode, char *matchPattern, size_t matchPatternSize,
                     char *displayName, size_t displayNameSize);
BOOL IsDuplicateWatchEntry(int watchMode, const char *matchPattern);
BOOL IsWatchedProcessRunning(const WatchedProcess *process, DWORD *pPID, DWORD *pMemoryMB);
void OpenLogFile(HWND hwndOwner);
void OpenLogFolder(HWND hwndOwner);
void ShowEventLogWindow(HWND hwndOwner);
void LayoutEventLogWindow(HWND hwnd);
void RefreshEventLogView(void);
void SanitizeLogField(const char *input, char *output, size_t outputSize);
void EscapeCsvField(const char *input, char *output, size_t outputSize);
BOOL IsAlreadyQuotedCsvSafe(const char *input);
void AddEventLogListRow(int itemIndex, const char *timestamp, const char *eventText,
                        const char *target, const char *pidText, const char *cpuText,
                        const char *memoryText, const char *pathText,
                        const char *commandLineText, const char *details);
int SplitCsvFields(char *line, char **fields, int maxFields);
void WriteApplicationLifecycleLogEntry(BOOL starting);
void WriteProcessStartLogEntry(const WatchedProcess *process);
void WriteProcessStopLogEntry(const WatchedProcess *process);
void WriteWatchListChangeLogEntry(const WatchedProcess *process, const char *eventText);
void RotateStopLogIfNeeded(const char *logPath, DWORD nextEntrySize);
void AppendLogEntry(const char *logEntry);
void AppendStructuredLogEntry(const char *timestamp, const char *eventText, const char *target,
                              const char *pidText, const char *cpuText, const char *memoryText,
                              const char *pathText, const char *commandLineText,
                              const char *details);
void ShowHelpDialog(HWND hwndOwner);
void ShowProcessBrowserWindow(HWND hwndOwner);
LRESULT CALLBACK ProcessBrowserWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void CloseTrackedProcessHandle(WatchedProcess *process);
void ResetStopReason(WatchedProcess *process);
BOOL EnsureTrackedProcessHandle(WatchedProcess *process, DWORD pid);
void CaptureTrackedProcessStopReason(WatchedProcess *process);
void FormatStopReasonSummary(const WatchedProcess *process, char *buffer, size_t bufferSize);
BOOL TryGetRecentApplicationErrorDetails(const WatchedProcess *process, char *buffer, size_t bufferSize);
BOOL GetWindowPickerTarget(DWORD *pid, char *processName, size_t processNameSize);
void UpdateWindowPickerTarget(void);
BOOL IsWindowPickerConfirmationPressed(void);
void HandleWindowPickerTimer(HWND hwndOwner);
void RecordStoppedProcessEvent(const WatchedProcess *process, int *stoppedCount,
                              char *firstStoppedProcess, size_t firstStoppedProcessSize,
                              char *firstStopReason, size_t firstStopReasonSize);
void UpdateRunningProcessSnapshot(WatchedProcess *process, DWORD pid, BOOL shouldLogStart);
void AddProcessListRow(HWND hwndListView, int rowIndex, const WatchedProcess *process);

BOOL Utf8ToWide(const char *input, WCHAR *output, size_t outputCount)
{
    int convertedLength;

    if (!output || outputCount == 0 || outputCount > INT_MAX)
        return FALSE;

    output[0] = L'\0';
    if (!input)
        return TRUE;

    convertedLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                                          output, (int)outputCount);
    if (convertedLength == 0)
    {
        convertedLength = MultiByteToWideChar(CP_ACP, 0, input, -1,
                                              output, (int)outputCount);
    }

    return convertedLength > 0;
}

WCHAR *Utf8ToWideAlloc(const char *input)
{
    WCHAR *buffer;
    int requiredLength;

    if (!input)
    {
        buffer = (WCHAR *)malloc(sizeof(WCHAR));
        if (buffer)
            buffer[0] = L'\0';
        return buffer;
    }

    requiredLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
    if (requiredLength == 0)
        requiredLength = MultiByteToWideChar(CP_ACP, 0, input, -1, NULL, 0);
    if (requiredLength == 0)
        return NULL;

    buffer = (WCHAR *)malloc((size_t)requiredLength * sizeof(WCHAR));
    if (!buffer)
        return NULL;

    if (!Utf8ToWide(input, buffer, (size_t)requiredLength))
    {
        free(buffer);
        return NULL;
    }

    return buffer;
}

BOOL WideToUtf8(const WCHAR *input, char *output, size_t outputSize)
{
    int convertedLength;

    if (!output || outputSize == 0 || outputSize > INT_MAX)
        return FALSE;

    output[0] = '\0';
    if (!input)
        return TRUE;

    convertedLength = WideCharToMultiByte(CP_UTF8, 0, input, -1, output, (int)outputSize, NULL, NULL);
    return convertedLength > 0;
}

char *WideToUtf8Alloc(const WCHAR *input)
{
    char *buffer;
    int requiredLength;

    if (!input)
    {
        buffer = (char *)malloc(1);
        if (buffer)
            buffer[0] = '\0';
        return buffer;
    }

    requiredLength = WideCharToMultiByte(CP_UTF8, 0, input, -1, NULL, 0, NULL, NULL);
    if (requiredLength == 0)
        return NULL;

    buffer = (char *)malloc((size_t)requiredLength);
    if (!buffer)
        return NULL;

    if (!WideToUtf8(input, buffer, (size_t)requiredLength))
    {
        free(buffer);
        return NULL;
    }

    return buffer;
}

int CompareUtf8Insensitive(const char *left, const char *right)
{
    WCHAR *wideLeft;
    WCHAR *wideRight;
    int result = 0;

    if (!left)
        left = "";
    if (!right)
        right = "";

    wideLeft = Utf8ToWideAlloc(left);
    wideRight = Utf8ToWideAlloc(right);
    if (!wideLeft || !wideRight)
    {
        free(wideLeft);
        free(wideRight);
        return _stricmp(left, right);
    }

    switch (CompareStringOrdinal(wideLeft, -1, wideRight, -1, TRUE))
    {
    case CSTR_LESS_THAN:
        result = -1;
        break;
    case CSTR_GREATER_THAN:
        result = 1;
        break;
    default:
        result = 0;
        break;
    }

    free(wideLeft);
    free(wideRight);
    return result;
}

BOOL StartsWithUtf8Insensitive(const char *text, const char *prefix)
{
    WCHAR *wideText;
    WCHAR *widePrefix;
    size_t prefixLength;
    size_t textLength;
    BOOL matches = FALSE;

    if (!text || !prefix)
        return FALSE;

    wideText = Utf8ToWideAlloc(text);
    widePrefix = Utf8ToWideAlloc(prefix);
    if (!wideText || !widePrefix)
    {
        free(wideText);
        free(widePrefix);
        return _strnicmp(text, prefix, strlen(prefix)) == 0;
    }

    prefixLength = wcslen(widePrefix);
    textLength = wcslen(wideText);
    if (prefixLength <= textLength && prefixLength <= INT_MAX)
    {
        matches = CompareStringOrdinal(wideText, (int)prefixLength,
                                       widePrefix, (int)prefixLength, TRUE) == CSTR_EQUAL;
    }

    free(wideText);
    free(widePrefix);
    return matches;
}

DWORD WINAPI GetModuleFileNameUtf8(HMODULE hModule, LPSTR lpFilename, DWORD nSize)
{
    DWORD bufferSize = MAX_PATH;
    WCHAR *widePath = NULL;
    DWORD pathLength = 0;
    DWORD utf8Length = 0;

    if (!lpFilename || nSize == 0)
        return 0;

    lpFilename[0] = '\0';

    while (bufferSize <= 32768)
    {
        widePath = (WCHAR *)malloc((size_t)bufferSize * sizeof(WCHAR));
        if (!widePath)
            return 0;

        pathLength = GetModuleFileNameW(hModule, widePath, bufferSize);
        if (pathLength == 0)
        {
            free(widePath);
            return 0;
        }

        if (pathLength < bufferSize - 1)
            break;

        free(widePath);
        widePath = NULL;
        bufferSize *= 2;
    }

    if (!widePath || !WideToUtf8(widePath, lpFilename, (size_t)nSize))
    {
        free(widePath);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    utf8Length = (DWORD)strlen(lpFilename);
    free(widePath);
    return utf8Length;
}

BOOL WINAPI QueryFullProcessImageNameUtf8(HANDLE hProcess, DWORD dwFlags, LPSTR lpExeName, PDWORD lpdwSize)
{
    WCHAR widePath[32768];
    DWORD wideSize = (DWORD)(sizeof(widePath) / sizeof(widePath[0]));

    if (!lpExeName || !lpdwSize || *lpdwSize == 0)
        return FALSE;

    lpExeName[0] = '\0';
    if (!QueryFullProcessImageNameW(hProcess, dwFlags, widePath, &wideSize))
        return FALSE;

    if (!WideToUtf8(widePath, lpExeName, *lpdwSize))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    *lpdwSize = (DWORD)strlen(lpExeName);
    return TRUE;
}

HANDLE WINAPI CreateFileUtf8(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                             LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                             DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    HANDLE result;
    WCHAR *widePath = Utf8ToWideAlloc(lpFileName);

    if (!widePath)
        return INVALID_HANDLE_VALUE;

    result = CreateFileW(widePath, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                         dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    free(widePath);
    return result;
}

BOOL WINAPI DeleteFileUtf8(LPCSTR lpFileName)
{
    BOOL result;
    WCHAR *widePath = Utf8ToWideAlloc(lpFileName);

    if (!widePath)
        return FALSE;

    result = DeleteFileW(widePath);
    free(widePath);
    return result;
}

BOOL WINAPI MoveFileExUtf8(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags)
{
    BOOL result;
    WCHAR *wideSource = Utf8ToWideAlloc(lpExistingFileName);
    WCHAR *wideTarget = Utf8ToWideAlloc(lpNewFileName);

    if (!wideSource || !wideTarget)
    {
        free(wideSource);
        free(wideTarget);
        return FALSE;
    }

    result = MoveFileExW(wideSource, wideTarget, dwFlags);
    free(wideSource);
    free(wideTarget);
    return result;
}

BOOL WINAPI WritePrivateProfileStringUtf8(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpString, LPCSTR lpFileName)
{
    BOOL result;
    WCHAR *wideApp = Utf8ToWideAlloc(lpAppName);
    WCHAR *wideKey = Utf8ToWideAlloc(lpKeyName);
    WCHAR *wideValue = Utf8ToWideAlloc(lpString);
    WCHAR *wideFile = Utf8ToWideAlloc(lpFileName);

    if (!wideApp || !wideFile || (lpKeyName && !wideKey) || (lpString && !wideValue))
    {
        free(wideApp);
        free(wideKey);
        free(wideValue);
        free(wideFile);
        return FALSE;
    }

    result = WritePrivateProfileStringW(wideApp, wideKey, wideValue, wideFile);
    free(wideApp);
    free(wideKey);
    free(wideValue);
    free(wideFile);
    return result;
}

DWORD WINAPI GetPrivateProfileStringUtf8(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault,
                                         LPSTR lpReturnedString, DWORD nSize, LPCSTR lpFileName)
{
    DWORD copiedLength = 0;
    WCHAR *wideApp = Utf8ToWideAlloc(lpAppName);
    WCHAR *wideKey = Utf8ToWideAlloc(lpKeyName);
    WCHAR *wideDefault = Utf8ToWideAlloc(lpDefault);
    WCHAR *wideFile = Utf8ToWideAlloc(lpFileName);
    WCHAR *wideBuffer;
    char *utf8Buffer;

    if (!lpReturnedString || nSize == 0 || !wideApp || !wideFile ||
        (lpKeyName && !wideKey) || (lpDefault && !wideDefault))
    {
        free(wideApp);
        free(wideKey);
        free(wideDefault);
        free(wideFile);
        return 0;
    }

    wideBuffer = (WCHAR *)calloc((size_t)nSize, sizeof(WCHAR));
    if (!wideBuffer)
    {
        free(wideApp);
        free(wideKey);
        free(wideDefault);
        free(wideFile);
        return 0;
    }

    GetPrivateProfileStringW(wideApp, wideKey, wideDefault, wideBuffer, nSize, wideFile);
    utf8Buffer = WideToUtf8Alloc(wideBuffer);
    if (utf8Buffer)
    {
        strcpy_s(lpReturnedString, nSize, utf8Buffer);
        copiedLength = (DWORD)strlen(lpReturnedString);
        free(utf8Buffer);
    }
    else
    {
        lpReturnedString[0] = '\0';
    }

    free(wideApp);
    free(wideKey);
    free(wideDefault);
    free(wideFile);
    free(wideBuffer);
    return copiedLength;
}

UINT WINAPI GetPrivateProfileIntUtf8(LPCSTR lpAppName, LPCSTR lpKeyName, INT nDefault, LPCSTR lpFileName)
{
    UINT result;
    WCHAR *wideApp = Utf8ToWideAlloc(lpAppName);
    WCHAR *wideKey = Utf8ToWideAlloc(lpKeyName);
    WCHAR *wideFile = Utf8ToWideAlloc(lpFileName);

    if (!wideApp || !wideKey || !wideFile)
    {
        free(wideApp);
        free(wideKey);
        free(wideFile);
        return (UINT)nDefault;
    }

    result = GetPrivateProfileIntW(wideApp, wideKey, nDefault, wideFile);
    free(wideApp);
    free(wideKey);
    free(wideFile);
    return result;
}

int WINAPI MessageBoxUtf8(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
    int result;
    WCHAR *wideText = Utf8ToWideAlloc(lpText);
    WCHAR *wideCaption = Utf8ToWideAlloc(lpCaption);

    if (!wideText || !wideCaption)
    {
        free(wideText);
        free(wideCaption);
        return 0;
    }

    result = MessageBoxW(hWnd, wideText, wideCaption, uType);
    free(wideText);
    free(wideCaption);
    return result;
}

HINSTANCE ShellExecuteUtf8(HWND hwnd, LPCSTR lpOperation, LPCSTR lpFile, LPCSTR lpParameters,
                           LPCSTR lpDirectory, INT nShowCmd)
{
    HINSTANCE result;
    WCHAR *wideOperation = Utf8ToWideAlloc(lpOperation);
    WCHAR *wideFile = Utf8ToWideAlloc(lpFile);
    WCHAR *wideParameters = Utf8ToWideAlloc(lpParameters);
    WCHAR *wideDirectory = Utf8ToWideAlloc(lpDirectory);

    if ((lpOperation && !wideOperation) || !wideFile ||
        (lpParameters && !wideParameters) || (lpDirectory && !wideDirectory))
    {
        free(wideOperation);
        free(wideFile);
        free(wideParameters);
        free(wideDirectory);
        return (HINSTANCE)SE_ERR_OOM;
    }

    result = ShellExecuteW(hwnd, wideOperation, wideFile, wideParameters, wideDirectory, nShowCmd);
    free(wideOperation);
    free(wideFile);
    free(wideParameters);
    free(wideDirectory);
    return result;
}

BOOL WINAPI CreateProcessUtf8(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                              LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                              BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                              LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
                              LPPROCESS_INFORMATION lpProcessInformation)
{
    BOOL result;
    WCHAR *wideApplicationName = Utf8ToWideAlloc(lpApplicationName);
    WCHAR *wideCommandLine = Utf8ToWideAlloc(lpCommandLine);
    WCHAR *wideCurrentDirectory = Utf8ToWideAlloc(lpCurrentDirectory);
    STARTUPINFOW startupInfoW = {0};

    if ((lpApplicationName && !wideApplicationName) || (lpCommandLine && !wideCommandLine) ||
        (lpCurrentDirectory && !wideCurrentDirectory) || !lpStartupInfo)
    {
        free(wideApplicationName);
        free(wideCommandLine);
        free(wideCurrentDirectory);
        return FALSE;
    }

    startupInfoW.cb = sizeof(startupInfoW);
    startupInfoW.dwFlags = lpStartupInfo->dwFlags;
    startupInfoW.wShowWindow = lpStartupInfo->wShowWindow;
    startupInfoW.hStdInput = lpStartupInfo->hStdInput;
    startupInfoW.hStdOutput = lpStartupInfo->hStdOutput;
    startupInfoW.hStdError = lpStartupInfo->hStdError;

    result = CreateProcessW(wideApplicationName, wideCommandLine,
                            lpProcessAttributes, lpThreadAttributes, bInheritHandles,
                            dwCreationFlags, lpEnvironment, wideCurrentDirectory,
                            &startupInfoW, lpProcessInformation);

    free(wideApplicationName);
    free(wideCommandLine);
    free(wideCurrentDirectory);
    return result;
}

WCHAR *ConvertUtf8FilterToWideAlloc(LPCSTR utf8Filter)
{
    const char *cursor;
    WCHAR *wideFilter;
    WCHAR *wideCursor;
    size_t totalLength = 1;

    if (!utf8Filter)
        return NULL;

    cursor = utf8Filter;
    while (*cursor != '\0' || cursor[1] != '\0')
    {
        WCHAR *wideSegment = Utf8ToWideAlloc(cursor);
        size_t segmentLength;

        if (!wideSegment)
            return NULL;

        segmentLength = wcslen(wideSegment) + 1;
        totalLength += segmentLength;
        free(wideSegment);
        cursor += strlen(cursor) + 1;
    }

    wideFilter = (WCHAR *)calloc(totalLength, sizeof(WCHAR));
    if (!wideFilter)
        return NULL;

    cursor = utf8Filter;
    wideCursor = wideFilter;
    while (*cursor != '\0' || cursor[1] != '\0')
    {
        WCHAR *wideSegment = Utf8ToWideAlloc(cursor);
        size_t segmentLength;

        if (!wideSegment)
        {
            free(wideFilter);
            return NULL;
        }

        segmentLength = wcslen(wideSegment) + 1;
        memcpy(wideCursor, wideSegment, segmentLength * sizeof(WCHAR));
        wideCursor += segmentLength;
        free(wideSegment);
        cursor += strlen(cursor) + 1;
    }

    *wideCursor = L'\0';
    return wideFilter;
}

BOOL WINAPI GetOpenFileNameUtf8(LPOPENFILENAMEA lpofn)
{
    BOOL result;
    OPENFILENAMEW wideOfn = {0};
    WCHAR *wideTitle = Utf8ToWideAlloc(lpofn ? lpofn->lpstrTitle : NULL);
    WCHAR *wideFilter = ConvertUtf8FilterToWideAlloc(lpofn ? lpofn->lpstrFilter : NULL);
    WCHAR *wideFile;

    if (!lpofn || !lpofn->lpstrFile || lpofn->nMaxFile == 0 ||
        (lpofn->lpstrTitle && !wideTitle) || (lpofn->lpstrFilter && !wideFilter))
    {
        free(wideTitle);
        free(wideFilter);
        return FALSE;
    }

    wideFile = (WCHAR *)calloc((size_t)lpofn->nMaxFile, sizeof(WCHAR));
    if (!wideFile)
    {
        free(wideTitle);
        free(wideFilter);
        return FALSE;
    }

    if (lpofn->lpstrFile[0] != '\0' &&
        !Utf8ToWide(lpofn->lpstrFile, wideFile, lpofn->nMaxFile))
    {
        free(wideTitle);
        free(wideFilter);
        free(wideFile);
        return FALSE;
    }

    wideOfn.lStructSize = sizeof(wideOfn);
    wideOfn.hwndOwner = lpofn->hwndOwner;
    wideOfn.hInstance = lpofn->hInstance;
    wideOfn.lpstrFilter = wideFilter;
    wideOfn.lpstrFile = wideFile;
    wideOfn.nMaxFile = lpofn->nMaxFile;
    wideOfn.lpstrTitle = wideTitle;
    wideOfn.Flags = lpofn->Flags;
    wideOfn.nFilterIndex = lpofn->nFilterIndex;

    result = GetOpenFileNameW(&wideOfn);
    if (result)
    {
        if (!WideToUtf8(wideFile, lpofn->lpstrFile, lpofn->nMaxFile))
            result = FALSE;
        lpofn->nFilterIndex = wideOfn.nFilterIndex;
    }

    free(wideTitle);
    free(wideFilter);
    free(wideFile);
    return result;
}

BOOL WINAPI SetWindowTextUtf8(HWND hWnd, LPCSTR lpString)
{
    BOOL result;
    WCHAR *wideText = Utf8ToWideAlloc(lpString);

    if (!wideText)
        return FALSE;

    result = SetWindowTextW(hWnd, wideText);
    free(wideText);
    return result;
}

int WINAPI GetWindowTextUtf8(HWND hWnd, LPSTR lpString, int nMaxCount)
{
    WCHAR *wideText;
    int wideLength;

    if (!lpString || nMaxCount <= 0)
        return 0;

    lpString[0] = '\0';
    wideLength = GetWindowTextLengthW(hWnd);
    if (wideLength < 0)
        return 0;

    wideText = (WCHAR *)calloc((size_t)wideLength + 1, sizeof(WCHAR));
    if (!wideText)
        return 0;

    GetWindowTextW(hWnd, wideText, wideLength + 1);
    if (!WideToUtf8(wideText, lpString, (size_t)nMaxCount))
    {
        free(wideText);
        return 0;
    }

    free(wideText);
    return (int)strlen(lpString);
}

ATOM WINAPI RegisterClassExUtf8(const WNDCLASSEXA *lpwcx)
{
    ATOM result;
    WNDCLASSEXW wideClass = {0};
    WCHAR *wideMenuName = NULL;
    WCHAR *wideClassName = NULL;

    if (!lpwcx)
        return 0;

    wideClass.cbSize = sizeof(wideClass);
    wideClass.style = lpwcx->style;
    wideClass.lpfnWndProc = lpwcx->lpfnWndProc;
    wideClass.cbClsExtra = lpwcx->cbClsExtra;
    wideClass.cbWndExtra = lpwcx->cbWndExtra;
    wideClass.hInstance = lpwcx->hInstance;
    wideClass.hIcon = lpwcx->hIcon;
    wideClass.hCursor = lpwcx->hCursor;
    wideClass.hbrBackground = lpwcx->hbrBackground;
    wideClass.hIconSm = lpwcx->hIconSm;

    if (lpwcx->lpszMenuName)
    {
        if (IS_INTRESOURCE(lpwcx->lpszMenuName))
            wideClass.lpszMenuName = (LPCWSTR)lpwcx->lpszMenuName;
        else
        {
            wideMenuName = Utf8ToWideAlloc(lpwcx->lpszMenuName);
            wideClass.lpszMenuName = wideMenuName;
        }
    }

    if (lpwcx->lpszClassName)
    {
        if (IS_INTRESOURCE(lpwcx->lpszClassName))
            wideClass.lpszClassName = (LPCWSTR)lpwcx->lpszClassName;
        else
        {
            wideClassName = Utf8ToWideAlloc(lpwcx->lpszClassName);
            wideClass.lpszClassName = wideClassName;
        }
    }

    result = RegisterClassExW(&wideClass);
    free(wideMenuName);
    free(wideClassName);
    return result;
}

HWND WINAPI CreateWindowExUtf8(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName,
                               DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                               HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
    HWND result;
    WCHAR *wideClassName = NULL;
    WCHAR *wideWindowName = NULL;
    LPCWSTR classNameParam = NULL;

    if (lpClassName)
    {
        if (IS_INTRESOURCE(lpClassName))
            classNameParam = (LPCWSTR)lpClassName;
        else
        {
            wideClassName = Utf8ToWideAlloc(lpClassName);
            classNameParam = wideClassName;
        }
    }

    if (lpWindowName)
        wideWindowName = Utf8ToWideAlloc(lpWindowName);

    result = CreateWindowExW(dwExStyle, classNameParam, wideWindowName,
                             dwStyle, X, Y, nWidth, nHeight,
                             hWndParent, hMenu, hInstance, lpParam);

    free(wideClassName);
    free(wideWindowName);
    return result;
}

LRESULT ComboBoxAddStringUtf8(HWND hwndCombo, const char *text)
{
    LRESULT result;
    WCHAR *wideText = Utf8ToWideAlloc(text);

    if (!wideText)
        return CB_ERR;

    result = SendMessageW(hwndCombo, CB_ADDSTRING, 0, (LPARAM)wideText);
    free(wideText);
    return result;
}

BOOL StatusBarSetTextUtf8(HWND hwndStatusBar, int partIndex, const char *text)
{
    WCHAR *wideText = Utf8ToWideAlloc(text);
    BOOL result;

    if (!wideText)
        return FALSE;

    result = SendMessageW(hwndStatusBar, SB_SETTEXTW, (WPARAM)partIndex, (LPARAM)wideText) != 0;
    free(wideText);
    return result;
}

BOOL ShellNotifyIconUtf8(DWORD dwMessage, const NOTIFYICONDATAA *source)
{
    NOTIFYICONDATAW wideData = {0};

    if (!source)
        return FALSE;

    wideData.cbSize = sizeof(wideData);
    wideData.hWnd = source->hWnd;
    wideData.uID = source->uID;
    wideData.uFlags = source->uFlags;
    wideData.uCallbackMessage = source->uCallbackMessage;
    wideData.hIcon = source->hIcon;
    wideData.dwState = source->dwState;
    wideData.dwStateMask = source->dwStateMask;
    wideData.uTimeout = source->uTimeout;
    wideData.dwInfoFlags = source->dwInfoFlags;
    wideData.guidItem = source->guidItem;
    wideData.hBalloonIcon = source->hBalloonIcon;

    if ((source->uFlags & NIF_TIP) != 0)
        Utf8ToWide(source->szTip, wideData.szTip, sizeof(wideData.szTip) / sizeof(wideData.szTip[0]));
    if ((source->uFlags & NIF_INFO) != 0)
    {
        Utf8ToWide(source->szInfo, wideData.szInfo, sizeof(wideData.szInfo) / sizeof(wideData.szInfo[0]));
        Utf8ToWide(source->szInfoTitle, wideData.szInfoTitle,
                   sizeof(wideData.szInfoTitle) / sizeof(wideData.szInfoTitle[0]));
    }
    if (dwMessage == NIM_SETVERSION)
        wideData.uVersion = source->uVersion;

    return Shell_NotifyIconW(dwMessage, &wideData);
}

BOOL ListViewInsertColumnUtf8(HWND hwndListView, int columnIndex, int columnWidth, const char *text)
{
    LVCOLUMNW column = {0};
    WCHAR *wideText = Utf8ToWideAlloc(text);
    BOOL result;

    if (!wideText)
        return FALSE;

    column.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
    column.fmt = LVCFMT_LEFT;
    column.cx = columnWidth;
    column.pszText = wideText;

    result = SendMessageW(hwndListView, LVM_INSERTCOLUMNW, (WPARAM)columnIndex, (LPARAM)&column) != -1;
    free(wideText);
    return result;
}

int ListViewInsertItemUtf8(HWND hwndListView, int itemIndex, const char *text, LPARAM lParam)
{
    LVITEMW item = {0};
    WCHAR *wideText = Utf8ToWideAlloc(text);
    int result;

    if (!wideText)
        return -1;

    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = itemIndex;
    item.pszText = wideText;
    item.lParam = lParam;

    result = (int)SendMessageW(hwndListView, LVM_INSERTITEMW, 0, (LPARAM)&item);
    free(wideText);
    return result;
}

BOOL ListViewSetItemTextUtf8(HWND hwndListView, int itemIndex, int subItem, const char *text)
{
    LVITEMW item = {0};
    WCHAR *wideText = Utf8ToWideAlloc(text);
    BOOL result;

    if (!wideText)
        return FALSE;

    item.iSubItem = subItem;
    item.pszText = wideText;
    result = SendMessageW(hwndListView, LVM_SETITEMTEXTW, (WPARAM)itemIndex, (LPARAM)&item) != 0;
    free(wideText);
    return result;
}

int ListViewGetItemTextUtf8(HWND hwndListView, int itemIndex, int subItem, char *buffer, int bufferSize)
{
    WCHAR *wideBuffer;
    LVITEMW item = {0};
    int result = 0;

    if (!buffer || bufferSize <= 0)
        return 0;

    buffer[0] = '\0';
    wideBuffer = (WCHAR *)calloc((size_t)bufferSize, sizeof(WCHAR));
    if (!wideBuffer)
        return 0;

    item.iSubItem = subItem;
    item.pszText = wideBuffer;
    item.cchTextMax = bufferSize;
    SendMessageW(hwndListView, LVM_GETITEMTEXTW, (WPARAM)itemIndex, (LPARAM)&item);

    if (WideToUtf8(wideBuffer, buffer, (size_t)bufferSize))
        result = (int)strlen(buffer);

    free(wideBuffer);
    return result;
}

const char *GetCommandLineUtf8(void)
{
    static char buffer[8192];

    if (!WideToUtf8(GetCommandLineW(), buffer, sizeof(buffer)))
        buffer[0] = '\0';

    return buffer;
}

#define GetModuleFileNameA GetModuleFileNameUtf8
#define QueryFullProcessImageNameA QueryFullProcessImageNameUtf8
#define CreateFileA CreateFileUtf8
#define DeleteFileA DeleteFileUtf8
#define MoveFileExA MoveFileExUtf8
#define WritePrivateProfileStringA WritePrivateProfileStringUtf8
#define GetPrivateProfileStringA GetPrivateProfileStringUtf8
#define GetPrivateProfileIntA GetPrivateProfileIntUtf8
#define MessageBoxA MessageBoxUtf8
#define ShellExecuteA ShellExecuteUtf8
#define CreateProcessA CreateProcessUtf8
#define GetOpenFileNameA GetOpenFileNameUtf8
#define SetWindowTextA SetWindowTextUtf8
#define GetWindowTextA GetWindowTextUtf8
#define RegisterClassExA RegisterClassExUtf8
#define CreateWindowExA CreateWindowExUtf8

BOOL IsAutoRefreshEnabled(void)
{
    return g_AppData.autoRefreshEnabled;
}

void ShowHelpDialog(HWND hwndOwner)
{
    MessageBoxA(hwndOwner,
                "What can you watch?\r\n"
                "\r\n"
                "By process name:\r\n"
                "  Type or select a process name, e.g.:\r\n"
                "    notepad.exe\r\n"
                "    chrome.exe\r\n"
                "\r\n"
                "By command line (use the cmd: prefix):\r\n"
                "  Matches any process whose command line contains the text, e.g.:\r\n"
                "    cmd:myscript.py\r\n"
                "    cmd:python myscript.py\r\n"
                "    cmd:--config production\r\n"
                "\r\n"
                "This lets you watch python.exe running a specific script,\r\n"
                "or any process launched with a particular argument.",
                "Watch Syntax Help",
                MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK ComboOpenOnClickProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    (void)dwRefData;

    if (uIdSubclass == 2 &&
        uMsg == WM_LBUTTONDOWN &&
        g_AppData.hwndProcessCombo &&
        SendMessage(g_AppData.hwndProcessCombo, CB_GETDROPPEDSTATE, 0, 0) == 0)
    {
        PostMessage(g_AppData.hwndProcessCombo, CB_SHOWDROPDOWN, TRUE, 0);
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

ULONGLONG FileTimeToUInt64(FILETIME fileTime)
{
    ULARGE_INTEGER value;
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}

ULONGLONG SystemTimeToUInt64(SYSTEMTIME systemTime)
{
    FILETIME fileTime;

    if (!SystemTimeToFileTime(&systemTime, &fileTime))
        return 0;

    return FileTimeToUInt64(fileTime);
}

void TrimProcessName(char *processName)
{
    size_t len = strlen(processName);
    while (len > 0)
    {
        char ch = processName[len - 1];
        if (ch != '\r' && ch != '\n' && ch != ' ' && ch != '\t')
            break;
        processName[--len] = '\0';
    }
}

void NormalizeWatchedProcessName(char *processName)
{
    char *suffix = strrchr(processName, '(');
    if (suffix != NULL && suffix > processName)
    {
        char *spaceBeforeSuffix = suffix - 1;
        char *cursor = suffix + 1;

        if (*spaceBeforeSuffix == ' ')
        {
            while (*cursor >= '0' && *cursor <= '9')
                cursor++;

            if (*cursor == ')' && cursor[1] == '\0')
                *spaceBeforeSuffix = '\0';
        }
    }

    TrimProcessName(processName);
}

BOOL ParseWatchInput(const char *input, int *watchMode, char *matchPattern, size_t matchPatternSize,
                     char *displayName, size_t displayNameSize)
{
    char workingText[1024] = {0};
    const char *patternText;
    int parsedMode = WATCH_MODE_PROCESS_NAME;

    if (!input || !watchMode || !matchPattern || matchPatternSize == 0 || !displayName || displayNameSize == 0)
        return FALSE;

    if (strcpy_s(workingText, sizeof(workingText), input) != 0)
        return FALSE;

    TrimProcessName(workingText);
    if (_strnicmp(workingText, "cmd:", 4) == 0)
    {
        /* `cmd:` switches the watch entry from exact executable matching to
           substring matching against the process command line snapshot. */
        parsedMode = WATCH_MODE_COMMAND_LINE;
        patternText = workingText + 4;
    }
    else
    {
        patternText = workingText;
    }

    while (*patternText == ' ' || *patternText == '\t')
        patternText++;

    if (parsedMode == WATCH_MODE_PROCESS_NAME)
    {
        char normalizedName[256] = {0};

        if (strcpy_s(normalizedName, sizeof(normalizedName), patternText) != 0)
            return FALSE;

        NormalizeWatchedProcessName(normalizedName);
        if (normalizedName[0] == '\0')
            return FALSE;

        if (strcpy_s(matchPattern, matchPatternSize, normalizedName) != 0 ||
            strcpy_s(displayName, displayNameSize, normalizedName) != 0)
        {
            return FALSE;
        }
    }
    else
    {
        char trimmedPattern[1024] = {0};

        if (strcpy_s(trimmedPattern, sizeof(trimmedPattern), patternText) != 0)
            return FALSE;

        TrimProcessName(trimmedPattern);
        if (trimmedPattern[0] == '\0')
            return FALSE;

        if (strcpy_s(matchPattern, matchPatternSize, trimmedPattern) != 0 ||
            sprintf_s(displayName, displayNameSize, "cmd:%s", trimmedPattern) <= 0)
        {
            return FALSE;
        }
    }

    *watchMode = parsedMode;
    return TRUE;
}

void GetSettingsFilePath(char *settingsPath, size_t settingsPathSize)
{
    char *lastSeparator;

    if (!settingsPath || settingsPathSize == 0)
        return;

    if (GetModuleFileNameA(NULL, settingsPath, (DWORD)settingsPathSize) == 0)
    {
        strcpy_s(settingsPath, settingsPathSize, SETTINGS_FILE_NAME);
        return;
    }

    lastSeparator = strrchr(settingsPath, '\\');
    if (lastSeparator)
    {
        lastSeparator[1] = '\0';
        strcat_s(settingsPath, settingsPathSize, SETTINGS_FILE_NAME);
    }
    else
    {
        strcpy_s(settingsPath, settingsPathSize, SETTINGS_FILE_NAME);
    }
}

BOOL GetTempSettingsFilePath(char *tempPath, size_t tempPathSize)
{
    char settingsPath[MAX_PATH] = {0};
    char *extension;

    if (!tempPath || tempPathSize == 0)
        return FALSE;

    GetSettingsFilePath(settingsPath, sizeof(settingsPath));
    if (strcpy_s(tempPath, tempPathSize, settingsPath) != 0)
        return FALSE;

    extension = strrchr(tempPath, '.');
    if (extension)
        return strcpy_s(extension, tempPathSize - (size_t)(extension - tempPath), ".tmp") == 0;

    return strcat_s(tempPath, tempPathSize, ".tmp") == 0;
}

BOOL GetProcessExecutablePathByPid(DWORD pid, char *processPath, size_t processPathSize)
{
    DWORD pathSize;
    HANDLE hProcess;

    if (!processPath || processPathSize == 0)
        return FALSE;

    processPath[0] = '\0';
    pathSize = (DWORD)processPathSize;
    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (!hProcess)
        return FALSE;

    if (!QueryFullProcessImageNameA(hProcess, 0, processPath, &pathSize))
    {
        CloseHandle(hProcess);
        processPath[0] = '\0';
        return FALSE;
    }

    CloseHandle(hProcess);
    return TRUE;
}

BOOL GetProcessCommandLineByPid(DWORD pid, char *commandLine, size_t commandLineSize)
{
    typedef NTSTATUS(NTAPI * NtQueryInformationProcessFn)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    typedef struct _PROCESS_BASIC_INFORMATION_COMPAT
    {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    } PROCESS_BASIC_INFORMATION_COMPAT;
    typedef struct _PEB_COMPAT
    {
        BYTE Reserved1[2];
        BYTE BeingDebugged;
        BYTE Reserved2[1];
        PVOID Reserved3[2];
        PVOID Ldr;
        PVOID ProcessParameters;
    } PEB_COMPAT;
    typedef struct _RTL_USER_PROCESS_PARAMETERS_COMPAT
    {
        BYTE Reserved1[16];
        PVOID Reserved2[10];
        UNICODE_STRING ImagePathName;
        UNICODE_STRING CommandLine;
    } RTL_USER_PROCESS_PARAMETERS_COMPAT;

    HMODULE hNtDll;
    NtQueryInformationProcessFn pNtQueryInformationProcess;
    HANDLE hProcess;
    PROCESS_BASIC_INFORMATION_COMPAT processInfo = {0};
    PEB_COMPAT peb = {0};
    RTL_USER_PROCESS_PARAMETERS_COMPAT processParameters = {0};
    WCHAR *commandLineBuffer = NULL;
    SIZE_T bytesRead = 0;
    ULONG returnLength = 0;
    int convertedLength;
    BOOL success = FALSE;

    if (!commandLine || commandLineSize == 0)
        return FALSE;

    commandLine[0] = '\0';
    hNtDll = GetModuleHandleA("ntdll.dll");
    if (!hNtDll)
        return FALSE;

    {
        FARPROC procAddress = GetProcAddress(hNtDll, "NtQueryInformationProcess");
        if (!procAddress)
            return FALSE;

        memcpy(&pNtQueryInformationProcess, &procAddress, sizeof(procAddress));
    }
    if (!pNtQueryInformationProcess)
        return FALSE;

    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess)
        return FALSE;

    if (pNtQueryInformationProcess(hProcess, ProcessBasicInformation,
                                   &processInfo, sizeof(processInfo), &returnLength) < 0 ||
        !processInfo.PebBaseAddress)
    {
        CloseHandle(hProcess);
        return FALSE;
    }

    if (!ReadProcessMemory(hProcess, processInfo.PebBaseAddress, &peb, sizeof(peb), &bytesRead) ||
        bytesRead < sizeof(peb) ||
        !peb.ProcessParameters)
    {
        CloseHandle(hProcess);
        return FALSE;
    }

    if (!ReadProcessMemory(hProcess, peb.ProcessParameters, &processParameters,
                           sizeof(processParameters), &bytesRead) ||
        bytesRead < sizeof(processParameters) ||
        !processParameters.CommandLine.Buffer ||
        processParameters.CommandLine.Length == 0)
    {
        CloseHandle(hProcess);
        return FALSE;
    }

    commandLineBuffer = (WCHAR *)malloc((size_t)processParameters.CommandLine.Length + sizeof(WCHAR));
    if (!commandLineBuffer)
    {
        CloseHandle(hProcess);
        return FALSE;
    }

    if (!ReadProcessMemory(hProcess, processParameters.CommandLine.Buffer, commandLineBuffer,
                           processParameters.CommandLine.Length, &bytesRead) ||
        bytesRead < processParameters.CommandLine.Length)
    {
        free(commandLineBuffer);
        CloseHandle(hProcess);
        return FALSE;
    }

    commandLineBuffer[processParameters.CommandLine.Length / sizeof(WCHAR)] = L'\0';
    convertedLength = WideCharToMultiByte(CP_UTF8, 0, commandLineBuffer, -1,
                                          commandLine, (int)commandLineSize, NULL, NULL);
    success = convertedLength > 0;

    free(commandLineBuffer);
    CloseHandle(hProcess);
    return success;
}

BOOL IsWatchedProcessName(const char *processName)
{
    for (int i = 0; i < g_AppData.count; i++)
    {
        if (CompareUtf8Insensitive(g_AppData.processes[i].name, processName) == 0)
            return TRUE;
    }

    return FALSE;
}

BOOL IsDuplicateWatchEntry(int watchMode, const char *matchPattern)
{
    if (!matchPattern)
        return FALSE;

    for (int i = 0; i < g_AppData.count; i++)
    {
        if (g_AppData.processes[i].watchMode == watchMode &&
            CompareUtf8Insensitive(g_AppData.processes[i].matchPattern, matchPattern) == 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

BOOL ContainsTextInsensitive(const char *text, const char *pattern)
{
    WCHAR *wideText;
    WCHAR *widePattern;
    size_t textLength;
    size_t patternLength;
    BOOL matches = FALSE;

    if (!text || !pattern)
        return FALSE;

    wideText = Utf8ToWideAlloc(text);
    widePattern = Utf8ToWideAlloc(pattern);
    if (!wideText || !widePattern)
    {
        free(wideText);
        free(widePattern);
        patternLength = strlen(pattern);
        if (patternLength == 0)
            return TRUE;

        for (; *text != '\0'; text++)
        {
            if (_strnicmp(text, pattern, patternLength) == 0)
                return TRUE;
        }

        return FALSE;
    }

    patternLength = wcslen(widePattern);
    if (patternLength == 0)
    {
        free(wideText);
        free(widePattern);
        return TRUE;
    }

    textLength = wcslen(wideText);
    for (size_t i = 0; i + patternLength <= textLength; i++)
    {
        if (patternLength <= INT_MAX &&
            CompareStringOrdinal(wideText + i, (int)patternLength,
                                 widePattern, (int)patternLength, TRUE) == CSTR_EQUAL)
        {
            matches = TRUE;
            break;
        }
    }

    free(wideText);
    free(widePattern);
    return matches;
}

BOOL IsWatchedProcessRunning(const WatchedProcess *process, DWORD *pPID, DWORD *pMemoryMB)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe32 = {0};

    if (hSnapshot == INVALID_HANDLE_VALUE || !process)
        return FALSE;

    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32))
    {
        do
        {
            BOOL matches = FALSE;

            if (process->watchMode == WATCH_MODE_PROCESS_NAME)
            {
                char processName[256] = {0};

                WideToUtf8(pe32.szExeFile, processName, sizeof(processName));
                matches = CompareUtf8Insensitive(processName, process->matchPattern) == 0;
            }
            else if (process->watchMode == WATCH_MODE_COMMAND_LINE)
            {
                char commandLine[4096] = {0};

                if (GetProcessCommandLineByPid(pe32.th32ProcessID, commandLine, sizeof(commandLine)))
                    matches = ContainsTextInsensitive(commandLine, process->matchPattern);
            }

            if (matches)
            {
                if (pPID)
                    *pPID = pe32.th32ProcessID;
                if (pMemoryMB)
                    *pMemoryMB = GetProcessMemoryMB(pe32.th32ProcessID);
                CloseHandle(hSnapshot);
                return TRUE;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return FALSE;
}

int GetComboMatchRank(const char *processName, const char *filterText)
{
    size_t filterLength;

    if (!filterText)
        return 0;

    filterLength = strlen(filterText);
    if (filterLength == 0)
        return 0;

    if (CompareUtf8Insensitive(processName, filterText) == 0)
        return 0;

    if (StartsWithUtf8Insensitive(processName, filterText))
        return 1;

    if (ContainsTextInsensitive(processName, filterText))
        return 2;

    return -1;
}

int CompareComboProcessEntries(const void *left, const void *right)
{
    const ComboProcessEntry *entryLeft = (const ComboProcessEntry *)left;
    const ComboProcessEntry *entryRight = (const ComboProcessEntry *)right;
    int result = 0;

    if (entryLeft->matchRank < entryRight->matchRank)
        result = -1;
    else if (entryLeft->matchRank > entryRight->matchRank)
        result = 1;

    if (result == 0)
        result = CompareUtf8Insensitive(entryLeft->name, entryRight->name);
    if (result == 0)
    {
        if (entryLeft->pid < entryRight->pid)
            result = -1;
        else if (entryLeft->pid > entryRight->pid)
            result = 1;
    }

    return result;
}

DWORD GetProcessMemoryMB(DWORD pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess)
        return 0;

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc)))
    {
        CloseHandle(hProcess);
        return pmc.WorkingSetSize / (1024 * 1024);
    }

    CloseHandle(hProcess);
    return 0;
}

void ResetProcessCpuSample(WatchedProcess *process)
{
    process->cpuPercent = 0.0;
    process->lastCpuTime = 0;
    process->lastSampleTime = 0;
    process->lastSamplePid = 0;
}

void CloseTrackedProcessHandle(WatchedProcess *process)
{
    if (!process)
        return;

    if (process->trackedProcessHandle)
    {
        CloseHandle(process->trackedProcessHandle);
        process->trackedProcessHandle = NULL;
    }

    process->trackedProcessPid = 0;
}

void ResetStopReason(WatchedProcess *process)
{
    if (!process)
        return;

    process->lastStopExitCode = 0;
    process->hasLastStopExitCode = FALSE;
    process->lastStopReason[0] = '\0';
    process->lastStopDetails[0] = '\0';
}

const char *DescribeExitCode(DWORD exitCode)
{
    switch (exitCode)
    {
    case 0:
        return "Normal exit";
    case STATUS_ACCESS_VIOLATION:
        return "Crash: access violation";
    case STATUS_STACK_OVERFLOW:
        return "Crash: stack overflow";
    case STATUS_ILLEGAL_INSTRUCTION:
        return "Crash: illegal instruction";
    case STATUS_PRIVILEGED_INSTRUCTION:
        return "Crash: privileged instruction";
    case STATUS_INTEGER_DIVIDE_BY_ZERO:
        return "Crash: divide by zero";
    case STATUS_INTEGER_OVERFLOW:
        return "Crash: integer overflow";
    case STATUS_FLOAT_DIVIDE_BY_ZERO:
        return "Crash: floating-point divide by zero";
    case STATUS_FLOAT_OVERFLOW:
        return "Crash: floating-point overflow";
    case STATUS_FLOAT_INVALID_OPERATION:
        return "Crash: invalid floating-point operation";
    case STATUS_HEAP_CORRUPTION:
        return "Crash: heap corruption";
    case STATUS_DLL_NOT_FOUND:
        return "Startup failure: DLL not found";
    case STATUS_DLL_INIT_FAILED:
        return "Startup failure: DLL initialization failed";
    case STATUS_CONTROL_C_EXIT:
        return "Terminated by console control event";
    default:
        break;
    }

    if ((exitCode & 0xF0000000u) == 0xC0000000u)
        return "Crash: unhandled exception";

    return "Exited with non-zero code";
}

BOOL EnsureTrackedProcessHandle(WatchedProcess *process, DWORD pid)
{
    HANDLE hProcess;

    if (!process || pid == 0)
        return FALSE;

    if (process->trackedProcessHandle && process->trackedProcessPid == pid)
        return TRUE;

    CloseTrackedProcessHandle(process);
    ResetStopReason(process);

    hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess)
        return FALSE;

    process->trackedProcessHandle = hProcess;
    process->trackedProcessPid = pid;
    return TRUE;
}

void CaptureTrackedProcessStopReason(WatchedProcess *process)
{
    DWORD exitCode = STILL_ACTIVE;

    if (!process)
        return;

    if (process->trackedProcessHandle &&
        WaitForSingleObject(process->trackedProcessHandle, 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process->trackedProcessHandle, &exitCode) &&
        exitCode != STILL_ACTIVE)
    {
        process->lastStopExitCode = exitCode;
        process->hasLastStopExitCode = TRUE;
        strcpy_s(process->lastStopReason, sizeof(process->lastStopReason), DescribeExitCode(exitCode));
        if ((exitCode & 0xF0000000u) == 0xC0000000u)
            TryGetRecentApplicationErrorDetails(process, process->lastStopDetails, sizeof(process->lastStopDetails));
    }
    else if (process->lastStopReason[0] == '\0')
    {
        strcpy_s(process->lastStopReason, sizeof(process->lastStopReason),
                 "Stopped, but exit reason was unavailable");
    }

    CloseTrackedProcessHandle(process);
}

void FormatStopReasonSummary(const WatchedProcess *process, char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return;

    if (!process)
    {
        strcpy_s(buffer, bufferSize, "Unknown");
        return;
    }

    if (process->hasLastStopExitCode)
    {
        sprintf_s(buffer, bufferSize, "%s (0x%08lX / %lu)",
                  process->lastStopReason[0] != '\0' ? process->lastStopReason : "Stopped",
                  process->lastStopExitCode, process->lastStopExitCode);
        return;
    }

    if (process->lastStopReason[0] != '\0')
    {
        strcpy_s(buffer, bufferSize, process->lastStopReason);
        return;
    }

    strcpy_s(buffer, bufferSize, "Unknown");
}

void UpdateProcessLastSeen(WatchedProcess *process)
{
    if (!process)
        return;

    GetLocalTime(&process->lastSeenLocalTime);
    process->hasLastSeen = TRUE;
}

void FormatProcessLastSeen(const WatchedProcess *process, char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return;

    if (!process || !process->hasLastSeen)
    {
        strcpy_s(buffer, bufferSize, "-");
        return;
    }

    sprintf_s(buffer, bufferSize, "%04u-%02u-%02u %02u:%02u:%02u",
              process->lastSeenLocalTime.wYear,
              process->lastSeenLocalTime.wMonth,
              process->lastSeenLocalTime.wDay,
              process->lastSeenLocalTime.wHour,
              process->lastSeenLocalTime.wMinute,
              process->lastSeenLocalTime.wSecond);
}

void FormatSystemTimeForLog(const SYSTEMTIME *systemTime, char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return;

    if (!systemTime)
    {
        strcpy_s(buffer, bufferSize, "Unknown");
        return;
    }

    sprintf_s(buffer, bufferSize, "%04u-%02u-%02u %02u:%02u:%02u",
              systemTime->wYear, systemTime->wMonth, systemTime->wDay,
              systemTime->wHour, systemTime->wMinute, systemTime->wSecond);
}

void GetStopLogFilePath(char *logPath, size_t logPathSize)
{
    char *lastSeparator;

    if (!logPath || logPathSize == 0)
        return;

    if (GetModuleFileNameA(NULL, logPath, (DWORD)logPathSize) == 0)
    {
        strcpy_s(logPath, logPathSize, "ProcessWatcher.log");
        return;
    }

    lastSeparator = strrchr(logPath, '\\');
    if (lastSeparator)
    {
        lastSeparator[1] = '\0';
        strcat_s(logPath, logPathSize, "ProcessWatcher.log");
    }
    else
    {
        strcpy_s(logPath, logPathSize, "ProcessWatcher.log");
    }
}

void RotateStopLogIfNeeded(const char *logPath, DWORD nextEntrySize)
{
    HANDLE hLogFile;
    LARGE_INTEGER fileSize = {0};

    if (!logPath || logPath[0] == '\0')
        return;

    hLogFile = CreateFileA(logPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLogFile == INVALID_HANDLE_VALUE)
        return;

    if (!GetFileSizeEx(hLogFile, &fileSize))
    {
        CloseHandle(hLogFile);
        return;
    }

    CloseHandle(hLogFile);

    if ((unsigned long long)fileSize.QuadPart + (unsigned long long)nextEntrySize <
        DEFAULT_STOP_LOG_MAX_BYTES)
    {
        return;
    }

    for (int i = DEFAULT_STOP_LOG_MAX_ROTATIONS; i >= 1; i--)
    {
        char sourcePath[MAX_PATH] = {0};
        char targetPath[MAX_PATH] = {0};

        if (i == DEFAULT_STOP_LOG_MAX_ROTATIONS)
        {
            sprintf_s(sourcePath, sizeof(sourcePath), "%s.%d", logPath, i);
            DeleteFileA(sourcePath);
        }

        if (i == 1)
            strcpy_s(sourcePath, sizeof(sourcePath), logPath);
        else
            sprintf_s(sourcePath, sizeof(sourcePath), "%s.%d", logPath, i - 1);

        sprintf_s(targetPath, sizeof(targetPath), "%s.%d", logPath, i);
        MoveFileExA(sourcePath, targetPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
}

void AppendLogEntry(const char *logEntry)
{
    char logPath[MAX_PATH] = {0};
    DWORD bytesWritten = 0;
    DWORD desiredLength;
    LARGE_INTEGER fileSize = {0};
    HANDLE hLogFile;

    if (!logEntry || logEntry[0] == '\0')
        return;

    GetStopLogFilePath(logPath, sizeof(logPath));
    desiredLength = (DWORD)strlen(logEntry);
    RotateStopLogIfNeeded(logPath, desiredLength);

    hLogFile = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLogFile == INVALID_HANDLE_VALUE)
        return;

    if (GetFileSizeEx(hLogFile, &fileSize) && fileSize.QuadPart == 0)
    {
        DWORD headerLength = (DWORD)strlen(LOG_FILE_HEADER);
        WriteFile(hLogFile, LOG_FILE_HEADER, headerLength, &bytesWritten, NULL);
    }

    WriteFile(hLogFile, logEntry, desiredLength, &bytesWritten, NULL);
    CloseHandle(hLogFile);
    RefreshEventLogView();
}

void SanitizeLogField(const char *input, char *output, size_t outputSize)
{
    size_t writeIndex = 0;

    if (!output || outputSize == 0)
        return;

    output[0] = '\0';
    if (!input)
        return;

    for (size_t readIndex = 0; input[readIndex] != '\0' && writeIndex + 1 < outputSize; readIndex++)
    {
        char ch = input[readIndex];

        if (ch == '\r' || ch == '\n' || ch == '\t')
        {
            if (writeIndex > 0 && output[writeIndex - 1] != ' ' && writeIndex + 1 < outputSize)
                output[writeIndex++] = ' ';
            continue;
        }

        output[writeIndex++] = ch;
    }

    while (writeIndex > 0 && output[writeIndex - 1] == ' ')
        writeIndex--;

    output[writeIndex] = '\0';
}

void EscapeCsvField(const char *input, char *output, size_t outputSize)
{
    size_t writeIndex = 0;

    if (!output || outputSize == 0)
        return;

    output[0] = '\0';
    if (!input)
        input = "";

    if (writeIndex + 1 < outputSize)
        output[writeIndex++] = '"';

    for (size_t readIndex = 0; input[readIndex] != '\0' && writeIndex + 1 < outputSize; readIndex++)
    {
        char ch = input[readIndex];

        if (ch == '"')
        {
            if (writeIndex + 2 >= outputSize)
                break;
            output[writeIndex++] = '"';
            output[writeIndex++] = '"';
        }
        else
        {
            output[writeIndex++] = ch;
        }
    }

    if (writeIndex + 1 < outputSize)
        output[writeIndex++] = '"';

    output[writeIndex] = '\0';
}

BOOL IsAlreadyQuotedCsvSafe(const char *input)
{
    size_t length;

    if (!input)
        return FALSE;

    length = strlen(input);
    if (length < 2 || input[0] != '"' || input[length - 1] != '"')
        return FALSE;

    for (size_t i = 1; i + 1 < length; i++)
    {
        if (input[i] == '"' || input[i] == ',' || input[i] == '\r' || input[i] == '\n')
            return FALSE;
    }

    return TRUE;
}

void AppendStructuredLogEntry(const char *timestamp, const char *eventText, const char *target,
                              const char *pidText, const char *cpuText, const char *memoryText,
                              const char *pathText, const char *commandLineText,
                              const char *details)
{
    char sanitizedTimestamp[64] = {0};
    char sanitizedEvent[128] = {0};
    char sanitizedTarget[256] = {0};
    char sanitizedPid[64] = {0};
    char sanitizedCpu[64] = {0};
    char sanitizedMemory[64] = {0};
    char sanitizedPath[MAX_PATH * 2] = {0};
    char sanitizedCommandLine[4096] = {0};
    char sanitizedDetails[2048] = {0};
    char escapedTimestamp[160] = {0};
    char escapedEvent[320] = {0};
    char escapedTarget[640] = {0};
    char escapedPid[160] = {0};
    char escapedCpu[160] = {0};
    char escapedMemory[160] = {0};
    char escapedPath[(MAX_PATH * 4)] = {0};
    char escapedCommandLine[8192] = {0};
    char escapedDetails[4096] = {0};
    char logEntry[16384];

    if (!g_AppData.createStopLogs)
        return;

    SanitizeLogField(timestamp ? timestamp : "", sanitizedTimestamp, sizeof(sanitizedTimestamp));
    SanitizeLogField(eventText ? eventText : "", sanitizedEvent, sizeof(sanitizedEvent));
    SanitizeLogField(target ? target : "", sanitizedTarget, sizeof(sanitizedTarget));
    SanitizeLogField(pidText ? pidText : "", sanitizedPid, sizeof(sanitizedPid));
    SanitizeLogField(cpuText ? cpuText : "", sanitizedCpu, sizeof(sanitizedCpu));
    SanitizeLogField(memoryText ? memoryText : "", sanitizedMemory, sizeof(sanitizedMemory));
    SanitizeLogField(pathText ? pathText : "", sanitizedPath, sizeof(sanitizedPath));
    SanitizeLogField(commandLineText ? commandLineText : "", sanitizedCommandLine, sizeof(sanitizedCommandLine));
    SanitizeLogField(details ? details : "", sanitizedDetails, sizeof(sanitizedDetails));

    EscapeCsvField(sanitizedTimestamp, escapedTimestamp, sizeof(escapedTimestamp));
    EscapeCsvField(sanitizedEvent, escapedEvent, sizeof(escapedEvent));
    EscapeCsvField(sanitizedTarget, escapedTarget, sizeof(escapedTarget));
    EscapeCsvField(sanitizedPid, escapedPid, sizeof(escapedPid));
    EscapeCsvField(sanitizedCpu, escapedCpu, sizeof(escapedCpu));
    EscapeCsvField(sanitizedMemory, escapedMemory, sizeof(escapedMemory));
    EscapeCsvField(sanitizedPath, escapedPath, sizeof(escapedPath));
    if (IsAlreadyQuotedCsvSafe(sanitizedCommandLine))
        strcpy_s(escapedCommandLine, sizeof(escapedCommandLine), sanitizedCommandLine);
    else
        EscapeCsvField(sanitizedCommandLine, escapedCommandLine, sizeof(escapedCommandLine));
    EscapeCsvField(sanitizedDetails, escapedDetails, sizeof(escapedDetails));

    sprintf_s(logEntry, sizeof(logEntry), "%s,%s,%s,%s,%s,%s,%s,%s,%s\r\n",
              escapedTimestamp, escapedEvent, escapedTarget, escapedPid,
              escapedCpu, escapedMemory, escapedPath, escapedCommandLine, escapedDetails);
    AppendLogEntry(logEntry);
}

BOOL TryGetRecentApplicationErrorDetails(const WatchedProcess *process, char *buffer, size_t bufferSize)
{
    HANDLE hEventLog;
    BYTE *recordBuffer;
    DWORD bytesRead = 0;
    DWORD minBytesNeeded = 0;
    DWORD nowSeconds;

    if (!buffer || bufferSize == 0)
        return FALSE;

    buffer[0] = '\0';
    if (!process || process->pid == 0 || process->name[0] == '\0')
        return FALSE;

    hEventLog = OpenEventLogA(NULL, "Application");
    if (!hEventLog)
        return FALSE;

    recordBuffer = (BYTE *)malloc(64 * 1024);
    if (!recordBuffer)
    {
        CloseEventLog(hEventLog);
        return FALSE;
    }

    nowSeconds = (DWORD)time(NULL);

    while (ReadEventLogA(hEventLog, EVENTLOG_BACKWARDS_READ | EVENTLOG_SEQUENTIAL_READ, 0,
                         recordBuffer, 64 * 1024, &bytesRead, &minBytesNeeded))
    {
        DWORD offset = 0;

        while (offset < bytesRead)
        {
            EVENTLOGRECORD *record = (EVENTLOGRECORD *)(recordBuffer + offset);
            LPCSTR sourceName = (LPCSTR)(recordBuffer + offset + sizeof(EVENTLOGRECORD));
            DWORD eventId = record->EventID & 0xFFFF;

            if (nowSeconds >= record->TimeGenerated && nowSeconds - record->TimeGenerated > 600)
            {
                free(recordBuffer);
                CloseEventLog(hEventLog);
                return FALSE;
            }

            if (CompareUtf8Insensitive(sourceName, "Application Error") == 0 && eventId == 1000 && record->NumStrings >= 8)
            {
                LPCSTR strings[16] = {0};
                LPCSTR cursor = (LPCSTR)(recordBuffer + offset + record->StringOffset);
                DWORD stringCount = record->NumStrings < 16 ? record->NumStrings : 16;
                DWORD loggedPid = 0;
                LPCSTR appName;
                LPCSTR moduleName;
                LPCSTR exceptionCode;
                LPCSTR faultOffset;

                for (DWORD i = 0; i < stringCount; i++)
                {
                    strings[i] = cursor;
                    cursor += strlen(cursor) + 1;
                }

                appName = strings[0] ? strings[0] : "";
                moduleName = strings[2] ? strings[2] : "Unknown";
                exceptionCode = strings[5] ? strings[5] : "Unknown";
                faultOffset = strings[6] ? strings[6] : "Unknown";
                if (strings[7] && strings[7][0] != '\0')
                    loggedPid = strtoul(strings[7], NULL, 0);

                if (loggedPid == process->pid && CompareUtf8Insensitive(appName, process->name) == 0)
                {
                    sprintf_s(buffer, bufferSize,
                              "Windows Application Error: module %s, exception %s, offset %s",
                              moduleName, exceptionCode, faultOffset);
                    free(recordBuffer);
                    CloseEventLog(hEventLog);
                    return TRUE;
                }
            }

            if (record->Length < sizeof(EVENTLOGRECORD))
                break;
            offset += record->Length;
        }
    }

    free(recordBuffer);
    CloseEventLog(hEventLog);
    return FALSE;
}

void WriteProcessStopLogEntry(const WatchedProcess *process)
{
    char stopTimeText[32];
    char stopReasonText[256];
    char detailText[1024];
    char pidText[32];
    char cpuText[32];
    char memoryText[32];
    SYSTEMTIME stopTime;

    if (!process || !g_AppData.createStopLogs)
        return;

    GetLocalTime(&stopTime);
    FormatSystemTimeForLog(&stopTime, stopTimeText, sizeof(stopTimeText));
    FormatStopReasonSummary(process, stopReasonText, sizeof(stopReasonText));
    if (process->lastStopDetails[0] != '\0')
        sprintf_s(detailText, sizeof(detailText), "%s | %s", stopReasonText, process->lastStopDetails);
    else
        strcpy_s(detailText, sizeof(detailText), stopReasonText);

    sprintf_s(pidText, sizeof(pidText), "%lu", process->pid);
    sprintf_s(cpuText, sizeof(cpuText), "%.1f%%", process->cpuPercent);
    sprintf_s(memoryText, sizeof(memoryText), "%lu MB", process->memoryMB);

    AppendStructuredLogEntry(stopTimeText, "STOPPED",
                             process->name[0] != '\0' ? process->name : "Unknown",
                             pidText, cpuText, memoryText,
                             process->executablePath[0] != '\0' ? process->executablePath : "",
                             process->commandLine[0] != '\0' ? process->commandLine : "",
                             detailText);
}

void WriteProcessStartLogEntry(const WatchedProcess *process)
{
    char startTimeText[32];
    char pidText[32];
    SYSTEMTIME startTime;

    if (!process || !g_AppData.createStopLogs)
        return;

    GetLocalTime(&startTime);
    FormatSystemTimeForLog(&startTime, startTimeText, sizeof(startTimeText));

    sprintf_s(pidText, sizeof(pidText), "%lu", process->pid);
    AppendStructuredLogEntry(startTimeText, "STARTED",
                             process->name[0] != '\0' ? process->name : "Unknown",
                             pidText, "", "",
                             process->executablePath[0] != '\0' ? process->executablePath : "",
                             process->commandLine[0] != '\0' ? process->commandLine : "",
                             "");
}

void WriteApplicationLifecycleLogEntry(BOOL starting)
{
    char executablePath[MAX_PATH] = {0};
    char timestampText[32];
    char pidText[32];
    DWORD pid;
    SYSTEMTIME eventTime;
    LPCSTR commandLine;

    if (!g_AppData.createStopLogs)
        return;

    GetLocalTime(&eventTime);
    FormatSystemTimeForLog(&eventTime, timestampText, sizeof(timestampText));

    if (GetModuleFileNameA(NULL, executablePath, sizeof(executablePath)) == 0)
        strcpy_s(executablePath, sizeof(executablePath), "Unknown");

    pid = GetCurrentProcessId();
    commandLine = GetCommandLineUtf8();

    sprintf_s(pidText, sizeof(pidText), "%lu", pid);
    AppendStructuredLogEntry(timestampText,
                             starting ? "STARTED" : "STOPPED",
                             "ProcessWatcher",
                             pidText, "", "",
                             executablePath,
                             commandLine && commandLine[0] != '\0' ? commandLine : "",
                             "");
}

void WriteWatchListChangeLogEntry(const WatchedProcess *process, const char *eventText)
{
    char timestampText[32];
    char details[1400];
    const char *watchModeText;
    SYSTEMTIME eventTime;

    if (!process || !eventText || eventText[0] == '\0' || !g_AppData.createStopLogs)
        return;

    GetLocalTime(&eventTime);
    FormatSystemTimeForLog(&eventTime, timestampText, sizeof(timestampText));

    watchModeText = process->watchMode == WATCH_MODE_COMMAND_LINE ? "Command Line" : "Process Name";
    sprintf_s(details, sizeof(details), "Watch Mode: %s | Match Pattern: %s",
              watchModeText,
              process->matchPattern[0] != '\0' ? process->matchPattern : "(empty)");

    AppendStructuredLogEntry(timestampText,
                             eventText,
                             process->name[0] != '\0' ? process->name : "Unknown",
                             "", "", "",
                             "",
                             "",
                             details);
}

void AddEventLogListRow(int itemIndex, const char *timestamp, const char *eventText,
                        const char *target, const char *pidText, const char *cpuText,
                        const char *memoryText, const char *pathText,
                        const char *commandLineText, const char *details)
{
    if (!g_AppData.hwndEventLogEdit)
        return;

    if (ListViewInsertItemUtf8(g_AppData.hwndEventLogEdit, itemIndex,
                               timestamp ? timestamp : "", (LPARAM)itemIndex) == -1)
        return;

    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 1, eventText ? eventText : "");
    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 2, target ? target : "");
    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 3, pidText ? pidText : "");
    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 4, cpuText ? cpuText : "");
    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 5, memoryText ? memoryText : "");
    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 6, pathText ? pathText : "");
    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 7, commandLineText ? commandLineText : "");
    ListViewSetItemTextUtf8(g_AppData.hwndEventLogEdit, itemIndex, 8, details ? details : "");
}

int SplitCsvFields(char *line, char **fields, int maxFields)
{
    int fieldCount = 0;
    char *cursor;

    if (!line || !fields || maxFields <= 0)
        return 0;

    cursor = line;
    while (fieldCount < maxFields)
    {
        fields[fieldCount++] = cursor;
        if (*cursor == '"')
        {
            char *readCursor = cursor + 1;
            char *writeCursor = cursor;

            while (*readCursor != '\0')
            {
                if (*readCursor == '"')
                {
                    if (readCursor[1] == '"')
                    {
                        *writeCursor++ = '"';
                        readCursor += 2;
                        continue;
                    }

                    readCursor++;
                    break;
                }

                *writeCursor++ = *readCursor++;
            }

            *writeCursor = '\0';
            cursor = readCursor;
            if (*cursor == ',')
            {
                *cursor = '\0';
                cursor++;
                continue;
            }
            break;
        }
        else
        {
            char *delimiter = strchr(cursor, ',');

            if (!delimiter)
                break;

            *delimiter = '\0';
            cursor = delimiter + 1;
            continue;
        }
    }

    while (fieldCount < maxFields)
        fields[fieldCount++] = "";

    return fieldCount;
}

void RefreshEventLogView(void)
{
    char logPath[MAX_PATH] = {0};
    HANDLE hLogFile;
    LARGE_INTEGER fileSize = {0};
    DWORD bytesToRead;
    DWORD bytesRead = 0;
    size_t startOffset = 0;
    char *buffer;
    int rowIndex = 0;

    if (!g_AppData.hwndEventLogEdit)
        return;

    ListView_DeleteAllItems(g_AppData.hwndEventLogEdit);

    GetStopLogFilePath(logPath, sizeof(logPath));
    hLogFile = CreateFileA(logPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLogFile == INVALID_HANDLE_VALUE)
    {
        AddEventLogListRow(0, "", "Info", "ProcessWatcher", "", "", "", "", "",
                           g_AppData.createStopLogs ? "No log entries yet." : "Logging is disabled.");
        return;
    }

    if (!GetFileSizeEx(hLogFile, &fileSize))
    {
        CloseHandle(hLogFile);
        AddEventLogListRow(0, "", "Error", "ProcessWatcher", "", "", "", "", "", "Unable to read the event log.");
        return;
    }

    bytesToRead = (DWORD)((fileSize.QuadPart > EVENT_LOG_VIEW_TAIL_BYTES) ? EVENT_LOG_VIEW_TAIL_BYTES : fileSize.QuadPart);
    buffer = (char *)malloc((size_t)bytesToRead + 1);
    if (!buffer)
    {
        CloseHandle(hLogFile);
        AddEventLogListRow(0, "", "Error", "ProcessWatcher", "", "", "", "", "",
                           "Unable to allocate memory for the event log view.");
        return;
    }

    if (fileSize.QuadPart > bytesToRead)
    {
        LARGE_INTEGER seekPosition;
        seekPosition.QuadPart = fileSize.QuadPart - bytesToRead;
        SetFilePointerEx(hLogFile, seekPosition, NULL, FILE_BEGIN);
    }

    if (!ReadFile(hLogFile, buffer, bytesToRead, &bytesRead, NULL))
    {
        free(buffer);
        CloseHandle(hLogFile);
        AddEventLogListRow(0, "", "Error", "ProcessWatcher", "", "", "", "", "", "Unable to read the event log.");
        return;
    }

    CloseHandle(hLogFile);
    buffer[bytesRead] = '\0';

    if (fileSize.QuadPart > bytesToRead)
    {
        while (startOffset < bytesRead && buffer[startOffset] != '\n')
            startOffset++;
        if (startOffset < bytesRead)
            startOffset++;
    }

    if (buffer[startOffset] == '\0')
    {
        AddEventLogListRow(0, "", "Info", "ProcessWatcher", "", "", "", "", "", "No log entries yet.");
    }
    else
    {
        char *cursor = buffer + startOffset;
        char *lineContext = NULL;

        for (char *line = strtok_s(cursor, "\r\n", &lineContext);
             line != NULL;
             line = strtok_s(NULL, "\r\n", &lineContext))
        {
            char lineCopy[4096];
            char *fields[EVENT_LOG_COLUMN_COUNT] = {0};

            if (strcpy_s(lineCopy, sizeof(lineCopy), line) != 0)
                continue;
            if (lineCopy[0] == '\0')
                continue;
            if (strcmp(lineCopy, "Timestamp,Event,Target,PID,CPU,Memory,Path,Command Line,Details") == 0)
                continue;

            if (SplitCsvFields(lineCopy, fields, EVENT_LOG_COLUMN_COUNT) == 0)
                continue;

            AddEventLogListRow(rowIndex++,
                               fields[0], fields[1], fields[2], fields[3],
                               fields[4], fields[5], fields[6], fields[7], fields[8]);
        }

        if (rowIndex == 0)
            AddEventLogListRow(0, "", "Info", "ProcessWatcher", "", "", "", "", "", "No log entries yet.");
    }

    free(buffer);

    if (g_AppData.logSortColumn != 0 || !g_AppData.logSortAscending)
    {
        ListView_SortItemsEx(g_AppData.hwndEventLogEdit, CompareEventLogItems,
                             (LPARAM)g_AppData.hwndEventLogEdit);
        UpdateEventLogSortIndicators();
    }
}

void OpenLogFile(HWND hwndOwner)
{
    char logPath[MAX_PATH] = {0};
    HINSTANCE result;

    GetStopLogFilePath(logPath, sizeof(logPath));
    result = ShellExecuteA(hwndOwner, "open", logPath, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
        MessageBoxA(hwndOwner, "Failed to open the log file.", "Open Log File", MB_OK | MB_ICONERROR);
}

void OpenLogFolder(HWND hwndOwner)
{
    char logPath[MAX_PATH] = {0};
    char commandLine[MAX_PATH + 32];
    HINSTANCE result;

    GetStopLogFilePath(logPath, sizeof(logPath));
    sprintf_s(commandLine, sizeof(commandLine), "/select,\"%s\"", logPath);
    result = ShellExecuteA(hwndOwner, "open", "explorer.exe", commandLine, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
        MessageBoxA(hwndOwner, "Failed to open the log folder.", "Open Log Folder", MB_OK | MB_ICONERROR);
}

void LayoutEventLogWindow(HWND hwnd)
{
    RECT clientRect;
    const int margin = 10;

    if (!hwnd || !g_AppData.hwndEventLogEdit || !GetClientRect(hwnd, &clientRect))
        return;

    MoveWindow(g_AppData.hwndEventLogEdit,
               margin, margin,
               (clientRect.right - clientRect.left) - (margin * 2),
               (clientRect.bottom - clientRect.top) - (margin * 2),
               TRUE);
}

void ShowEventLogWindow(HWND hwndOwner)
{
    if (g_AppData.hwndEventLogWindow)
    {
        ShowWindow(g_AppData.hwndEventLogWindow, SW_SHOWNORMAL);
        SetForegroundWindow(g_AppData.hwndEventLogWindow);
        RefreshEventLogView();
        return;
    }

    g_AppData.hwndEventLogWindow = CreateWindowExA(
        WS_EX_APPWINDOW,
        "ProcessWatcherEventLog",
        "ProcessWatcher Event Log",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 520,
        hwndOwner, NULL, GetModuleHandle(NULL), NULL);

    if (!g_AppData.hwndEventLogWindow)
    {
        MessageBoxA(hwndOwner, "Failed to open the Event Log window.",
                    "Event Log Window", MB_OK | MB_ICONERROR);
    }
}

BOOL GetKnownProcessExecutablePath(const WatchedProcess *process, char *processPath, size_t processPathSize)
{
    if (!process || !processPath || processPathSize == 0)
        return FALSE;

    processPath[0] = '\0';

    if (process->running && process->pid != 0 &&
        GetProcessExecutablePathByPid(process->pid, processPath, processPathSize))
    {
        return TRUE;
    }

    return process->executablePath[0] != '\0' &&
           strcpy_s(processPath, processPathSize, process->executablePath) == 0;
}

BOOL TryGetProcessExecutableNameFromSnapshot(DWORD pid, char *processName, size_t processNameSize)
{
    HANDLE hSnapshot;
    PROCESSENTRY32W pe32 = {0};

    if (!processName || processNameSize == 0)
        return FALSE;

    processName[0] = '\0';
    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return FALSE;

    pe32.dwSize = sizeof(pe32);
    if (!Process32FirstW(hSnapshot, &pe32))
    {
        CloseHandle(hSnapshot);
        return FALSE;
    }

    do
    {
        if (pe32.th32ProcessID == pid)
        {
            WideToUtf8(pe32.szExeFile, processName, processNameSize);
            CloseHandle(hSnapshot);
            return processName[0] != '\0';
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return FALSE;
}

BOOL GetProcessExecutableNameByPid(DWORD pid, char *processName, size_t processNameSize)
{
    char processPath[4096];
    char *baseName;

    if (!processName || processNameSize == 0)
        return FALSE;

    processName[0] = '\0';
    if (!GetProcessExecutablePathByPid(pid, processPath, sizeof(processPath)))
        return TryGetProcessExecutableNameFromSnapshot(pid, processName, processNameSize);

    baseName = strrchr(processPath, '\\');
    if (baseName)
        baseName++;
    else
        baseName = processPath;

    if (strcpy_s(processName, processNameSize, baseName) == 0)
        return TRUE;

    processName[0] = '\0';
    return FALSE;
}

double GetProcessCpuPercent(WatchedProcess *process, DWORD pid)
{
    SYSTEM_INFO systemInfo;
    HANDLE hProcess;
    FILETIME creationTime, exitTime, kernelTime, userTime, systemTime;
    ULONGLONG processCpuTime;
    ULONGLONG sampleTime;
    double cpuPercent = 0.0;

    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess)
    {
        ResetProcessCpuSample(process);
        return 0.0;
    }

    if (!GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime, &userTime))
    {
        CloseHandle(hProcess);
        ResetProcessCpuSample(process);
        return 0.0;
    }

    GetSystemInfo(&systemInfo);
    GetSystemTimeAsFileTime(&systemTime);

    processCpuTime = FileTimeToUInt64(kernelTime) + FileTimeToUInt64(userTime);
    sampleTime = FileTimeToUInt64(systemTime);

    if (process->lastSamplePid == pid &&
        process->lastSampleTime > 0 &&
        sampleTime > process->lastSampleTime &&
        systemInfo.dwNumberOfProcessors > 0)
    {
        ULONGLONG cpuDelta = processCpuTime - process->lastCpuTime;
        ULONGLONG timeDelta = sampleTime - process->lastSampleTime;

        cpuPercent = ((double)cpuDelta / (double)timeDelta) * 100.0 / systemInfo.dwNumberOfProcessors;
        if (cpuPercent < 0.0)
            cpuPercent = 0.0;
    }

    process->lastCpuTime = processCpuTime;
    process->lastSampleTime = sampleTime;
    process->lastSamplePid = pid;
    process->cpuPercent = cpuPercent;

    CloseHandle(hProcess);
    return cpuPercent;
}

BOOL SaveSettingsToIni(HWND hwndOwner)
{
    char settingsPath[MAX_PATH] = {0};
    char tempPath[MAX_PATH] = {0};
    char keyName[32];
    char value[64];
    RECT windowRect;
    WINDOWPLACEMENT windowPlacement = {0};
    BOOL autoRefreshEnabled;

    GetSettingsFilePath(settingsPath, sizeof(settingsPath));
    if (!GetTempSettingsFilePath(tempPath, sizeof(tempPath)))
        return FALSE;

    DeleteFileA(tempPath);

    if (!WritePrivateProfileStringA("WatchedProcesses", NULL, NULL, tempPath))
        return FALSE;
    sprintf_s(value, sizeof(value), "%d", g_AppData.count);
    if (!WritePrivateProfileStringA("WatchedProcesses", "Count", value, tempPath))
        return FALSE;

    for (int i = 0; i < g_AppData.count; i++)
    {
        sprintf_s(keyName, sizeof(keyName), "Item%d", i);
        if (!WritePrivateProfileStringA("WatchedProcesses", keyName, g_AppData.processes[i].name, tempPath))
            return FALSE;
        sprintf_s(keyName, sizeof(keyName), "Item%dMode", i);
        sprintf_s(value, sizeof(value), "%d", g_AppData.processes[i].watchMode);
        if (!WritePrivateProfileStringA("WatchedProcesses", keyName, value, tempPath))
            return FALSE;
        sprintf_s(keyName, sizeof(keyName), "Item%dPattern", i);
        if (!WritePrivateProfileStringA("WatchedProcesses", keyName, g_AppData.processes[i].matchPattern, tempPath))
            return FALSE;
    }

    autoRefreshEnabled = IsAutoRefreshEnabled();
    sprintf_s(value, sizeof(value), "%d", autoRefreshEnabled ? 1 : 0);
    if (!WritePrivateProfileStringA("General", "AutoRefresh", value, tempPath))
        return FALSE;
    sprintf_s(value, sizeof(value), "%d", g_AppData.startWithWindows ? 1 : 0);
    if (!WritePrivateProfileStringA("General", "StartWithWindows", value, tempPath))
        return FALSE;
    sprintf_s(value, sizeof(value), "%d", g_AppData.notifyOnStop ? 1 : 0);
    if (!WritePrivateProfileStringA("General", "NotifyOnStop", value, tempPath))
        return FALSE;
    sprintf_s(value, sizeof(value), "%d", g_AppData.createStopLogs ? 1 : 0);
    if (!WritePrivateProfileStringA("General", "CreateStopLogs", value, tempPath))
        return FALSE;
    sprintf_s(value, sizeof(value), "%d", g_AppData.sortColumn);
    if (!WritePrivateProfileStringA("General", "SortColumn", value, tempPath))
        return FALSE;
    sprintf_s(value, sizeof(value), "%d", g_AppData.sortAscending ? 1 : 0);
    if (!WritePrivateProfileStringA("General", "SortAscending", value, tempPath))
        return FALSE;

    if (g_AppData.hwndListView)
    {
        for (int i = 0; i < COLUMN_COUNT; i++)
        {
            sprintf_s(keyName, sizeof(keyName), "ColumnWidth%d", i);
            sprintf_s(value, sizeof(value), "%d", ListView_GetColumnWidth(g_AppData.hwndListView, i));
            if (!WritePrivateProfileStringA("Columns", keyName, value, tempPath))
                return FALSE;
        }
    }

    sprintf_s(value, sizeof(value), "%d", IsWindowTopmost(hwndOwner) ? 1 : 0);
    if (!WritePrivateProfileStringA("Window", "KeepOnTop", value, tempPath))
        return FALSE;
    if (hwndOwner)
    {
        windowPlacement.length = sizeof(windowPlacement);
        if (GetWindowPlacement(hwndOwner, &windowPlacement))
            windowRect = windowPlacement.rcNormalPosition;
        else if (!GetWindowRect(hwndOwner, &windowRect))
            ZeroMemory(&windowRect, sizeof(windowRect));

        sprintf_s(value, sizeof(value), "%ld", windowRect.left);
        if (!WritePrivateProfileStringA("Window", "Left", value, tempPath))
            return FALSE;
        sprintf_s(value, sizeof(value), "%ld", windowRect.top);
        if (!WritePrivateProfileStringA("Window", "Top", value, tempPath))
            return FALSE;
        sprintf_s(value, sizeof(value), "%ld", windowRect.right - windowRect.left);
        if (!WritePrivateProfileStringA("Window", "Width", value, tempPath))
            return FALSE;
        sprintf_s(value, sizeof(value), "%ld", windowRect.bottom - windowRect.top);
        if (!WritePrivateProfileStringA("Window", "Height", value, tempPath))
            return FALSE;
    }

    if (!MoveFileExA(tempPath, settingsPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
    {
        DeleteFileA(tempPath);
        return FALSE;
    }

    return TRUE;
}

BOOL SetStartWithWindowsEnabled(BOOL enabled)
{
    HKEY hKey = NULL;
    char executablePath[MAX_PATH] = {0};
    char quotedPath[MAX_PATH + 2];
    LONG result;

    {
        WCHAR *wideRunPath = Utf8ToWideAlloc(RUN_REGISTRY_PATH);
        result = wideRunPath ? RegCreateKeyExW(HKEY_CURRENT_USER, wideRunPath, 0, NULL, 0,
                                               KEY_SET_VALUE, NULL, &hKey, NULL)
                             : ERROR_NOT_ENOUGH_MEMORY;
        free(wideRunPath);
    }
    if (result != ERROR_SUCCESS)
        return FALSE;

    if (enabled)
    {
        if (GetModuleFileNameA(NULL, executablePath, sizeof(executablePath)) == 0)
        {
            RegCloseKey(hKey);
            return FALSE;
        }

        sprintf_s(quotedPath, sizeof(quotedPath), "\"%s\"", executablePath);
        {
            WCHAR *wideValueName = Utf8ToWideAlloc(RUN_REGISTRY_VALUE);
            WCHAR *wideQuotedPath = Utf8ToWideAlloc(quotedPath);

            if (!wideValueName || !wideQuotedPath)
                result = ERROR_NOT_ENOUGH_MEMORY;
            else
                result = RegSetValueExW(hKey, wideValueName, 0, REG_SZ,
                                        (const BYTE *)wideQuotedPath,
                                        (DWORD)((wcslen(wideQuotedPath) + 1) * sizeof(WCHAR)));

            free(wideValueName);
            free(wideQuotedPath);
        }
    }
    else
    {
        WCHAR *wideValueName = Utf8ToWideAlloc(RUN_REGISTRY_VALUE);
        result = wideValueName ? RegDeleteValueW(hKey, wideValueName) : ERROR_NOT_ENOUGH_MEMORY;
        free(wideValueName);
        if (result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

BOOL IsStartWithWindowsEnabled(void)
{
    HKEY hKey = NULL;
    char executablePath[MAX_PATH] = {0};
    char expectedValue[MAX_PATH + 2];
    char actualValue[1024] = {0};
    DWORD valueType = 0;
    LONG result;
    BOOL isEnabled = FALSE;

    if (GetModuleFileNameA(NULL, executablePath, sizeof(executablePath)) == 0)
        return FALSE;

    sprintf_s(expectedValue, sizeof(expectedValue), "\"%s\"", executablePath);

    {
        WCHAR *wideRunPath = Utf8ToWideAlloc(RUN_REGISTRY_PATH);
        result = wideRunPath ? RegOpenKeyExW(HKEY_CURRENT_USER, wideRunPath, 0, KEY_QUERY_VALUE, &hKey)
                             : ERROR_NOT_ENOUGH_MEMORY;
        free(wideRunPath);
    }
    if (result != ERROR_SUCCESS)
        return FALSE;

    {
        WCHAR *wideValueName = Utf8ToWideAlloc(RUN_REGISTRY_VALUE);
        WCHAR wideActualValue[1024] = {0};
        DWORD wideValueSize = sizeof(wideActualValue);

        result = wideValueName ? RegQueryValueExW(hKey, wideValueName, NULL, &valueType,
                                                  (LPBYTE)wideActualValue, &wideValueSize)
                               : ERROR_NOT_ENOUGH_MEMORY;
        if (result == ERROR_SUCCESS && valueType == REG_SZ)
            WideToUtf8(wideActualValue, actualValue, sizeof(actualValue));
        free(wideValueName);
    }
    if (result == ERROR_SUCCESS && valueType == REG_SZ)
        isEnabled = CompareUtf8Insensitive(actualValue, expectedValue) == 0;

    RegCloseKey(hKey);
    return isEnabled;
}

void ShowSettingsSaveError(HWND hwndOwner)
{
    char settingsPath[MAX_PATH] = {0};
    char message[512];

    GetSettingsFilePath(settingsPath, sizeof(settingsPath));
    sprintf_s(message, sizeof(message),
              "Unable to save settings or update startup registration.\n\nSettings file:\n%s",
              settingsPath);
    MessageBoxA(hwndOwner, message, "Save Settings Failed", MB_OK | MB_ICONERROR);
}

void SaveSettingsWithFeedback(HWND hwndOwner)
{
    if (!SetStartWithWindowsEnabled(g_AppData.startWithWindows))
    {
        g_AppData.startWithWindows = IsStartWithWindowsEnabled();
        UpdateOptionsMenuState(hwndOwner);
        ShowSettingsSaveError(hwndOwner);
        return;
    }

    g_AppData.startWithWindows = IsStartWithWindowsEnabled();
    if (!SaveSettingsToIni(hwndOwner))
        ShowSettingsSaveError(hwndOwner);
}

void LoadSettingsFromIni(HWND hwndOwner)
{
    char settingsPath[MAX_PATH] = {0};
    char value[64];
    UINT count;
    int windowLeft = 0;
    int windowTop = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    BOOL hasWindowLeft = FALSE;
    BOOL hasWindowTop = FALSE;

    GetSettingsFilePath(settingsPath, sizeof(settingsPath));
    g_AppData.count = 0;
    g_AppData.autoRefreshEnabled = GetPrivateProfileIntA("General", "AutoRefresh", 1, settingsPath) != 0;
    g_AppData.startWithWindows = IsStartWithWindowsEnabled();
    g_AppData.notifyOnStop = GetPrivateProfileIntA("General", "NotifyOnStop", 0, settingsPath) != 0;
    g_AppData.createStopLogs = GetPrivateProfileIntA("General", "CreateStopLogs", 1, settingsPath) != 0;
    g_AppData.sortColumn = GetPrivateProfileIntA("General", "SortColumn", 0, settingsPath);
    if (g_AppData.sortColumn < 0 || g_AppData.sortColumn >= COLUMN_COUNT)
        g_AppData.sortColumn = 0;
    g_AppData.sortAscending = GetPrivateProfileIntA("General", "SortAscending", 1, settingsPath) != 0;
    count = GetPrivateProfileIntA("WatchedProcesses", "Count", 0, settingsPath);

    for (UINT i = 0; i < count && g_AppData.count < MAX_PROCESSES; i++)
    {
        char keyName[32];
        char processName[256] = {0};
        char patternValue[1024] = {0};
        char displayName[256] = {0};
        int watchMode;

        sprintf_s(keyName, sizeof(keyName), "Item%u", i);
        GetPrivateProfileStringA("WatchedProcesses", keyName, "",
                                 processName, (DWORD)sizeof(processName), settingsPath);
        sprintf_s(keyName, sizeof(keyName), "Item%uMode", i);
        watchMode = GetPrivateProfileIntA("WatchedProcesses", keyName, WATCH_MODE_PROCESS_NAME, settingsPath);
        sprintf_s(keyName, sizeof(keyName), "Item%uPattern", i);
        GetPrivateProfileStringA("WatchedProcesses", keyName, "",
                                 patternValue, (DWORD)sizeof(patternValue), settingsPath);

        if (patternValue[0] == '\0')
        {
            if (!ParseWatchInput(processName, &watchMode, patternValue, sizeof(patternValue),
                                 displayName, sizeof(displayName)))
            {
                continue;
            }
        }
        else
        {
            if (watchMode == WATCH_MODE_COMMAND_LINE)
            {
                if (sprintf_s(displayName, sizeof(displayName), "cmd:%s", patternValue) <= 0)
                    continue;
            }
            else
            {
                if (strcpy_s(displayName, sizeof(displayName), patternValue) != 0)
                    continue;
            }
        }

        if (displayName[0] != '\0' && !IsDuplicateWatchEntry(watchMode, patternValue))
        {
            g_AppData.processes[g_AppData.count].watchMode = watchMode;
            strcpy_s(g_AppData.processes[g_AppData.count].name,
                     sizeof(g_AppData.processes[0].name), displayName);
            strcpy_s(g_AppData.processes[g_AppData.count].matchPattern,
                     sizeof(g_AppData.processes[0].matchPattern), patternValue);
            g_AppData.count++;
        }
    }

    for (int i = 0; i < COLUMN_COUNT; i++)
    {
        char keyName[32];
        sprintf_s(keyName, sizeof(keyName), "ColumnWidth%d", i);
        g_AppData.savedColumnWidths[i] = GetPrivateProfileIntA("Columns", keyName, 0, settingsPath);
    }

    if (GetPrivateProfileStringA("Window", "Left", "", value, sizeof(value), settingsPath) > 0)
    {
        windowLeft = atoi(value);
        hasWindowLeft = TRUE;
    }
    if (GetPrivateProfileStringA("Window", "Top", "", value, sizeof(value), settingsPath) > 0)
    {
        windowTop = atoi(value);
        hasWindowTop = TRUE;
    }
    windowWidth = GetPrivateProfileIntA("Window", "Width", 0, settingsPath);
    windowHeight = GetPrivateProfileIntA("Window", "Height", 0, settingsPath);
    if (windowWidth > 0 && windowHeight > 0 && hasWindowLeft && hasWindowTop)
    {
        g_AppData.savedWindowRect.left = windowLeft;
        g_AppData.savedWindowRect.top = windowTop;
        g_AppData.savedWindowRect.right = windowLeft + windowWidth;
        g_AppData.savedWindowRect.bottom = windowTop + windowHeight;
        g_AppData.hasSavedWindowRect = TRUE;
    }
    else
    {
        g_AppData.hasSavedWindowRect = FALSE;
    }

    if (GetPrivateProfileIntA("Window", "KeepOnTop", 0, settingsPath) != 0)
        ToggleWatcherTopmost(hwndOwner, TRUE);
}

int CompareInts(int left, int right)
{
    if (left < right)
        return -1;
    if (left > right)
        return 1;
    return 0;
}

int CompareUInt32(DWORD left, DWORD right)
{
    if (left < right)
        return -1;
    if (left > right)
        return 1;
    return 0;
}

int CompareDoubleValues(double left, double right)
{
    if (left < right)
        return -1;
    if (left > right)
        return 1;
    return 0;
}

int CompareLastSeenValues(const WatchedProcess *left, const WatchedProcess *right)
{
    ULONGLONG leftValue;
    ULONGLONG rightValue;

    if (!left->hasLastSeen && !right->hasLastSeen)
        return 0;
    if (!left->hasLastSeen)
        return -1;
    if (!right->hasLastSeen)
        return 1;

    leftValue = SystemTimeToUInt64(left->lastSeenLocalTime);
    rightValue = SystemTimeToUInt64(right->lastSeenLocalTime);

    if (leftValue < rightValue)
        return -1;
    if (leftValue > rightValue)
        return 1;
    return 0;
}

int __cdecl CompareWatchedProcesses(const void *left, const void *right)
{
    const WatchedProcess *processLeft = (const WatchedProcess *)left;
    const WatchedProcess *processRight = (const WatchedProcess *)right;
    int result;

    switch (g_AppData.sortColumn)
    {
    case 1:
        result = CompareInts(processLeft->running, processRight->running);
        break;

    case 2:
        result = CompareUInt32(processLeft->pid, processRight->pid);
        break;

    case 3:
        result = CompareDoubleValues(processLeft->cpuPercent, processRight->cpuPercent);
        break;

    case 4:
        result = CompareUInt32(processLeft->memoryMB, processRight->memoryMB);
        break;

    case 5:
        result = CompareLastSeenValues(processLeft, processRight);
        break;

    case 0:
    default:
        result = CompareUtf8Insensitive(processLeft->name, processRight->name);
        break;
    }

    if (result == 0)
        result = CompareUtf8Insensitive(processLeft->name, processRight->name);
    if (result == 0)
        result = CompareUInt32(processLeft->pid, processRight->pid);

    if (!g_AppData.sortAscending)
        result = -result;

    return result;
}

void SortWatchedProcesses(void)
{
    if (g_AppData.count > 1)
    {
        qsort(g_AppData.processes, (size_t)g_AppData.count,
              sizeof(g_AppData.processes[0]), CompareWatchedProcesses);
    }
}

void ApplySavedWindowPlacement(HWND hwnd)
{
    if (!hwnd || !g_AppData.hasSavedWindowRect)
        return;

    SetWindowPos(hwnd, NULL,
                 g_AppData.savedWindowRect.left,
                 g_AppData.savedWindowRect.top,
                 g_AppData.savedWindowRect.right - g_AppData.savedWindowRect.left,
                 g_AppData.savedWindowRect.bottom - g_AppData.savedWindowRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplyAutoRefreshTimer(HWND hwnd)
{
    if (!hwnd)
        return;

    KillTimer(hwnd, ID_REFRESH_TIMER);
    if (IsAutoRefreshEnabled())
        SetTimer(hwnd, ID_REFRESH_TIMER, DEFAULT_REFRESH_INTERVAL_MS, NULL);
}

HMENU CreateMainWindowMenu(void)
{
    HMENU hMenuBar = CreateMenu();
    HMENU hFileMenu;
    HMENU hLogMenu;
    HMENU hOptionsMenu;
    HMENU hHelpMenu;

    if (!hMenuBar)
        return NULL;

    hFileMenu = CreatePopupMenu();
    if (!hFileMenu)
    {
        DestroyMenu(hMenuBar);
        return NULL;
    }

    hLogMenu = CreatePopupMenu();
    if (!hLogMenu)
    {
        DestroyMenu(hFileMenu);
        DestroyMenu(hMenuBar);
        return NULL;
    }

    hOptionsMenu = CreatePopupMenu();
    if (!hOptionsMenu)
    {
        DestroyMenu(hFileMenu);
        DestroyMenu(hLogMenu);
        DestroyMenu(hMenuBar);
        return NULL;
    }

    hHelpMenu = CreatePopupMenu();
    if (!hHelpMenu)
    {
        DestroyMenu(hFileMenu);
        DestroyMenu(hLogMenu);
        DestroyMenu(hOptionsMenu);
        DestroyMenu(hMenuBar);
        return NULL;
    }

    AppendMenu(hFileMenu, MF_STRING, ID_FILE_LAUNCH_NEW_PROCESS, TEXT("Start New Process..."));
    AppendMenu(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_PROCESS_BROWSER, TEXT("Process Browser..."));
    AppendMenu(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hFileMenu, MF_STRING, ID_FORCE_REFRESH, TEXT("Refresh Now\tF5"));
    AppendMenu(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_EXIT, TEXT("Exit"));
    AppendMenu(hLogMenu, MF_STRING, ID_FILE_OPEN_LOG, TEXT("Open Log File"));
    AppendMenu(hLogMenu, MF_STRING, ID_FILE_OPEN_LOG_FOLDER, TEXT("Open Log Folder"));
    AppendMenu(hLogMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hLogMenu, MF_STRING, ID_FILE_OPEN_EVENT_LOG_WINDOW, TEXT("Event Log Window"));
    AppendMenu(hLogMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hLogMenu, MF_STRING, ID_OPTIONS_CREATE_STOP_LOGS, TEXT("Enable Logging"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_OPTIONS_AUTO_REFRESH, TEXT("Auto-Refresh"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_OPTIONS_START_WITH_WINDOWS, TEXT("Start with Windows"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_OPTIONS_NOTIFY_ON_STOP, TEXT("Notify on Stop"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_CONTEXT_TOGGLE_TOPMOST, TEXT("Keep On Top"));
    AppendMenu(hHelpMenu, MF_STRING, ID_HELP_WATCH_SYNTAX, TEXT("Watch Syntax..."));

    if (!AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, TEXT("File")) ||
        !AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hLogMenu, TEXT("Log")) ||
        !AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hOptionsMenu, TEXT("Options")) ||
        !AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hHelpMenu, TEXT("Help")))
    {
        DestroyMenu(hFileMenu);
        DestroyMenu(hLogMenu);
        DestroyMenu(hOptionsMenu);
        DestroyMenu(hHelpMenu);
        DestroyMenu(hMenuBar);
        return NULL;
    }

    return hMenuBar;
}

void UpdateOptionsMenuState(HWND hwnd)
{
    HMENU hMenu;

    if (!hwnd)
        return;

    hMenu = GetMenu(hwnd);
    if (!hMenu)
        return;

    CheckMenuItem(hMenu, ID_OPTIONS_AUTO_REFRESH,
                  MF_BYCOMMAND | (g_AppData.autoRefreshEnabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, ID_OPTIONS_START_WITH_WINDOWS,
                  MF_BYCOMMAND | (g_AppData.startWithWindows ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, ID_OPTIONS_NOTIFY_ON_STOP,
                  MF_BYCOMMAND | (g_AppData.notifyOnStop ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, ID_OPTIONS_CREATE_STOP_LOGS,
                  MF_BYCOMMAND | (g_AppData.createStopLogs ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, ID_CONTEXT_TOGGLE_TOPMOST,
                  MF_BYCOMMAND | (IsWindowTopmost(hwnd) ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(hMenu, ID_FORCE_REFRESH,
                   MF_BYCOMMAND | (g_AppData.autoRefreshEnabled ? MF_GRAYED : MF_ENABLED));
    DrawMenuBar(hwnd);
}

void UpdateListSortIndicators(void)
{
    HWND hwndHeader;

    if (!g_AppData.hwndListView)
        return;

    hwndHeader = ListView_GetHeader(g_AppData.hwndListView);
    if (!hwndHeader)
        return;

    for (int i = 0; i < COLUMN_COUNT; i++)
    {
        HDITEM item = {0};

        item.mask = HDI_FORMAT;
        if (!Header_GetItem(hwndHeader, i, &item))
            continue;

        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_AppData.sortColumn)
            item.fmt |= g_AppData.sortAscending ? HDF_SORTUP : HDF_SORTDOWN;

        Header_SetItem(hwndHeader, i, &item);
    }
}

BOOL EnsureNotificationIcon(HWND hwnd)
{
    NOTIFYICONDATAA nid = {0};
    HICON hIcon;

    if (!hwnd)
        return FALSE;
    if (g_AppData.notificationIconAdded)
        return TRUE;

    hIcon = (HICON)SendMessage(hwnd, WM_GETICON, ICON_SMALL, 0);
    if (!hIcon)
        hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
                                 IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!hIcon)
        hIcon = LoadIcon(NULL, IDI_APPLICATION);

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = NOTIFICATION_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_STATE;
    nid.hIcon = hIcon;
    nid.dwState = NIS_HIDDEN;
    nid.dwStateMask = NIS_HIDDEN;
    strcpy_s(nid.szTip, sizeof(nid.szTip), "ProcessWatcher");

    if (!ShellNotifyIconUtf8(NIM_ADD, &nid))
        return FALSE;

    nid.uVersion = NOTIFYICON_VERSION_4;
    ShellNotifyIconUtf8(NIM_SETVERSION, &nid);
    g_AppData.notificationIconAdded = TRUE;
    return TRUE;
}

void RemoveNotificationIcon(HWND hwnd)
{
    NOTIFYICONDATAA nid = {0};

    if (!hwnd || !g_AppData.notificationIconAdded)
        return;

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = NOTIFICATION_ICON_ID;
    ShellNotifyIconUtf8(NIM_DELETE, &nid);
    g_AppData.notificationIconAdded = FALSE;
}

void ShowStopNotification(HWND hwnd, const char *message)
{
    NOTIFYICONDATAA nid = {0};

    if (!hwnd || !message || message[0] == '\0')
        return;
    if (!EnsureNotificationIcon(hwnd))
        return;

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = NOTIFICATION_ICON_ID;
    nid.uFlags = NIF_INFO;
    strcpy_s(nid.szInfoTitle, sizeof(nid.szInfoTitle), "Process Stopped");
    strcpy_s(nid.szInfo, sizeof(nid.szInfo), message);
    nid.dwInfoFlags = NIIF_WARNING;
    nid.uTimeout = 5000;
    ShellNotifyIconUtf8(NIM_MODIFY, &nid);
}

BOOL GetWindowPickerTarget(DWORD *pid, char *processName, size_t processNameSize)
{
    POINT cursorPos;
    HWND hwndAtPoint;
    DWORD targetPid = 0;

    if (!pid || !processName || processNameSize == 0)
        return FALSE;

    *pid = 0;
    processName[0] = '\0';

    if (!GetCursorPos(&cursorPos))
        return FALSE;

    hwndAtPoint = WindowFromPoint(cursorPos);
    if (!hwndAtPoint)
        return FALSE;

    hwndAtPoint = GetAncestor(hwndAtPoint, GA_ROOT);
    if (!hwndAtPoint)
        return FALSE;

    GetWindowThreadProcessId(hwndAtPoint, &targetPid);
    if (targetPid == 0 || targetPid == GetCurrentProcessId())
        return FALSE;

    if (!GetProcessExecutableNameByPid(targetPid, processName, processNameSize))
        return FALSE;

    *pid = targetPid;
    return TRUE;
}

void UpdateWindowPickerTarget(void)
{
    DWORD pid = 0;
    char processName[256] = {0};
    BOOL hasTarget;

    if (!g_AppData.windowPickerActive)
        return;

    hasTarget = GetWindowPickerTarget(&pid, processName, sizeof(processName));
    if (!hasTarget)
    {
        pid = 0;
        processName[0] = '\0';
    }

    if (g_AppData.windowPickerPid == pid &&
        CompareUtf8Insensitive(g_AppData.windowPickerProcessName, processName) == 0)
    {
        return;
    }

    g_AppData.windowPickerPid = pid;
    strcpy_s(g_AppData.windowPickerProcessName, sizeof(g_AppData.windowPickerProcessName), processName);
    UpdateStatusBar();
}

BOOL IsWindowPickerConfirmationPressed(void)
{
    return ((GetAsyncKeyState(VK_RETURN) & 0x0001) != 0) ||
           ((GetAsyncKeyState(VK_SPACE) & 0x0001) != 0) ||
           ((GetAsyncKeyState(VK_LBUTTON) & 0x0001) != 0);
}

void HandleWindowPickerTimer(HWND hwndOwner)
{
    char pickedProcessName[256] = {0};

    /* The picker is driven entirely from the UI thread timer so it can
       follow the cursor without introducing a worker thread. */
    UpdateWindowPickerTarget();

    if (GetAsyncKeyState(VK_ESCAPE) & 0x0001)
    {
        ToggleWindowPicker(hwndOwner);
        return;
    }

    if (g_AppData.windowPickerAwaitingButtonRelease)
    {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
            return;

        g_AppData.windowPickerAwaitingButtonRelease = FALSE;
    }

    if (!IsWindowPickerConfirmationPressed() ||
        g_AppData.windowPickerPid == 0 ||
        g_AppData.windowPickerProcessName[0] == '\0')
    {
        return;
    }

    strcpy_s(pickedProcessName, sizeof(pickedProcessName), g_AppData.windowPickerProcessName);
    ToggleWindowPicker(hwndOwner);
    AddProcess(pickedProcessName);
}

void ToggleWindowPicker(HWND hwndOwner)
{
    if (!hwndOwner)
        return;

    if (g_AppData.windowPickerActive)
    {
        KillTimer(hwndOwner, ID_WINDOW_PICKER_TIMER);
        g_AppData.windowPickerActive = FALSE;
        g_AppData.windowPickerAwaitingButtonRelease = FALSE;
        g_AppData.windowPickerPid = 0;
        g_AppData.windowPickerProcessName[0] = '\0';
        if (g_AppData.hwndPickWindowButton)
            SetWindowTextA(g_AppData.hwndPickWindowButton, "Pick Window");
        UpdateStatusBar();
        return;
    }

    g_AppData.windowPickerActive = TRUE;
    g_AppData.windowPickerAwaitingButtonRelease = TRUE;
    g_AppData.windowPickerPid = 0;
    g_AppData.windowPickerProcessName[0] = '\0';
    if (g_AppData.hwndPickWindowButton)
        SetWindowTextA(g_AppData.hwndPickWindowButton, "Cancel Pick");
    UpdateWindowPickerTarget();
    UpdateStatusBar();
    SetTimer(hwndOwner, ID_WINDOW_PICKER_TIMER, WINDOW_PICKER_TIMER_MS, NULL);
}

void UpdateStatusBar(void)
{
    char statusText[512];
    int runningCount = 0;

    if (!g_AppData.hwndStatusBar)
        return;

    if (g_AppData.windowPickerActive)
    {
        if (g_AppData.windowPickerPid != 0 && g_AppData.windowPickerProcessName[0] != '\0')
        {
            sprintf_s(statusText, sizeof(statusText),
                      "Pick Window: hover target, then press Enter/Space or click to add  Target: %s (PID %lu)  Esc cancels",
                      g_AppData.windowPickerProcessName, g_AppData.windowPickerPid);
        }
        else
        {
            strcpy_s(statusText, sizeof(statusText),
                     "Pick Window: move the cursor over another app window, then press Enter/Space or click to add  Esc cancels");
        }
        StatusBarSetTextUtf8(g_AppData.hwndStatusBar, 0, statusText);
        return;
    }

    for (int i = 0; i < g_AppData.count; i++)
    {
        if (g_AppData.processes[i].running)
            runningCount++;
    }

    sprintf_s(statusText, sizeof(statusText),
              "Watching: %d  Running: %d  Refresh: %s",
              g_AppData.count, runningCount,
              IsAutoRefreshEnabled() ? "Auto" : "Paused");
    StatusBarSetTextUtf8(g_AppData.hwndStatusBar, 0, statusText);
}

void UpdateRemoveButtonState(void)
{
    if (!g_AppData.hwndRemoveButton)
        return;

    EnableWindow(g_AppData.hwndRemoveButton,
                 g_AppData.hwndListView &&
                     SendMessage(g_AppData.hwndListView, LVM_GETNEXTITEM, -1, LVNI_SELECTED) != -1);
}

BOOL GetSelectedProcessName(char *processName, size_t processNameSize)
{
    int index;

    if (!processName || processNameSize == 0 || !g_AppData.hwndListView)
        return FALSE;

    index = (int)SendMessage(g_AppData.hwndListView, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
    if (index == -1 || index >= g_AppData.count)
        return FALSE;

    return strcpy_s(processName, processNameSize, g_AppData.processes[index].name) == 0;
}

void RestoreSelectedProcess(HWND hwndListView, const char *processName)
{
    if (!hwndListView || !processName || processName[0] == '\0')
        return;

    for (int i = 0; i < g_AppData.count; i++)
    {
        if (CompareUtf8Insensitive(g_AppData.processes[i].name, processName) == 0)
        {
            ListView_SetItemState(hwndListView, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetSelectionMark(hwndListView, i);
            ListView_EnsureVisible(hwndListView, i, FALSE);
            break;
        }
    }
}

void RecordStoppedProcessEvent(const WatchedProcess *process, int *stoppedCount,
                              char *firstStoppedProcess, size_t firstStoppedProcessSize,
                              char *firstStopReason, size_t firstStopReasonSize)
{
    if (!process || !stoppedCount || !firstStoppedProcess || firstStoppedProcessSize == 0 ||
        !firstStopReason || firstStopReasonSize == 0)
    {
        return;
    }

    WriteProcessStopLogEntry(process);
    if (*stoppedCount == 0)
    {
        strcpy_s(firstStoppedProcess, firstStoppedProcessSize, process->name);
        FormatStopReasonSummary(process, firstStopReason, firstStopReasonSize);
    }

    (*stoppedCount)++;
}

void UpdateRunningProcessSnapshot(WatchedProcess *process, DWORD pid, BOOL shouldLogStart)
{
    char processPath[MAX_PATH] = {0};

    if (!process || pid == 0)
        return;

    EnsureTrackedProcessHandle(process, pid);
    process->cpuPercent = GetProcessCpuPercent(process, pid);
    UpdateProcessLastSeen(process);

    if (GetProcessExecutablePathByPid(pid, processPath, sizeof(processPath)))
        strcpy_s(process->executablePath, sizeof(process->executablePath), processPath);

    if (GetProcessCommandLineByPid(pid, process->commandLine, sizeof(process->commandLine)))
    {
        /* Keep the last successful command line snapshot for stop logging. */
    }

    if (shouldLogStart)
        WriteProcessStartLogEntry(process);
}

void AddProcessListRow(HWND hwndListView, int rowIndex, const WatchedProcess *process)
{
    char statusText[32];
    char pidText[32];
    char cpuText[32];
    char memoryText[32];
    char lastSeenText[32];
    int index;

    if (!hwndListView || !process)
        return;

    if (process->running)
    {
        strcpy_s(statusText, sizeof(statusText), "RUNNING");
        sprintf_s(pidText, sizeof(pidText), "%lu", process->pid);
        sprintf_s(cpuText, sizeof(cpuText), "%.1f%%", process->cpuPercent);
        sprintf_s(memoryText, sizeof(memoryText), "%lu MB", process->memoryMB);
    }
    else
    {
        strcpy_s(statusText, sizeof(statusText), "STOPPED");
        strcpy_s(pidText, sizeof(pidText), "-");
        strcpy_s(cpuText, sizeof(cpuText), "-");
        strcpy_s(memoryText, sizeof(memoryText), "-");
    }

    FormatProcessLastSeen(process, lastSeenText, sizeof(lastSeenText));

    index = ListViewInsertItemUtf8(hwndListView, rowIndex, process->name, process->running ? 1 : 0);
    if (index == -1)
        return;

    ListViewSetItemTextUtf8(hwndListView, index, 1, statusText);
    ListViewSetItemTextUtf8(hwndListView, index, 2, pidText);
    ListViewSetItemTextUtf8(hwndListView, index, 3, cpuText);
    ListViewSetItemTextUtf8(hwndListView, index, 4, memoryText);
    ListViewSetItemTextUtf8(hwndListView, index, 5, lastSeenText);
}

void RefreshProcessList(HWND hwndListView)
{
    char selectedProcessName[256] = {0};
    int stoppedCount = 0;
    char firstStoppedProcess[256] = {0};
    char firstStopReason[256] = {0};
    HWND hwndOwner;

    if (!hwndListView)
        return;

    hwndOwner = GetParent(hwndListView);
    GetSelectedProcessName(selectedProcessName, sizeof(selectedProcessName));

    SendMessage(hwndListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hwndListView);

    /* Refreshing does three distinct jobs: update the in-memory snapshot,
       emit start/stop events, then rebuild the visible list view rows. */
    for (int i = 0; i < g_AppData.count; i++)
    {
        WatchedProcess previousProcess = g_AppData.processes[i];
        DWORD pid = 0;
        DWORD memoryMB = 0;
        BOOL wasRunning = previousProcess.running;
        BOOL running = IsWatchedProcessRunning(&g_AppData.processes[i], &pid, &memoryMB);
        BOOL pidChanged = wasRunning && running && previousProcess.pid != 0 && pid != 0 && previousProcess.pid != pid;

        if (pidChanged)
        {
            CloseTrackedProcessHandle(&g_AppData.processes[i]);
            CaptureTrackedProcessStopReason(&previousProcess);
            RecordStoppedProcessEvent(&previousProcess, &stoppedCount,
                                      firstStoppedProcess, sizeof(firstStoppedProcess),
                                      firstStopReason, sizeof(firstStopReason));
            ResetProcessCpuSample(&g_AppData.processes[i]);
        }

        g_AppData.processes[i].running = running;
        g_AppData.processes[i].pid = pid;
        g_AppData.processes[i].memoryMB = memoryMB;

        if (running)
        {
            BOOL shouldLogStart = !wasRunning || pidChanged;

            UpdateRunningProcessSnapshot(&g_AppData.processes[i], pid, shouldLogStart);
        }
        else
        {
            if (wasRunning)
            {
                CloseTrackedProcessHandle(&g_AppData.processes[i]);
                CaptureTrackedProcessStopReason(&previousProcess);
                RecordStoppedProcessEvent(&previousProcess, &stoppedCount,
                                          firstStoppedProcess, sizeof(firstStoppedProcess),
                                          firstStopReason, sizeof(firstStopReason));
            }
            else
            {
                CloseTrackedProcessHandle(&g_AppData.processes[i]);
            }
            ResetProcessCpuSample(&g_AppData.processes[i]);
            g_AppData.processes[i].cpuPercent = 0.0;
        }
    }

    SortWatchedProcesses();

    for (int i = 0; i < g_AppData.count; i++)
        AddProcessListRow(hwndListView, i, &g_AppData.processes[i]);

    RestoreSelectedProcess(hwndListView, selectedProcessName);

    SendMessage(hwndListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwndListView, NULL, TRUE);
    UpdateWindow(hwndListView);
    GetLocalTime(&g_AppData.lastRefreshLocalTime);
    g_AppData.hasLastRefresh = TRUE;
    UpdateRemoveButtonState();
    UpdateStatusBar();
    if (g_AppData.notifyOnStop && stoppedCount > 0)
    {
        char notificationMessage[256];

        if (stoppedCount == 1)
        {
            sprintf_s(notificationMessage, sizeof(notificationMessage),
                      "%s stopped: %s", firstStoppedProcess,
                      firstStopReason[0] != '\0' ? firstStopReason : "reason unavailable");
        }
        else
        {
            sprintf_s(notificationMessage, sizeof(notificationMessage),
                      "%s and %d other watched process%s stopped.",
                      firstStoppedProcess, stoppedCount - 1,
                      stoppedCount == 2 ? "" : "es");
        }

        ShowStopNotification(hwndOwner, notificationMessage);
    }
}

void AddProcess(const char *processName)
{
    char displayName[256] = {0};
    char matchPattern[1024] = {0};
    int watchMode = WATCH_MODE_PROCESS_NAME;

    if (g_AppData.count >= MAX_PROCESSES)
    {
        MessageBox(NULL, "Maximum number of processes reached!", "Limit", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!ParseWatchInput(processName, &watchMode, matchPattern, sizeof(matchPattern),
                         displayName, sizeof(displayName)))
        return;

    if (IsDuplicateWatchEntry(watchMode, matchPattern))
    {
        MessageBox(NULL, "This watch entry is already being watched!", "Duplicate", MB_OK | MB_ICONWARNING);
        return;
    }

    strcpy_s(g_AppData.processes[g_AppData.count].name,
             sizeof(g_AppData.processes[0].name), displayName);
    strcpy_s(g_AppData.processes[g_AppData.count].matchPattern,
             sizeof(g_AppData.processes[0].matchPattern), matchPattern);
    g_AppData.processes[g_AppData.count].watchMode = watchMode;
    WriteWatchListChangeLogEntry(&g_AppData.processes[g_AppData.count], "ADDED");
    g_AppData.count++;

    SaveSettingsWithFeedback(GetParent(g_AppData.hwndListView));
    RefreshProcessList(g_AppData.hwndListView);
}

void RemoveProcess(int index)
{
    WatchedProcess removedProcess;

    if (index < 0 || index >= g_AppData.count)
        return;

    removedProcess = g_AppData.processes[index];
    WriteWatchListChangeLogEntry(&removedProcess, "REMOVED");
    CloseTrackedProcessHandle(&g_AppData.processes[index]);

    for (int i = index; i < g_AppData.count - 1; i++)
        g_AppData.processes[i] = g_AppData.processes[i + 1];

    g_AppData.count--;
    ZeroMemory(&g_AppData.processes[g_AppData.count], sizeof(g_AppData.processes[g_AppData.count]));

    SaveSettingsWithFeedback(GetParent(g_AppData.hwndListView));
    RefreshProcessList(g_AppData.hwndListView);
}

int GetSelectedProcessIndex(void)
{
    if (!g_AppData.hwndListView)
        return -1;

    return (int)SendMessage(g_AppData.hwndListView, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
}

void RemoveSelectedProcess(void)
{
    int index = GetSelectedProcessIndex();
    if (index != -1)
        RemoveProcess(index);
}

BOOL IsWindowTopmost(HWND hwnd)
{
    return hwnd && (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

void ToggleWatcherTopmost(HWND hwndOwner, BOOL makeTopmost)
{
    if (!hwndOwner)
        return;

    SetWindowPos(hwndOwner,
                 makeTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
}

void ShowGlobalContextMenu(HWND hwndOwner, POINT screenPoint)
{
    HMENU hMenu;
    BOOL watcherTopmost;
    int command;

    if (!hwndOwner)
        return;

    if (screenPoint.x == -1 && screenPoint.y == -1)
    {
        RECT windowRect;

        if (!GetWindowRect(hwndOwner, &windowRect))
            return;

        screenPoint.x = windowRect.left + ((windowRect.right - windowRect.left) / 2);
        screenPoint.y = windowRect.top + ((windowRect.bottom - windowRect.top) / 2);
    }

    watcherTopmost = IsWindowTopmost(hwndOwner);
    hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    AppendMenu(hMenu, MF_STRING | (watcherTopmost ? MF_CHECKED : MF_UNCHECKED), ID_CONTEXT_TOGGLE_TOPMOST,
               TEXT("Keep On Top"));
    command = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y,
                             0, hwndOwner, NULL);
    DestroyMenu(hMenu);

    if (command == ID_CONTEXT_TOGGLE_TOPMOST)
    {
        ToggleWatcherTopmost(hwndOwner, !watcherTopmost);
        UpdateOptionsMenuState(hwndOwner);
        SaveSettingsWithFeedback(hwndOwner);
    }
}

void EndSelectedProcess(HWND hwndOwner)
{
    int index = GetSelectedProcessIndex();
    HANDLE hProcess;
    char message[512];
    DWORD errorCode;

    if (index == -1 || index >= g_AppData.count)
        return;

    if (!g_AppData.processes[index].running || g_AppData.processes[index].pid == 0)
    {
        MessageBox(hwndOwner, "The selected process is not currently running.",
                   "End Process", MB_OK | MB_ICONINFORMATION);
        return;
    }

    sprintf_s(message, sizeof(message), "End process \"%s\" (PID %lu)?",
              g_AppData.processes[index].name, g_AppData.processes[index].pid);
    if (MessageBox(hwndOwner, message, "Confirm End Process",
                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, g_AppData.processes[index].pid);
    if (!hProcess)
    {
        errorCode = GetLastError();
        sprintf_s(message, sizeof(message),
                  "Unable to open PID %lu for termination.\nWindows error: %lu",
                  g_AppData.processes[index].pid, errorCode);
        MessageBox(hwndOwner, message, "End Process Failed", MB_OK | MB_ICONERROR);
        return;
    }

    if (!TerminateProcess(hProcess, 1))
    {
        errorCode = GetLastError();
        CloseHandle(hProcess);
        sprintf_s(message, sizeof(message),
                  "Failed to end \"%s\" (PID %lu).\nWindows error: %lu",
                  g_AppData.processes[index].name, g_AppData.processes[index].pid, errorCode);
        MessageBox(hwndOwner, message, "End Process Failed", MB_OK | MB_ICONERROR);
        RefreshProcessList(g_AppData.hwndListView);
        return;
    }

    CloseHandle(hProcess);
    RefreshProcessList(g_AppData.hwndListView);
}

void OpenSelectedProcessInTaskManager(HWND hwndOwner)
{
    int index = GetSelectedProcessIndex();
    HINSTANCE result;

    if (index == -1 || index >= g_AppData.count)
        return;

    result = ShellExecuteA(hwndOwner, "open", "taskmgr.exe", NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
    {
        MessageBox(hwndOwner, "Failed to open Task Manager.",
                   "Open in Task Manager", MB_OK | MB_ICONERROR);
    }
}

void OpenSelectedProcessFileLocation(HWND hwndOwner)
{
    int index = GetSelectedProcessIndex();
    char processPath[MAX_PATH] = {0};
    char commandLine[MAX_PATH + 32];
    HINSTANCE result;

    if (index == -1 || index >= g_AppData.count)
        return;

    if (!GetKnownProcessExecutablePath(&g_AppData.processes[index], processPath, sizeof(processPath)))
    {
        MessageBoxA(hwndOwner, "The selected process path is not known yet.",
                    "Open File Location", MB_OK | MB_ICONINFORMATION);
        return;
    }

    sprintf_s(commandLine, sizeof(commandLine), "/select,\"%s\"", processPath);
    result = ShellExecuteA(hwndOwner, "open", "explorer.exe", commandLine, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
    {
        MessageBoxA(hwndOwner, "Failed to open the process file location.",
                    "Open File Location", MB_OK | MB_ICONERROR);
    }
}

void StartSelectedProcess(HWND hwndOwner)
{
    int index = GetSelectedProcessIndex();
    char processPath[MAX_PATH] = {0};
    char launchDirectory[MAX_PATH] = {0};
    char commandLineBuffer[4096];
    PROCESS_INFORMATION processInfo = {0};
    STARTUPINFOA startupInfo = {0};
    HINSTANCE result;

    if (index == -1 || index >= g_AppData.count)
        return;

    if (g_AppData.processes[index].running)
    {
        MessageBoxA(hwndOwner, "The selected process is already running.",
                    "Start Process", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (!GetKnownProcessExecutablePath(&g_AppData.processes[index], processPath, sizeof(processPath)))
    {
        MessageBoxA(hwndOwner, "The selected process can only be started after its executable path has been discovered.",
                    "Start Process", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (strcpy_s(launchDirectory, sizeof(launchDirectory), processPath) == 0)
    {
        char *lastSeparator = strrchr(launchDirectory, '\\');
        if (lastSeparator)
            *lastSeparator = '\0';
        else
            launchDirectory[0] = '\0';
    }

    if (g_AppData.processes[index].commandLine[0] != '\0')
    {
        if (strcpy_s(commandLineBuffer, sizeof(commandLineBuffer), g_AppData.processes[index].commandLine) != 0)
        {
            MessageBoxA(hwndOwner, "The saved command line is too long to restart.",
                        "Start Process", MB_OK | MB_ICONERROR);
            return;
        }

        startupInfo.cb = sizeof(startupInfo);
        if (CreateProcessA(NULL, commandLineBuffer, NULL, NULL, FALSE, 0, NULL,
                           launchDirectory[0] != '\0' ? launchDirectory : NULL,
                           &startupInfo, &processInfo))
        {
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            RefreshProcessList(g_AppData.hwndListView);
            return;
        }
    }

    result = ShellExecuteA(hwndOwner, "open", processPath, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
    {
        MessageBoxA(hwndOwner, "Failed to start the selected process.",
                    "Start Process", MB_OK | MB_ICONERROR);
        return;
    }

    RefreshProcessList(g_AppData.hwndListView);
}

void LaunchNewProcess(HWND hwndOwner)
{
    OPENFILENAMEA ofn = {0};
    char filePath[MAX_PATH] = {0};
    HINSTANCE result;

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndOwner;
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = sizeof(filePath);
    ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = "Start New Process";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn))
        return;

    result = ShellExecuteA(hwndOwner, "open", filePath, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
    {
        MessageBoxA(hwndOwner, "Failed to start the process.",
                    "Start New Process", MB_OK | MB_ICONERROR);
        return;
    }

    RefreshProcessList(g_AppData.hwndListView);
}

BOOL TrySelectListViewItemAtScreenPoint(HWND hwndListView, POINT screenPoint, int *pIndex)
{
    LVHITTESTINFO hitTest = {0};

    hitTest.pt = screenPoint;
    ScreenToClient(hwndListView, &hitTest.pt);

    hitTest.iItem = ListView_SubItemHitTest(hwndListView, &hitTest);
    if (hitTest.iItem == -1 || (hitTest.flags & LVHT_ONITEM) == 0)
        return FALSE;

    ListView_SetItemState(hwndListView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(hwndListView, hitTest.iItem, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hwndListView, hitTest.iItem, FALSE);

    if (pIndex)
        *pIndex = hitTest.iItem;

    return TRUE;
}

BOOL ShowListViewContextMenu(HWND hwndOwner, HWND hwndListView, POINT screenPoint)
{
    int index = -1;
    char processPath[MAX_PATH] = {0};
    HMENU hMenu;
    UINT endProcessFlags = MF_STRING;
    UINT startProcessFlags = MF_STRING;
    UINT openLocationFlags = MF_STRING;
    BOOL watcherTopmost;
    BOOL hasKnownPath;
    int command;

    if (!hwndListView)
        return FALSE;

    if (screenPoint.x == -1 && screenPoint.y == -1)
    {
        RECT itemRect;

        index = GetSelectedProcessIndex();
        if (index == -1 || !ListView_GetItemRect(hwndListView, index, &itemRect, LVIR_BOUNDS))
            return FALSE;

        screenPoint.x = itemRect.left;
        screenPoint.y = itemRect.bottom;
        ClientToScreen(hwndListView, &screenPoint);
    }
    else if (!TrySelectListViewItemAtScreenPoint(hwndListView, screenPoint, &index))
    {
        return FALSE;
    }

    if (index == -1)
        return FALSE;

    hasKnownPath = GetKnownProcessExecutablePath(&g_AppData.processes[index], processPath, sizeof(processPath));

    if (!g_AppData.processes[index].running || g_AppData.processes[index].pid == 0)
        endProcessFlags |= MF_GRAYED;
    if (g_AppData.processes[index].running || !hasKnownPath)
        startProcessFlags |= MF_GRAYED;
    if (!hasKnownPath)
        openLocationFlags |= MF_GRAYED;

    watcherTopmost = IsWindowTopmost(hwndOwner);

    hMenu = CreatePopupMenu();
    if (!hMenu)
        return FALSE;

    AppendMenu(hMenu, openLocationFlags, ID_CONTEXT_OPEN_FILE_LOCATION, TEXT("Open File Location"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_CONTEXT_OPEN_TASK_MANAGER, TEXT("Open Task Manager"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, startProcessFlags, ID_CONTEXT_START_PROCESS, TEXT("Start Process"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, endProcessFlags, ID_CONTEXT_END_PROCESS, TEXT("End Process"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING | (watcherTopmost ? MF_CHECKED : MF_UNCHECKED), ID_CONTEXT_TOGGLE_TOPMOST,
               TEXT("Keep On Top"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_CONTEXT_REMOVE_PROCESS, TEXT("Remove Selected"));
    command = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y,
                             0, hwndOwner, NULL);
    DestroyMenu(hMenu);

    if (command == ID_CONTEXT_OPEN_FILE_LOCATION)
        OpenSelectedProcessFileLocation(hwndOwner);
    else if (command == ID_CONTEXT_OPEN_TASK_MANAGER)
        OpenSelectedProcessInTaskManager(hwndOwner);
    else if (command == ID_CONTEXT_START_PROCESS)
        StartSelectedProcess(hwndOwner);
    else if (command == ID_CONTEXT_END_PROCESS)
        EndSelectedProcess(hwndOwner);
    else if (command == ID_CONTEXT_TOGGLE_TOPMOST)
    {
        ToggleWatcherTopmost(hwndOwner, !watcherTopmost);
        UpdateOptionsMenuState(hwndOwner);
        SaveSettingsWithFeedback(hwndOwner);
    }
    else if (command == ID_CONTEXT_REMOVE_PROCESS)
        RemoveSelectedProcess();

    return TRUE;
}

void PopulateComboBoxFiltered(HWND hwndCombo, const char *filterText)
{
    char currentText[256] = {0};
    ComboProcessEntry *entries = NULL;
    int entryCount = 0;
    int entryCapacity = 0;

    GetWindowTextA(hwndCombo, currentText, sizeof(currentText));

    SendMessage(hwndCombo, CB_RESETCONTENT, 0, 0);

    {
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32W pe32 = {0};
            pe32.dwSize = sizeof(PROCESSENTRY32W);

            if (Process32FirstW(hSnapshot, &pe32))
            {
                do
                {
                    char processName[256] = {0};
                    int matchRank;

                    if (!GetProcessExecutableNameByPid(pe32.th32ProcessID, processName, sizeof(processName)))
                        WideToUtf8(pe32.szExeFile, processName, sizeof(processName));

                    if (IsWatchedProcessName(processName))
                        continue;

                    matchRank = GetComboMatchRank(processName, filterText);
                    if (matchRank < 0)
                        continue;

                    if (entryCount == entryCapacity)
                    {
                        int newCapacity = entryCapacity == 0 ? 64 : entryCapacity * 2;
                        ComboProcessEntry *newEntries = (ComboProcessEntry *)realloc(entries, sizeof(ComboProcessEntry) * (size_t)newCapacity);

                        if (!newEntries)
                            break;

                        entries = newEntries;
                        entryCapacity = newCapacity;
                    }

                    strcpy_s(entries[entryCount].name, sizeof(entries[entryCount].name), processName);
                    entries[entryCount].pid = pe32.th32ProcessID;
                    entries[entryCount].matchRank = matchRank;
                    entryCount++;
                } while (Process32NextW(hSnapshot, &pe32));
            }

            CloseHandle(hSnapshot);
        }
    }

    if (entries && entryCount > 1)
        qsort(entries, (size_t)entryCount, sizeof(entries[0]), CompareComboProcessEntries);

    for (int i = 0; i < entryCount; i++)
    {
        char displayText[320];
        LRESULT index;

        sprintf_s(displayText, sizeof(displayText), "%s (%lu)",
                  entries[i].name, entries[i].pid);

        index = ComboBoxAddStringUtf8(hwndCombo, displayText);
        if (index != CB_ERR && index != CB_ERRSPACE)
            SendMessage(hwndCombo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)entries[i].pid);
    }

    free(entries);

    SetWindowTextA(hwndCombo, currentText);
}

void PopulateComboBox(HWND hwndCombo)
{
    PopulateComboBoxFiltered(hwndCombo, NULL);
}

void RefreshComboBoxForCurrentText(HWND hwndCombo)
{
    char currentText[256] = {0};

    if (!hwndCombo)
        return;

    GetWindowTextA(hwndCombo, currentText, sizeof(currentText));

    if (currentText[0] != '\0')
        PopulateComboBoxFiltered(hwndCombo, currentText);
    else
        PopulateComboBox(hwndCombo);
}

void ApplyComboBoxFilter(HWND hwndCombo)
{
    char typedText[256] = {0};
    DWORD editSelection;
    int selectionStart;
    int selectionEnd;

    if (!hwndCombo || g_AppData.bUpdatingComboText)
        return;

    GetWindowTextA(hwndCombo, typedText, sizeof(typedText));
    editSelection = (DWORD)SendMessage(hwndCombo, CB_GETEDITSEL, 0, 0);
    selectionStart = LOWORD(editSelection);
    selectionEnd = HIWORD(editSelection);

    g_AppData.bUpdatingComboText = TRUE;
    PopulateComboBoxFiltered(hwndCombo, typedText);
    SetWindowTextA(hwndCombo, typedText);
    SendMessage(hwndCombo, CB_SETEDITSEL, 0, MAKELPARAM(selectionStart, selectionEnd));
    g_AppData.bUpdatingComboText = FALSE;
}

void LayoutControls(HWND hwnd)
{
    RECT clientRect;
    const int margin = 10;
    const int buttonWidth = 130;
    const int rowHeight = 25;
    int statusBarHeight = 22;

    if (!GetClientRect(hwnd, &clientRect))
        return;

    {
        RECT statusRect;
        int clientWidth = clientRect.right - clientRect.left;
        int clientHeight = clientRect.bottom - clientRect.top;
        int statusBarTop;
        int buttonY;
        int listHeight;
        int comboWidth = clientWidth - (margin * 6) - (buttonWidth * 4);

        if (g_AppData.hwndStatusBar && GetWindowRect(g_AppData.hwndStatusBar, &statusRect))
            statusBarHeight = statusRect.bottom - statusRect.top;

        statusBarTop = clientHeight - statusBarHeight;
        buttonY = statusBarTop - margin - rowHeight;
        listHeight = buttonY - (margin * 2);

        if (comboWidth < 120)
            comboWidth = 120;
        if (listHeight < 80)
            listHeight = 80;

        if (g_AppData.hwndListView)
            MoveWindow(g_AppData.hwndListView, margin, margin,
                       clientWidth - (margin * 2), listHeight, TRUE);

        if (g_AppData.hwndProcessCombo)
            MoveWindow(g_AppData.hwndProcessCombo, margin, buttonY,
                       comboWidth, 200, TRUE);

        if (g_AppData.hwndAddButton)
            MoveWindow(g_AppData.hwndAddButton, margin + comboWidth + margin, buttonY,
                       buttonWidth, rowHeight, TRUE);

        if (g_AppData.hwndPickWindowButton)
            MoveWindow(g_AppData.hwndPickWindowButton,
                       margin + comboWidth + (margin * 2) + buttonWidth,
                       buttonY, buttonWidth, rowHeight, TRUE);

        if (g_AppData.hwndProcessBrowserButton)
            MoveWindow(g_AppData.hwndProcessBrowserButton,
                       margin + comboWidth + (margin * 3) + (buttonWidth * 2),
                       buttonY, buttonWidth, rowHeight, TRUE);

        if (g_AppData.hwndRemoveButton)
            MoveWindow(g_AppData.hwndRemoveButton, clientWidth - margin - buttonWidth,
                       buttonY, buttonWidth, rowHeight, TRUE);

        if (g_AppData.hwndStatusBar)
            MoveWindow(g_AppData.hwndStatusBar, 0, statusBarTop,
                       clientWidth, statusBarHeight, TRUE);
    }
}

void GetMinimumWindowSize(HWND hwnd, int *minWidth, int *minHeight)
{
    RECT minClientRect = {0, 0, 700, 170};
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    AdjustWindowRectEx(&minClientRect, style, GetMenu(hwnd) != NULL, exStyle);

    if (minWidth)
        *minWidth = minClientRect.right - minClientRect.left;
    if (minHeight)
        *minHeight = minClientRect.bottom - minClientRect.top;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
    {
        HMENU hMenuBar;

        LoadSettingsFromIni(hwnd);

        hMenuBar = CreateMainWindowMenu();
        if (hMenuBar)
            SetMenu(hwnd, hMenuBar);

        g_AppData.hwndProcessCombo = CreateWindow(TEXT("COMBOBOX"), TEXT(""),
                                                  WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                                      CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                                  10, 10, 200, 200,
                                                  hwnd, (HMENU)IDC_PROCESS_NAME, GetModuleHandle(NULL), NULL);
        SetWindowSubclass(g_AppData.hwndProcessCombo, ComboOpenOnClickProc, 1, 0);
        {
            COMBOBOXINFO comboInfo = {0};
            comboInfo.cbSize = sizeof(comboInfo);
            if (GetComboBoxInfo(g_AppData.hwndProcessCombo, &comboInfo) && comboInfo.hwndItem)
                SetWindowSubclass(comboInfo.hwndItem, ComboOpenOnClickProc, 2, 0);
        }
        PopulateComboBox(g_AppData.hwndProcessCombo);

        g_AppData.hwndAddButton = CreateWindow(TEXT("BUTTON"), TEXT("Add Process"),
                                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               220, 10, 130, 25,
                                               hwnd, (HMENU)IDC_ADD_BUTTON, GetModuleHandle(NULL), NULL);

        g_AppData.hwndPickWindowButton = CreateWindow(TEXT("BUTTON"), TEXT("Pick Window"),
                                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                      360, 10, 130, 25,
                                                      hwnd, (HMENU)IDC_PICK_WINDOW_BUTTON,
                                                      GetModuleHandle(NULL), NULL);

        g_AppData.hwndProcessBrowserButton = CreateWindow(TEXT("BUTTON"), TEXT("Process Browser"),
                                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                          500, 10, 130, 25,
                                                          hwnd, (HMENU)IDC_PROCESS_BROWSER_BUTTON,
                                                          GetModuleHandle(NULL), NULL);

        g_AppData.hwndRemoveButton = CreateWindow(TEXT("BUTTON"), TEXT("Remove Selected"),
                                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                  640, 10, 130, 25,
                                                  hwnd, (HMENU)IDC_REMOVE_BUTTON, GetModuleHandle(NULL), NULL);

        g_AppData.hwndListView = CreateWindow(WC_LISTVIEW, TEXT(""),
                                              WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                                                  LVS_REPORT | LVS_SINGLESEL,
                                              10, 75, 490, 250,
                                              hwnd, (HMENU)IDC_LISTBOX, GetModuleHandle(NULL), NULL);

        g_AppData.hwndStatusBar = CreateWindow(STATUSCLASSNAME, TEXT(""),
                                               WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                               0, 0, 0, 0,
                                               hwnd, (HMENU)IDC_STATUS_BAR, GetModuleHandle(NULL), NULL);

        SendMessage(g_AppData.hwndListView, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        ListViewInsertColumnUtf8(g_AppData.hwndListView, 0, 180, "Process Name");
        ListViewInsertColumnUtf8(g_AppData.hwndListView, 1, 80, "Status");
        ListViewInsertColumnUtf8(g_AppData.hwndListView, 2, 70, "PID");
        ListViewInsertColumnUtf8(g_AppData.hwndListView, 3, 70, "CPU %");
        ListViewInsertColumnUtf8(g_AppData.hwndListView, 4, 90, "Memory");
        ListViewInsertColumnUtf8(g_AppData.hwndListView, 5, 150, "Last Seen");

        for (int i = 0; i < COLUMN_COUNT; i++)
        {
            if (g_AppData.savedColumnWidths[i] > 0)
                ListView_SetColumnWidth(g_AppData.hwndListView, i, g_AppData.savedColumnWidths[i]);
        }

        UpdateListSortIndicators();
        ApplySavedWindowPlacement(hwnd);
        UpdateOptionsMenuState(hwnd);
        WriteApplicationLifecycleLogEntry(TRUE);
        LayoutControls(hwnd);
        UpdateRemoveButtonState();
        RefreshProcessList(g_AppData.hwndListView);
        UpdateStatusBar();
        ApplyAutoRefreshTimer(hwnd);
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int notif = HIWORD(wParam);

        if (id == IDC_ADD_BUTTON)
        {
            HWND hwndCombo = GetDlgItem(hwnd, IDC_PROCESS_NAME);
            char processName[256] = {0};
            int selectedIndex = (int)SendMessage(hwndCombo, CB_GETCURSEL, 0, 0);
            GetWindowTextA(hwndCombo, processName, sizeof(processName));

            if (selectedIndex != CB_ERR)
            {
                LRESULT selectedItemData = SendMessage(hwndCombo, CB_GETITEMDATA, (WPARAM)selectedIndex, 0);
                char selectedProcessName[256];

                if (selectedItemData != CB_ERR &&
                    GetProcessExecutableNameByPid((DWORD)selectedItemData,
                                                  selectedProcessName, sizeof(selectedProcessName)))
                {
                    strcpy_s(processName, sizeof(processName), selectedProcessName);
                }
            }

            if (strlen(processName) > 0)
            {
                AddProcess(processName);
                KillTimer(hwnd, ID_COMBO_FILTER_TIMER);
                SetWindowTextA(hwndCombo, "");
                SetFocus(hwndCombo);
            }
        }
        else if (id == IDC_PROCESS_NAME && notif == CBN_DROPDOWN)
        {
            HWND hwndCombo = GetDlgItem(hwnd, IDC_PROCESS_NAME);

            KillTimer(hwnd, ID_COMBO_FILTER_TIMER);
            RefreshComboBoxForCurrentText(hwndCombo);
        }
        else if (id == IDC_PROCESS_NAME && notif == CBN_KILLFOCUS)
        {
            KillTimer(hwnd, ID_COMBO_FILTER_TIMER);
        }
        else if (id == IDC_PROCESS_NAME && notif == CBN_EDITCHANGE)
        {
            if (!g_AppData.bUpdatingComboText)
                SetTimer(hwnd, ID_COMBO_FILTER_TIMER, COMBO_FILTER_DELAY_MS, NULL);
        }
        else if (id == IDC_REMOVE_BUTTON)
        {
            RemoveSelectedProcess();
        }
        else if (id == IDC_PICK_WINDOW_BUTTON)
        {
            ToggleWindowPicker(hwnd);
        }
        else if (id == IDC_PROCESS_BROWSER_BUTTON)
        {
            ShowProcessBrowserWindow(hwnd);
        }
        else if (id == ID_FILE_EXIT)
        {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        else if (id == ID_FILE_OPEN_EVENT_LOG_WINDOW)
        {
            ShowEventLogWindow(hwnd);
        }
        else if (id == ID_FILE_OPEN_LOG)
        {
            OpenLogFile(hwnd);
        }
        else if (id == ID_FILE_OPEN_LOG_FOLDER)
        {
            OpenLogFolder(hwnd);
        }
        else if (id == ID_FILE_LAUNCH_NEW_PROCESS)
        {
            LaunchNewProcess(hwnd);
        }
        else if (id == ID_FILE_PROCESS_BROWSER)
        {
            ShowProcessBrowserWindow(hwnd);
        }
        else if (id == ID_FORCE_REFRESH)
        {
            RefreshProcessList(g_AppData.hwndListView);
        }
        else if (id == ID_OPTIONS_AUTO_REFRESH)
        {
            g_AppData.autoRefreshEnabled = !g_AppData.autoRefreshEnabled;
            ApplyAutoRefreshTimer(hwnd);
            UpdateStatusBar();
            UpdateOptionsMenuState(hwnd);
            SaveSettingsWithFeedback(hwnd);
            InvalidateRect(g_AppData.hwndListView, NULL, TRUE);
            UpdateWindow(g_AppData.hwndListView);
        }
        else if (id == ID_OPTIONS_START_WITH_WINDOWS)
        {
            g_AppData.startWithWindows = !g_AppData.startWithWindows;
            UpdateOptionsMenuState(hwnd);
            SaveSettingsWithFeedback(hwnd);
        }
        else if (id == ID_OPTIONS_NOTIFY_ON_STOP)
        {
            g_AppData.notifyOnStop = !g_AppData.notifyOnStop;
            UpdateOptionsMenuState(hwnd);
            SaveSettingsWithFeedback(hwnd);
        }
        else if (id == ID_OPTIONS_CREATE_STOP_LOGS)
        {
            g_AppData.createStopLogs = !g_AppData.createStopLogs;
            UpdateOptionsMenuState(hwnd);
            SaveSettingsWithFeedback(hwnd);
            RefreshEventLogView();
        }
        else if (id == ID_CONTEXT_TOGGLE_TOPMOST)
        {
            ToggleWatcherTopmost(hwnd, !IsWindowTopmost(hwnd));
            UpdateOptionsMenuState(hwnd);
            SaveSettingsWithFeedback(hwnd);
        }
        else if (id == ID_HELP_WATCH_SYNTAX)
        {
            ShowHelpDialog(hwnd);
        }
        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == ID_REFRESH_TIMER)
        {
            RefreshProcessList(g_AppData.hwndListView);
        }
        else if (wParam == ID_COMBO_FILTER_TIMER)
        {
            KillTimer(hwnd, ID_COMBO_FILTER_TIMER);
            ApplyComboBoxFilter(g_AppData.hwndProcessCombo);
        }
        else if (wParam == ID_WINDOW_PICKER_TIMER)
        {
            HandleWindowPickerTimer(hwnd);
        }
        return 0;
    }

    case WM_NOTIFY:
    {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh->idFrom == IDC_LISTBOX && pnmh->code == LVN_COLUMNCLICK)
        {
            NMLISTVIEW *pnmlv = (NMLISTVIEW *)lParam;
            if (g_AppData.sortColumn == pnmlv->iSubItem)
                g_AppData.sortAscending = !g_AppData.sortAscending;
            else
            {
                g_AppData.sortColumn = pnmlv->iSubItem;
                g_AppData.sortAscending = TRUE;
            }

            UpdateListSortIndicators();
            RefreshProcessList(g_AppData.hwndListView);
            return 0;
        }
        else if (pnmh->idFrom == IDC_LISTBOX && pnmh->code == NM_CUSTOMDRAW)
        {
            LPNMLVCUSTOMDRAW pcd = (LPNMLVCUSTOMDRAW)lParam;
            switch (pcd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;

            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYSUBITEMDRAW;

            case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                if ((pcd->nmcd.uItemState & CDIS_SELECTED) == 0)
                {
                    if (!IsAutoRefreshEnabled() && pcd->iSubItem >= 1)
                    {
                        pcd->clrText = RGB(128, 128, 128);
                    }
                    else if (pcd->iSubItem == 1)
                    {
                        pcd->clrText = pcd->nmcd.lItemlParam ? RGB(0, 128, 0) : RGB(192, 0, 0);
                    }
                }
                return CDRF_DODEFAULT;
            }
        }
        else if (pnmh->idFrom == IDC_LISTBOX && pnmh->code == LVN_ITEMCHANGED)
        {
            UpdateRemoveButtonState();
        }
        break;
    }

    case WM_CONTEXTMENU:
    {
        POINT screenPoint;
        screenPoint.x = GET_X_LPARAM(lParam);
        screenPoint.y = GET_Y_LPARAM(lParam);

        if ((HWND)wParam == g_AppData.hwndListView)
        {
            if (ShowListViewContextMenu(hwnd, g_AppData.hwndListView, screenPoint))
                return 0;
        }

        if ((HWND)wParam == hwnd)
        {
            ShowGlobalContextMenu(hwnd, screenPoint);
            return 0;
        }

        break;
    }

    case WM_SIZE:
    {
        (void)lParam;
        LayoutControls(hwnd);
        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *minMaxInfo = (MINMAXINFO *)lParam;
        int minWidth = 0;
        int minHeight = 0;

        GetMinimumWindowSize(hwnd, &minWidth, &minHeight);
        minMaxInfo->ptMinTrackSize.x = minWidth;
        minMaxInfo->ptMinTrackSize.y = minHeight;
        return 0;
    }

    case WM_DESTROY:
    {
        KillTimer(hwnd, ID_REFRESH_TIMER);
        KillTimer(hwnd, ID_COMBO_FILTER_TIMER);
        KillTimer(hwnd, ID_WINDOW_PICKER_TIMER);
        WriteApplicationLifecycleLogEntry(FALSE);
        if (g_AppData.hwndEventLogWindow)
            DestroyWindow(g_AppData.hwndEventLogWindow);
        if (g_AppData.hwndProcessBrowserWindow)
            DestroyWindow(g_AppData.hwndProcessBrowserWindow);
        for (int i = 0; i < g_AppData.count; i++)
            CloseTrackedProcessHandle(&g_AppData.processes[i]);
        RemoveNotificationIcon(hwnd);
        SaveSettingsWithFeedback(hwnd);
        PostQuitMessage(0);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int CALLBACK CompareEventLogItems(LPARAM lParamItem1, LPARAM lParamItem2, LPARAM lParamSort)
{
    HWND hwndList = (HWND)lParamSort;
    char text1[512] = {0};
    char text2[512] = {0};
    int index1;
    int index2;
    int result;

    index1 = FindListViewItemByParam(hwndList, lParamItem1);
    index2 = FindListViewItemByParam(hwndList, lParamItem2);
    if (index1 == -1 || index2 == -1)
        return 0;

    ListViewGetItemTextUtf8(hwndList, index1, g_AppData.logSortColumn, text1, sizeof(text1));
    ListViewGetItemTextUtf8(hwndList, index2, g_AppData.logSortColumn, text2, sizeof(text2));

    result = CompareEventLogColumnValues(g_AppData.logSortColumn, text1, text2);
    return g_AppData.logSortAscending ? result : -result;
}

int FindListViewItemByParam(HWND hwndListView, LPARAM itemParam)
{
    LVFINDINFO findInfo = {0};

    findInfo.flags = LVFI_PARAM;
    findInfo.lParam = itemParam;
    return ListView_FindItem(hwndListView, -1, &findInfo);
}

int CompareEventLogColumnValues(int columnIndex, const char *left, const char *right)
{
    char *endLeft;
    char *endRight;

    switch (columnIndex)
    {
    case 3:
    {
        unsigned long leftValue = strtoul(left && left[0] != '\0' ? left : "0", &endLeft, 10);
        unsigned long rightValue = strtoul(right && right[0] != '\0' ? right : "0", &endRight, 10);

        if (endLeft != left && endRight != right)
            return CompareUInt32((DWORD)leftValue, (DWORD)rightValue);
        break;
    }

    case 4:
    case 5:
    {
        double leftValue = strtod(left && left[0] != '\0' ? left : "0", &endLeft);
        double rightValue = strtod(right && right[0] != '\0' ? right : "0", &endRight);

        if (endLeft != left && endRight != right)
            return CompareDoubleValues(leftValue, rightValue);
        break;
    }

    default:
        break;
    }

    return CompareUtf8Insensitive(left, right);
}

void UpdateEventLogSortIndicators(void)
{
    HWND hwndHeader;
    int i;

    if (!g_AppData.hwndEventLogEdit)
        return;

    hwndHeader = ListView_GetHeader(g_AppData.hwndEventLogEdit);
    if (!hwndHeader)
        return;

    for (i = 0; i < EVENT_LOG_COLUMN_COUNT; i++)
    {
        HDITEM hditem = {0};
        hditem.mask = HDI_FORMAT;
        if (!Header_GetItem(hwndHeader, i, &hditem))
            continue;
        hditem.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_AppData.logSortColumn)
            hditem.fmt |= g_AppData.logSortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hwndHeader, i, &hditem);
    }
}

int CompareProcessBrowserEntries(const void *left, const void *right)
{
    const ProcessBrowserEntry *entryLeft = (const ProcessBrowserEntry *)left;
    const ProcessBrowserEntry *entryRight = (const ProcessBrowserEntry *)right;
    int result = 0;

    switch (g_AppData.processBrowserSortColumn)
    {
    case 0:
        if (entryLeft->pid < entryRight->pid)
            result = -1;
        else if (entryLeft->pid > entryRight->pid)
            result = 1;
        break;

    case 1:
        result = CompareUtf8Insensitive(entryLeft->processName, entryRight->processName);
        break;

    case 2:
        result = CompareUtf8Insensitive(entryLeft->commandLine, entryRight->commandLine);
        break;

    default:
        break;
    }

    if (result == 0)
    {
        if (entryLeft->pid < entryRight->pid)
            result = -1;
        else if (entryLeft->pid > entryRight->pid)
            result = 1;
    }

    return g_AppData.processBrowserSortAscending ? result : -result;
}

void UpdateProcessBrowserSortIndicators(void)
{
    HWND hwndHeader;

    if (!g_AppData.hwndProcessBrowserList)
        return;

    hwndHeader = ListView_GetHeader(g_AppData.hwndProcessBrowserList);
    if (!hwndHeader)
        return;

    for (int i = 0; i < PROCESS_BROWSER_COLUMN_COUNT; i++)
    {
        HDITEM item = {0};

        item.mask = HDI_FORMAT;
        if (!Header_GetItem(hwndHeader, i, &item))
            continue;

        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_AppData.processBrowserSortColumn)
            item.fmt |= g_AppData.processBrowserSortAscending ? HDF_SORTUP : HDF_SORTDOWN;

        Header_SetItem(hwndHeader, i, &item);
    }
}

LRESULT CALLBACK EventLogWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
    {
        g_AppData.hwndEventLogEdit = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                                         LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                                     10, 10, 100, 100,
                                                     hwnd, (HMENU)IDC_EVENT_LOG_EDIT, GetModuleHandle(NULL), NULL);
        if (g_AppData.hwndEventLogEdit)
        {
            SendMessage(g_AppData.hwndEventLogEdit, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 0, 140, "Timestamp");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 1, 110, "Event");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 2, 180, "Target");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 3, 80, "PID");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 4, 90, "CPU");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 5, 100, "Memory");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 6, 260, "Path");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 7, 360, "Command Line");
            ListViewInsertColumnUtf8(g_AppData.hwndEventLogEdit, 8, 320, "Details");
        }
        LayoutEventLogWindow(hwnd);
        RefreshEventLogView();
        return 0;
    }

    case WM_SIZE:
        LayoutEventLogWindow(hwnd);
        return 0;

    case WM_NOTIFY:
    {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh->idFrom == IDC_EVENT_LOG_EDIT && pnmh->code == LVN_COLUMNCLICK)
        {
            NMLISTVIEW *pnmlv = (NMLISTVIEW *)lParam;
            if (g_AppData.logSortColumn == pnmlv->iSubItem)
                g_AppData.logSortAscending = !g_AppData.logSortAscending;
            else
            {
                g_AppData.logSortColumn = pnmlv->iSubItem;
                g_AppData.logSortAscending = TRUE;
            }
            UpdateEventLogSortIndicators();
            ListView_SortItemsEx(g_AppData.hwndEventLogEdit, CompareEventLogItems,
                                 (LPARAM)g_AppData.hwndEventLogEdit);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_AppData.hwndEventLogEdit = NULL;
        g_AppData.hwndEventLogWindow = NULL;
        return 0;

    default:
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void RefreshProcessBrowserList(void)
{
    HANDLE hSnapshot;
    PROCESSENTRY32W pe32 = {0};
    ProcessBrowserEntry *entries = NULL;
    int entryCount = 0;
    int entryCapacity = 0;
    int rowIndex = 0;
    int selectedIndex;
    DWORD selectedPid = 0;

    if (!g_AppData.hwndProcessBrowserList)
        return;

    selectedIndex = ListView_GetNextItem(g_AppData.hwndProcessBrowserList, -1, LVNI_SELECTED);
    if (selectedIndex != -1)
    {
        LVITEMW selectedItem = {0};
        selectedItem.mask = LVIF_PARAM;
        selectedItem.iItem = selectedIndex;
        SendMessageW(g_AppData.hwndProcessBrowserList, LVM_GETITEMW, 0, (LPARAM)&selectedItem);
        selectedPid = (DWORD)selectedItem.lParam;
    }

    ListView_DeleteAllItems(g_AppData.hwndProcessBrowserList);

    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return;

    pe32.dwSize = sizeof(PROCESSENTRY32W);
    if (!Process32FirstW(hSnapshot, &pe32))
    {
        CloseHandle(hSnapshot);
        return;
    }

    do
    {
        ProcessBrowserEntry *newEntries;

        if (entryCount == entryCapacity)
        {
            int newCapacity = entryCapacity == 0 ? 64 : entryCapacity * 2;
            newEntries = (ProcessBrowserEntry *)realloc(entries, sizeof(ProcessBrowserEntry) * (size_t)newCapacity);
            if (!newEntries)
                break;

            entries = newEntries;
            entryCapacity = newCapacity;
        }

        entries[entryCount].pid = pe32.th32ProcessID;
        entries[entryCount].processName[0] = '\0';
        entries[entryCount].commandLine[0] = '\0';

        if (!GetProcessExecutableNameByPid(pe32.th32ProcessID,
                                           entries[entryCount].processName,
                                           sizeof(entries[entryCount].processName)))
        {
            WideToUtf8(pe32.szExeFile, entries[entryCount].processName,
                       sizeof(entries[entryCount].processName));
        }

        GetProcessCommandLineByPid(pe32.th32ProcessID,
                                   entries[entryCount].commandLine,
                                   sizeof(entries[entryCount].commandLine));
        entryCount++;
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);

    if (entries && entryCount > 1)
        qsort(entries, (size_t)entryCount, sizeof(entries[0]), CompareProcessBrowserEntries);

    for (int i = 0; i < entryCount; i++)
    {
        char pidText[32] = {0};
        int index;

        sprintf_s(pidText, sizeof(pidText), "%lu", entries[i].pid);

        index = ListViewInsertItemUtf8(g_AppData.hwndProcessBrowserList, rowIndex,
                                       pidText, (LPARAM)entries[i].pid);
        if (index == -1)
            continue;

        ListViewSetItemTextUtf8(g_AppData.hwndProcessBrowserList, index, 1, entries[i].processName);
        ListViewSetItemTextUtf8(g_AppData.hwndProcessBrowserList, index, 2, entries[i].commandLine);

        if (selectedPid != 0 && entries[i].pid == selectedPid)
        {
            ListView_SetItemState(g_AppData.hwndProcessBrowserList, index,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetSelectionMark(g_AppData.hwndProcessBrowserList, index);
            ListView_EnsureVisible(g_AppData.hwndProcessBrowserList, index, FALSE);
        }

        rowIndex++;
    }

    UpdateProcessBrowserSortIndicators();
    free(entries);
}

void ShowProcessBrowserWindow(HWND hwndOwner)
{
    if (g_AppData.hwndProcessBrowserWindow)
    {
        ShowWindow(g_AppData.hwndProcessBrowserWindow, SW_SHOWNORMAL);
        SetForegroundWindow(g_AppData.hwndProcessBrowserWindow);
        RefreshProcessBrowserList();
        return;
    }

    g_AppData.hwndProcessBrowserWindow = CreateWindowExA(
        WS_EX_APPWINDOW,
        "ProcessWatcherBrowser",
        "Process Browser - double-click to add to watch list",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 480,
        hwndOwner, NULL, GetModuleHandle(NULL), NULL);

    if (!g_AppData.hwndProcessBrowserWindow)
        MessageBoxA(hwndOwner, "Failed to open the Process Browser window.",
                    "Process Browser", MB_OK | MB_ICONERROR);
}

LRESULT CALLBACK ProcessBrowserWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
    {
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);

        g_AppData.hwndProcessBrowserList = CreateWindowExA(
            WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0,
            clientRect.right - clientRect.left,
            clientRect.bottom - clientRect.top,
            hwnd, (HMENU)IDC_PROCESS_BROWSER_LIST, GetModuleHandle(NULL), NULL);

        if (g_AppData.hwndProcessBrowserList)
        {
            SendMessage(g_AppData.hwndProcessBrowserList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            ListViewInsertColumnUtf8(g_AppData.hwndProcessBrowserList, 0, 70, "PID");
            ListViewInsertColumnUtf8(g_AppData.hwndProcessBrowserList, 1, 180, "Process Name");
            ListViewInsertColumnUtf8(g_AppData.hwndProcessBrowserList, 2, 620, "Command Line");
            g_AppData.processBrowserSortColumn = 0;
            g_AppData.processBrowserSortAscending = TRUE;
        }

        RefreshProcessBrowserList();
        return 0;
    }

    case WM_SIZE:
    {
        RECT clientRect;
        if (g_AppData.hwndProcessBrowserList && GetClientRect(hwnd, &clientRect))
            MoveWindow(g_AppData.hwndProcessBrowserList, 0, 0,
                       clientRect.right - clientRect.left,
                       clientRect.bottom - clientRect.top, TRUE);
        return 0;
    }

    case WM_NOTIFY:
    {
        LPNMHDR pnmh = (LPNMHDR)lParam;

        if (pnmh->idFrom == IDC_PROCESS_BROWSER_LIST && pnmh->code == LVN_COLUMNCLICK)
        {
            NMLISTVIEW *pnmlv = (NMLISTVIEW *)lParam;
            if (g_AppData.processBrowserSortColumn == pnmlv->iSubItem)
                g_AppData.processBrowserSortAscending = !g_AppData.processBrowserSortAscending;
            else
            {
                g_AppData.processBrowserSortColumn = pnmlv->iSubItem;
                g_AppData.processBrowserSortAscending = TRUE;
            }

            RefreshProcessBrowserList();
            return 0;
        }
        else if (pnmh->idFrom == IDC_PROCESS_BROWSER_LIST && pnmh->code == NM_DBLCLK)
        {
            NMITEMACTIVATE *pnmia = (NMITEMACTIVATE *)lParam;
            if (pnmia->iItem >= 0)
            {
                char processName[256] = {0};
                char commandLine[4096] = {0};
                LVITEMW lvi = {0};
                DWORD pid;

                lvi.mask = LVIF_PARAM;
                lvi.iItem = pnmia->iItem;
                SendMessageW(g_AppData.hwndProcessBrowserList, LVM_GETITEMW, 0, (LPARAM)&lvi);
                pid = (DWORD)lvi.lParam;

                ListViewGetItemTextUtf8(g_AppData.hwndProcessBrowserList, pnmia->iItem, 1,
                                        processName, sizeof(processName));
                ListViewGetItemTextUtf8(g_AppData.hwndProcessBrowserList, pnmia->iItem, 2,
                                        commandLine, sizeof(commandLine));

                if (processName[0] != '\0')
                {
                    char watchInput[1280] = {0};

                    if (commandLine[0] != '\0' && pid != 0)
                    {
                        char pidName[256] = {0};
                        GetProcessExecutableNameByPid(pid, pidName, sizeof(pidName));

                        if (CompareUtf8Insensitive(pidName, processName) != 0 ||
                            ContainsTextInsensitive(commandLine, " "))
                        {
                            sprintf_s(watchInput, sizeof(watchInput), "cmd:%s", commandLine);
                        }
                    }

                    if (watchInput[0] == '\0')
                        strcpy_s(watchInput, sizeof(watchInput), processName);

                    AddProcess(watchInput);
                }
            }
        }
        else if (pnmh->idFrom == IDC_PROCESS_BROWSER_LIST && pnmh->code == LVN_KEYDOWN)
        {
            NMLVKEYDOWN *pnkd = (NMLVKEYDOWN *)lParam;
            if (pnkd->wVKey == VK_F5)
                RefreshProcessBrowserList();
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_AppData.hwndProcessBrowserList = NULL;
        g_AppData.hwndProcessBrowserWindow = NULL;
        return 0;

    default:
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    const char *CLASS_NAME = "ProcessWatcher";
    const char *EVENT_LOG_CLASS_NAME = "ProcessWatcherEventLog";
    INITCOMMONCONTROLSEX icex = {0};
    HICON hLargeIcon;
    HICON hSmallIcon;
    HWND hwnd;
    HACCEL hAccel = NULL;
    (void)hPrevInstance;
    (void)lpCmdLine;

    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    hLargeIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON),
                                  IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    hSmallIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON),
                                  IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    {
        WNDCLASSEX wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = CLASS_NAME;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon = hLargeIcon;
        wc.hIconSm = hSmallIcon;

        RegisterClassEx(&wc);
    }

    {
        WNDCLASSEX wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = EventLogWindowProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = EVENT_LOG_CLASS_NAME;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon = hLargeIcon;
        wc.hIconSm = hSmallIcon;

        RegisterClassEx(&wc);
    }

    {
        WNDCLASSEX wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ProcessBrowserWindowProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = "ProcessWatcherBrowser";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon = hLargeIcon;
        wc.hIconSm = hSmallIcon;

        RegisterClassEx(&wc);
    }

    hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        "ProcessWatcher",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 250,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        MessageBox(NULL, "Window creation failed!", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hLargeIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmallIcon);

    {
        ACCEL refreshAccel = {0};
        refreshAccel.fVirt = FVIRTKEY;
        refreshAccel.key = VK_F5;
        refreshAccel.cmd = ID_FORCE_REFRESH;
        hAccel = CreateAcceleratorTable(&refreshAccel, 1);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    {
        MSG msg = {0};
        while (GetMessage(&msg, NULL, 0, 0))
        {
            if (!hAccel || !TranslateAccelerator(hwnd, hAccel, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        if (hAccel)
            DestroyAcceleratorTable(hAccel);

        return (int)msg.wParam;
    }
}
