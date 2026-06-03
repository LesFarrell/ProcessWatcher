#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_PROCESS_NAME 1001
#define IDC_ADD_BUTTON 1002
#define IDC_REMOVE_BUTTON 1003
#define IDC_LISTBOX 1004
#define IDC_STATUS_BAR 1005
#define MAX_PROCESSES 100
#define COLUMN_COUNT 6
#define ID_REFRESH_TIMER 1
#define ID_COMBO_FILTER_TIMER 2
#define COMBO_FILTER_DELAY_MS 300
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
#define IDI_APP_ICON 101
#define NOTIFICATION_ICON_ID 1
#define SETTINGS_FILE_NAME "ProcessWatcher.ini"
#define RUN_REGISTRY_PATH "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_REGISTRY_VALUE "ProcessWatcher"
#define DEFAULT_REFRESH_INTERVAL_MS 1000

typedef struct
{
    char name[256];
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
} WatchedProcess;

typedef struct
{
    WatchedProcess processes[MAX_PROCESSES];
    int count;
    HWND hwndProcessCombo;
    HWND hwndAddButton;
    HWND hwndRemoveButton;
    HWND hwndListView;
    HWND hwndStatusBar;
    BOOL bUpdatingComboText;
    BOOL autoRefreshEnabled;
    BOOL startWithWindows;
    BOOL notifyOnStop;
    BOOL notificationIconAdded;
    SYSTEMTIME lastRefreshLocalTime;
    BOOL hasLastRefresh;
    int sortColumn;
    BOOL sortAscending;
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

AppData g_AppData = {0};

LRESULT CALLBACK ComboOpenOnClickProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
BOOL IsWindowTopmost(HWND hwnd);
void ToggleWatcherTopmost(HWND hwndOwner, BOOL makeTopmost);
BOOL SaveSettingsToIni(HWND hwndOwner);
void LoadSettingsFromIni(HWND hwndOwner);
void ShowSettingsSaveError(HWND hwndOwner);
void SaveSettingsWithFeedback(HWND hwndOwner);
BOOL GetProcessExecutablePathByPid(DWORD pid, char *processPath, size_t processPathSize);
void UpdateStatusBar(void);
void ApplyAutoRefreshTimer(HWND hwnd);
BOOL SetStartWithWindowsEnabled(BOOL enabled);
void ApplySavedWindowPlacement(HWND hwnd);
HMENU CreateMainWindowMenu(void);
void UpdateOptionsMenuState(HWND hwnd);
void UpdateRemoveButtonState(void);
void UpdateListSortIndicators(void);
BOOL EnsureNotificationIcon(HWND hwnd);
void RemoveNotificationIcon(HWND hwnd);
void ShowStopNotification(HWND hwnd, const char *message);
BOOL GetKnownProcessExecutablePath(const WatchedProcess *process, char *processPath, size_t processPathSize);
void StartSelectedProcess(HWND hwndOwner);

BOOL IsAutoRefreshEnabled(void)
{
    return g_AppData.autoRefreshEnabled;
}

LRESULT CALLBACK ComboOpenOnClickProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    (void)dwRefData;

    if ((uMsg == WM_LBUTTONUP || uMsg == WM_LBUTTONDBLCLK) &&
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

BOOL IsWatchedProcessName(const char *processName)
{
    for (int i = 0; i < g_AppData.count; i++)
    {
        if (_stricmp(g_AppData.processes[i].name, processName) == 0)
            return TRUE;
    }

    return FALSE;
}

BOOL ContainsTextInsensitive(const char *text, const char *pattern)
{
    size_t patternLength;

    if (!text || !pattern)
        return FALSE;

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

int GetComboMatchRank(const char *processName, const char *filterText)
{
    size_t filterLength;

    if (!filterText)
        return 0;

    filterLength = strlen(filterText);
    if (filterLength == 0)
        return 0;

    if (_stricmp(processName, filterText) == 0)
        return 0;

    if (_strnicmp(processName, filterText, filterLength) == 0)
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
        result = _stricmp(entryLeft->name, entryRight->name);
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

BOOL GetProcessExecutableNameByPid(DWORD pid, char *processName, size_t processNameSize)
{
    char processPath[4096];
    char *baseName;

    if (!GetProcessExecutablePathByPid(pid, processPath, sizeof(processPath)))
        return FALSE;

    baseName = strrchr(processPath, '\\');
    if (baseName)
        baseName++;
    else
        baseName = processPath;

    return strcpy_s(processName, processNameSize, baseName) == 0;
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

BOOL IsProcessRunning(const char *processName, DWORD *pPID, DWORD *pMemoryMB)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return FALSE;

    PROCESSENTRY32 pe32 = {0};
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe32))
    {
        do
        {
            if (_stricmp(pe32.szExeFile, processName) == 0)
            {
                if (pPID)
                    *pPID = pe32.th32ProcessID;
                if (pMemoryMB)
                    *pMemoryMB = GetProcessMemoryMB(pe32.th32ProcessID);
                CloseHandle(hSnapshot);
                return TRUE;
            }
        } while (Process32Next(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return FALSE;
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

    result = RegCreateKeyExA(HKEY_CURRENT_USER, RUN_REGISTRY_PATH, 0, NULL, 0,
                             KEY_SET_VALUE, NULL, &hKey, NULL);
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
        result = RegSetValueExA(hKey, RUN_REGISTRY_VALUE, 0, REG_SZ,
                                (const BYTE *)quotedPath, (DWORD)(strlen(quotedPath) + 1));
    }
    else
    {
        result = RegDeleteValueA(hKey, RUN_REGISTRY_VALUE);
        if (result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
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
    if (!SaveSettingsToIni(hwndOwner) || !SetStartWithWindowsEnabled(g_AppData.startWithWindows))
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
    g_AppData.startWithWindows = GetPrivateProfileIntA("General", "StartWithWindows", 0, settingsPath) != 0;
    g_AppData.notifyOnStop = GetPrivateProfileIntA("General", "NotifyOnStop", 0, settingsPath) != 0;
    g_AppData.sortColumn = GetPrivateProfileIntA("General", "SortColumn", 0, settingsPath);
    if (g_AppData.sortColumn < 0 || g_AppData.sortColumn >= COLUMN_COUNT)
        g_AppData.sortColumn = 0;
    g_AppData.sortAscending = GetPrivateProfileIntA("General", "SortAscending", 1, settingsPath) != 0;
    count = GetPrivateProfileIntA("WatchedProcesses", "Count", 0, settingsPath);

    for (UINT i = 0; i < count && g_AppData.count < MAX_PROCESSES; i++)
    {
        char keyName[32];
        char processName[256] = {0};

        sprintf_s(keyName, sizeof(keyName), "Item%u", i);
        GetPrivateProfileStringA("WatchedProcesses", keyName, "",
                                 processName, (DWORD)sizeof(processName), settingsPath);
        TrimProcessName(processName);
        if (strlen(processName) > 0 && !IsWatchedProcessName(processName))
        {
            strcpy_s(g_AppData.processes[g_AppData.count].name,
                     sizeof(g_AppData.processes[0].name), processName);
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
        result = _stricmp(processLeft->name, processRight->name);
        break;
    }

    if (result == 0)
        result = _stricmp(processLeft->name, processRight->name);
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
    HMENU hOptionsMenu;

    if (!hMenuBar)
        return NULL;

    hFileMenu = CreatePopupMenu();
    if (!hFileMenu)
    {
        DestroyMenu(hMenuBar);
        return NULL;
    }

    hOptionsMenu = CreatePopupMenu();
    if (!hOptionsMenu)
    {
        DestroyMenu(hFileMenu);
        DestroyMenu(hMenuBar);
        return NULL;
    }

    AppendMenu(hFileMenu, MF_STRING, ID_FORCE_REFRESH, TEXT("Refresh Now\tF5"));
    AppendMenu(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_EXIT, TEXT("Exit"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_OPTIONS_AUTO_REFRESH, TEXT("Auto-Refresh"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_OPTIONS_START_WITH_WINDOWS, TEXT("Start with Windows"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_OPTIONS_NOTIFY_ON_STOP, TEXT("Notify on Stop"));
    AppendMenu(hOptionsMenu, MF_STRING, ID_CONTEXT_TOGGLE_TOPMOST, TEXT("Keep On Top"));

    if (!AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, TEXT("File")) ||
        !AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hOptionsMenu, TEXT("Options")))
    {
        DestroyMenu(hFileMenu);
        DestroyMenu(hOptionsMenu);
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

    if (!Shell_NotifyIconA(NIM_ADD, &nid))
        return FALSE;

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconA(NIM_SETVERSION, &nid);
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
    Shell_NotifyIconA(NIM_DELETE, &nid);
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
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

void UpdateStatusBar(void)
{
    char statusText[256];
    int runningCount = 0;

    if (!g_AppData.hwndStatusBar)
        return;

    for (int i = 0; i < g_AppData.count; i++)
    {
        if (g_AppData.processes[i].running)
            runningCount++;
    }

    sprintf_s(statusText, sizeof(statusText),
              "Watching: %d  Running: %d  Refresh: %s",
              g_AppData.count, runningCount,
              IsAutoRefreshEnabled() ? "Auto" : "Paused");
    SendMessageA(g_AppData.hwndStatusBar, SB_SETTEXTA, 0, (LPARAM)statusText);
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
        if (_stricmp(g_AppData.processes[i].name, processName) == 0)
        {
            ListView_SetItemState(hwndListView, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetSelectionMark(hwndListView, i);
            ListView_EnsureVisible(hwndListView, i, FALSE);
            break;
        }
    }
}

void RefreshProcessList(HWND hwndListView)
{
    char selectedProcessName[256] = {0};
    int stoppedCount = 0;
    char firstStoppedProcess[256] = {0};
    HWND hwndOwner;

    if (!hwndListView)
        return;

    hwndOwner = GetParent(hwndListView);
    GetSelectedProcessName(selectedProcessName, sizeof(selectedProcessName));

    SendMessage(hwndListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hwndListView);

    for (int i = 0; i < g_AppData.count; i++)
    {
        DWORD pid = 0;
        DWORD memoryMB = 0;
        double cpuPercent = 0.0;
        BOOL wasRunning = g_AppData.processes[i].running;
        BOOL running = IsProcessRunning(g_AppData.processes[i].name, &pid, &memoryMB);
        g_AppData.processes[i].running = running;
        g_AppData.processes[i].pid = pid;
        g_AppData.processes[i].memoryMB = memoryMB;

        if (running)
        {
            char processPath[MAX_PATH] = {0};

            cpuPercent = GetProcessCpuPercent(&g_AppData.processes[i], pid);
            g_AppData.processes[i].cpuPercent = cpuPercent;
            UpdateProcessLastSeen(&g_AppData.processes[i]);
            if (GetProcessExecutablePathByPid(pid, processPath, sizeof(processPath)))
                strcpy_s(g_AppData.processes[i].executablePath, sizeof(g_AppData.processes[i].executablePath), processPath);
        }
        else
        {
            ResetProcessCpuSample(&g_AppData.processes[i]);
            g_AppData.processes[i].cpuPercent = 0.0;
            if (wasRunning)
            {
                if (stoppedCount == 0)
                    strcpy_s(firstStoppedProcess, sizeof(firstStoppedProcess), g_AppData.processes[i].name);
                stoppedCount++;
            }
        }
    }

    SortWatchedProcesses();

    for (int i = 0; i < g_AppData.count; i++)
    {
        char statusText[32];
        char pidText[32];
        char cpuText[32];
        char memoryText[32];
        char lastSeenText[32];

        if (g_AppData.processes[i].running)
        {
            strcpy_s(statusText, sizeof(statusText), "RUNNING");
            sprintf_s(pidText, sizeof(pidText), "%lu", g_AppData.processes[i].pid);
            sprintf_s(cpuText, sizeof(cpuText), "%.1f%%", g_AppData.processes[i].cpuPercent);
            sprintf_s(memoryText, sizeof(memoryText), "%lu MB", g_AppData.processes[i].memoryMB);
        }
        else
        {
            strcpy_s(statusText, sizeof(statusText), "STOPPED");
            strcpy_s(pidText, sizeof(pidText), "-");
            strcpy_s(cpuText, sizeof(cpuText), "-");
            strcpy_s(memoryText, sizeof(memoryText), "-");
        }

        FormatProcessLastSeen(&g_AppData.processes[i], lastSeenText, sizeof(lastSeenText));

        LVITEM lvi = {0};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = g_AppData.processes[i].name;
        lvi.lParam = g_AppData.processes[i].running ? 1 : 0;

        {
            int index = ListView_InsertItem(hwndListView, &lvi);
            if (index == -1)
                continue;

            ListView_SetItemText(hwndListView, index, 1, statusText);
            ListView_SetItemText(hwndListView, index, 2, pidText);
            ListView_SetItemText(hwndListView, index, 3, cpuText);
            ListView_SetItemText(hwndListView, index, 4, memoryText);
            ListView_SetItemText(hwndListView, index, 5, lastSeenText);
        }
    }

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
                      "%s has stopped running.", firstStoppedProcess);
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
    char normalizedProcessName[256] = {0};

    if (g_AppData.count >= MAX_PROCESSES)
    {
        MessageBox(NULL, "Maximum number of processes reached!", "Limit", MB_OK | MB_ICONWARNING);
        return;
    }

    strcpy_s(normalizedProcessName, sizeof(normalizedProcessName), processName);
    TrimProcessName(normalizedProcessName);
    NormalizeWatchedProcessName(normalizedProcessName);

    if (normalizedProcessName[0] == '\0')
        return;

    if (IsWatchedProcessName(normalizedProcessName))
    {
        MessageBox(NULL, "This process is already being watched!", "Duplicate", MB_OK | MB_ICONWARNING);
        return;
    }

    strcpy_s(g_AppData.processes[g_AppData.count].name,
             sizeof(g_AppData.processes[0].name), normalizedProcessName);
    g_AppData.count++;

    SaveSettingsWithFeedback(GetParent(g_AppData.hwndListView));
    RefreshProcessList(g_AppData.hwndListView);
}

void RemoveProcess(int index)
{
    if (index < 0 || index >= g_AppData.count)
        return;

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

    result = ShellExecuteA(hwndOwner, "open", processPath, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
    {
        MessageBoxA(hwndOwner, "Failed to start the selected process.",
                    "Start Process", MB_OK | MB_ICONERROR);
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
            PROCESSENTRY32 pe32 = {0};
            pe32.dwSize = sizeof(PROCESSENTRY32);

            if (Process32First(hSnapshot, &pe32))
            {
                do
                {
                    char processName[256];
                    int matchRank;

                    if (!GetProcessExecutableNameByPid(pe32.th32ProcessID, processName, sizeof(processName)))
                        strcpy_s(processName, sizeof(processName), pe32.szExeFile);

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
                } while (Process32Next(hSnapshot, &pe32));
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

        index = SendMessage(hwndCombo, CB_ADDSTRING, 0, (LPARAM)displayText);
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
        int comboWidth = clientWidth - (margin * 4) - (buttonWidth * 2);

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
    RECT minClientRect = {0, 0, 525, 170};
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

        g_AppData.hwndRemoveButton = CreateWindow(TEXT("BUTTON"), TEXT("Remove Selected"),
                                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                  360, 10, 130, 25,
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

        {
            LVCOLUMN lvc = {0};
            lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
            lvc.fmt = LVCFMT_LEFT;

            lvc.cx = 180;
            lvc.pszText = TEXT("Process Name");
            SendMessage(g_AppData.hwndListView, LVM_INSERTCOLUMN, 0, (LPARAM)&lvc);

            lvc.cx = 80;
            lvc.pszText = TEXT("Status");
            SendMessage(g_AppData.hwndListView, LVM_INSERTCOLUMN, 1, (LPARAM)&lvc);

            lvc.cx = 70;
            lvc.pszText = TEXT("PID");
            SendMessage(g_AppData.hwndListView, LVM_INSERTCOLUMN, 2, (LPARAM)&lvc);

            lvc.cx = 70;
            lvc.pszText = TEXT("CPU %");
            SendMessage(g_AppData.hwndListView, LVM_INSERTCOLUMN, 3, (LPARAM)&lvc);

            lvc.cx = 90;
            lvc.pszText = TEXT("Memory");
            SendMessage(g_AppData.hwndListView, LVM_INSERTCOLUMN, 4, (LPARAM)&lvc);

            lvc.cx = 150;
            lvc.pszText = TEXT("Last Seen");
            SendMessage(g_AppData.hwndListView, LVM_INSERTCOLUMN, 5, (LPARAM)&lvc);
        }

        for (int i = 0; i < COLUMN_COUNT; i++)
        {
            if (g_AppData.savedColumnWidths[i] > 0)
                ListView_SetColumnWidth(g_AppData.hwndListView, i, g_AppData.savedColumnWidths[i]);
        }

        UpdateListSortIndicators();
        ApplySavedWindowPlacement(hwnd);
        UpdateOptionsMenuState(hwnd);
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
        else if (id == ID_FILE_EXIT)
        {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
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
        else if (id == ID_CONTEXT_TOGGLE_TOPMOST)
        {
            ToggleWatcherTopmost(hwnd, !IsWindowTopmost(hwnd));
            UpdateOptionsMenuState(hwnd);
            SaveSettingsWithFeedback(hwnd);
        }
        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == ID_REFRESH_TIMER)
            RefreshProcessList(g_AppData.hwndListView);
        else if (wParam == ID_COMBO_FILTER_TIMER)
        {
            KillTimer(hwnd, ID_COMBO_FILTER_TIMER);
            ApplyComboBoxFilter(g_AppData.hwndProcessCombo);
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
        RemoveNotificationIcon(hwnd);
        SaveSettingsWithFeedback(hwnd);
        PostQuitMessage(0);
        return 0;
    }

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    const char *CLASS_NAME = "ProcessWatcher";
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

    hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        "ProcessWatcher",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 340,
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
