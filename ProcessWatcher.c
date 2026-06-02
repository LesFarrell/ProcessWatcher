#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_PROCESS_NAME 1001
#define IDC_ADD_BUTTON 1002
#define IDC_REMOVE_BUTTON 1003
#define IDC_REFRESH_BUTTON 1004
#define IDC_LISTBOX 1005
#define IDC_AUTO_REFRESH 1006
#define MAX_PROCESSES 100
#define ID_REFRESH_TIMER 1
#define ID_CONTEXT_REMOVE_PROCESS 40001
#define ID_CONTEXT_END_PROCESS 40002
#define IDI_APP_ICON 101
#define PROCESSES_FILE "WatchedProcesses.txt"

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
} WatchedProcess;

typedef struct
{
    WatchedProcess processes[MAX_PROCESSES];
    int count;
    HWND hwndProcessCombo;
    HWND hwndAddButton;
    HWND hwndRemoveButton;
    HWND hwndRefreshButton;
    HWND hwndListView;
    HWND hwndAutoRefresh;
    BOOL bUpdatingComboText;
    int sortColumn;
    BOOL sortAscending;
} AppData;

typedef struct
{
    char name[256];
    DWORD pid;
    int matchRank;
} ComboProcessEntry;

AppData g_AppData = {0};

BOOL IsAutoRefreshEnabled(void)
{
    return g_AppData.hwndAutoRefresh &&
           SendMessage(g_AppData.hwndAutoRefresh, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void UpdateManualRefreshControls(void)
{
    BOOL enableManualControls = !IsAutoRefreshEnabled();

    if (g_AppData.hwndRemoveButton)
        EnableWindow(g_AppData.hwndRemoveButton, enableManualControls);

    if (g_AppData.hwndRefreshButton)
        EnableWindow(g_AppData.hwndRefreshButton, enableManualControls);
}

ULONGLONG FileTimeToUInt64(FILETIME fileTime)
{
    ULARGE_INTEGER value;
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
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

BOOL GetProcessExecutableNameByPid(DWORD pid, char *processName, size_t processNameSize)
{
    char processPath[4096];
    char *baseName;
    DWORD pathSize = sizeof(processPath);
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (!hProcess)
        return FALSE;

    if (!QueryFullProcessImageNameA(hProcess, 0, processPath, &pathSize))
    {
        CloseHandle(hProcess);
        return FALSE;
    }

    CloseHandle(hProcess);

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

void SaveProcessesToFile()
{
    FILE *file = NULL;
    fopen_s(&file, PROCESSES_FILE, "w");

    if (file)
    {
        for (int i = 0; i < g_AppData.count; i++)
            fprintf(file, "%s\n", g_AppData.processes[i].name);

        fclose(file);
    }
}

void LoadProcessesFromFile()
{
    FILE *file = NULL;
    fopen_s(&file, PROCESSES_FILE, "r");

    if (file)
    {
        char processName[256];
        while (fgets(processName, sizeof(processName), file) && g_AppData.count < MAX_PROCESSES)
        {
            TrimProcessName(processName);

            if (strlen(processName) > 0 && !IsWatchedProcessName(processName))
            {
                strcpy_s(g_AppData.processes[g_AppData.count].name,
                         sizeof(g_AppData.processes[0].name), processName);
                g_AppData.count++;
            }
        }
        fclose(file);
    }
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

    if (!hwndListView)
        return;

    GetSelectedProcessName(selectedProcessName, sizeof(selectedProcessName));

    SendMessage(hwndListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(hwndListView);

    for (int i = 0; i < g_AppData.count; i++)
    {
        DWORD pid = 0;
        DWORD memoryMB = 0;
        double cpuPercent = 0.0;
        BOOL running = IsProcessRunning(g_AppData.processes[i].name, &pid, &memoryMB);
        g_AppData.processes[i].running = running;
        g_AppData.processes[i].pid = pid;
        g_AppData.processes[i].memoryMB = memoryMB;

        if (running)
        {
            cpuPercent = GetProcessCpuPercent(&g_AppData.processes[i], pid);
            g_AppData.processes[i].cpuPercent = cpuPercent;
        }
        else
        {
            ResetProcessCpuSample(&g_AppData.processes[i]);
            g_AppData.processes[i].cpuPercent = 0.0;
        }
    }

    SortWatchedProcesses();

    for (int i = 0; i < g_AppData.count; i++)
    {
        char statusText[32];
        char pidText[32];
        char cpuText[32];
        char memoryText[32];

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
        }
    }

    RestoreSelectedProcess(hwndListView, selectedProcessName);

    SendMessage(hwndListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwndListView, NULL, TRUE);
    UpdateWindow(hwndListView);
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

    SaveProcessesToFile();
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

    SaveProcessesToFile();
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

void ShowListViewContextMenu(HWND hwndOwner, HWND hwndListView, POINT screenPoint)
{
    int index = -1;
    HMENU hMenu;
    UINT removeFlags = MF_STRING;
    UINT endProcessFlags = MF_STRING;
    int command;

    if (!hwndListView)
        return;

    if (screenPoint.x == -1 && screenPoint.y == -1)
    {
        RECT itemRect;

        index = GetSelectedProcessIndex();
        if (index == -1 || !ListView_GetItemRect(hwndListView, index, &itemRect, LVIR_BOUNDS))
            return;

        screenPoint.x = itemRect.left;
        screenPoint.y = itemRect.bottom;
        ClientToScreen(hwndListView, &screenPoint);
    }
    else if (!TrySelectListViewItemAtScreenPoint(hwndListView, screenPoint, &index))
    {
        return;
    }

    if (index == -1)
        return;

    if (IsAutoRefreshEnabled())
        removeFlags |= MF_GRAYED;
    if (!g_AppData.processes[index].running || g_AppData.processes[index].pid == 0)
        endProcessFlags |= MF_GRAYED;

    hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    AppendMenu(hMenu, endProcessFlags, ID_CONTEXT_END_PROCESS, TEXT("End Process"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, removeFlags, ID_CONTEXT_REMOVE_PROCESS, TEXT("Remove Selected"));
    command = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y,
                             0, hwndOwner, NULL);
    DestroyMenu(hMenu);

    if (command == ID_CONTEXT_END_PROCESS)
        EndSelectedProcess(hwndOwner);
    else if (command == ID_CONTEXT_REMOVE_PROCESS)
        RemoveSelectedProcess();
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

void LayoutControls(HWND hwnd)
{
    RECT clientRect;
    const int margin = 10;
    const int buttonWidth = 130;
    const int refreshWidth = 95;
    const int autoRefreshWidth = 110;
    const int rowHeight = 25;
    const int rowGap = 10;
    const int bottomSectionHeight = rowHeight + rowGap + rowHeight;

    if (!GetClientRect(hwnd, &clientRect))
        return;

    {
        int clientWidth = clientRect.right - clientRect.left;
        int clientHeight = clientRect.bottom - clientRect.top;
        int bottomTop = clientHeight - margin - bottomSectionHeight;
        int buttonY = clientHeight - margin - rowHeight;
        int autoRefreshY = bottomTop;
        int listHeight = bottomTop - (margin * 2);
        int comboWidth = clientWidth - (margin * 5) - (buttonWidth * 2) - refreshWidth;

        if (comboWidth < 120)
            comboWidth = 120;
        if (listHeight < 80)
            listHeight = 80;

        if (g_AppData.hwndListView)
            MoveWindow(g_AppData.hwndListView, margin, margin,
                       clientWidth - (margin * 2), listHeight, TRUE);

        if (g_AppData.hwndAutoRefresh)
            MoveWindow(g_AppData.hwndAutoRefresh, margin, autoRefreshY,
                       autoRefreshWidth, 20, TRUE);

        if (g_AppData.hwndProcessCombo)
            MoveWindow(g_AppData.hwndProcessCombo, margin, buttonY,
                       comboWidth, 200, TRUE);

        if (g_AppData.hwndAddButton)
            MoveWindow(g_AppData.hwndAddButton, margin + comboWidth + margin, buttonY,
                       buttonWidth, rowHeight, TRUE);

        if (g_AppData.hwndRemoveButton)
            MoveWindow(g_AppData.hwndRemoveButton,
                       clientWidth - margin - refreshWidth - margin - buttonWidth,
                       buttonY, buttonWidth, rowHeight, TRUE);

        if (g_AppData.hwndRefreshButton)
            MoveWindow(g_AppData.hwndRefreshButton, clientWidth - margin - refreshWidth,
                       buttonY, refreshWidth, rowHeight, TRUE);
    }
}

void GetMinimumWindowSize(HWND hwnd, int *minWidth, int *minHeight)
{
    RECT minClientRect = {0, 0, 525, 170};
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    AdjustWindowRectEx(&minClientRect, style, FALSE, exStyle);

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
        g_AppData.sortColumn = 0;
        g_AppData.sortAscending = TRUE;
        LoadProcessesFromFile();

        g_AppData.hwndProcessCombo = CreateWindow(TEXT("COMBOBOX"), TEXT(""),
                                                  WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                                      CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                                  10, 10, 200, 200,
                                                  hwnd, (HMENU)IDC_PROCESS_NAME, GetModuleHandle(NULL), NULL);
        PopulateComboBox(g_AppData.hwndProcessCombo);

        g_AppData.hwndAddButton = CreateWindow(TEXT("BUTTON"), TEXT("Add Process"),
                                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               220, 10, 130, 25,
                                               hwnd, (HMENU)IDC_ADD_BUTTON, GetModuleHandle(NULL), NULL);

        g_AppData.hwndRemoveButton = CreateWindow(TEXT("BUTTON"), TEXT("Remove Selected"),
                                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                  360, 10, 130, 25,
                                                  hwnd, (HMENU)IDC_REMOVE_BUTTON, GetModuleHandle(NULL), NULL);

        g_AppData.hwndRefreshButton = CreateWindow(TEXT("BUTTON"), TEXT("Refresh"),
                                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                   500, 10, 95, 25,
                                                   hwnd, (HMENU)IDC_REFRESH_BUTTON, GetModuleHandle(NULL), NULL);

        CreateWindow(TEXT("BUTTON"), TEXT("Auto-Refresh"),
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     10, 45, 110, 20,
                     hwnd, (HMENU)IDC_AUTO_REFRESH, GetModuleHandle(NULL), NULL);
        g_AppData.hwndAutoRefresh = GetDlgItem(hwnd, IDC_AUTO_REFRESH);
        SendMessage(g_AppData.hwndAutoRefresh, BM_SETCHECK, BST_CHECKED, 0);

        g_AppData.hwndListView = CreateWindow(WC_LISTVIEW, TEXT(""),
                                              WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LVS_REPORT | LVS_SINGLESEL,
                                              10, 75, 490, 250,
                                              hwnd, (HMENU)IDC_LISTBOX, GetModuleHandle(NULL), NULL);

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
        }

        LayoutControls(hwnd);
        UpdateManualRefreshControls();
        RefreshProcessList(g_AppData.hwndListView);
        SetTimer(hwnd, ID_REFRESH_TIMER, 1000, NULL);
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
                SetWindowTextA(hwndCombo, "");
                SetFocus(hwndCombo);
            }
        }
        else if (id == IDC_PROCESS_NAME && (notif == CBN_DROPDOWN || notif == CBN_KILLFOCUS))
        {
            HWND hwndCombo = GetDlgItem(hwnd, IDC_PROCESS_NAME);
            PopulateComboBox(hwndCombo);
        }
        else if (id == IDC_PROCESS_NAME && notif == CBN_EDITCHANGE)
        {
            HWND hwndCombo = GetDlgItem(hwnd, IDC_PROCESS_NAME);

            if (!g_AppData.bUpdatingComboText)
            {
                char typedText[256] = {0};
                DWORD editSelection;
                int selectionStart;
                int selectionEnd;
                BOOL wasDroppedDown;

                GetWindowTextA(hwndCombo, typedText, sizeof(typedText));
                editSelection = (DWORD)SendMessage(hwndCombo, CB_GETEDITSEL, 0, 0);
                selectionStart = LOWORD(editSelection);
                selectionEnd = HIWORD(editSelection);
                wasDroppedDown = (BOOL)SendMessage(hwndCombo, CB_GETDROPPEDSTATE, 0, 0);

                g_AppData.bUpdatingComboText = TRUE;
                PopulateComboBoxFiltered(hwndCombo, typedText);
                SetWindowTextA(hwndCombo, typedText);
                SendMessage(hwndCombo, CB_SETEDITSEL, 0, MAKELPARAM(selectionStart, selectionEnd));
                g_AppData.bUpdatingComboText = FALSE;

                SendMessage(hwndCombo, CB_SHOWDROPDOWN,
                            (WPARAM)(typedText[0] != '\0' || wasDroppedDown), 0);
            }
        }
        else if (id == IDC_REMOVE_BUTTON)
        {
            RemoveSelectedProcess();
        }
        else if (id == IDC_REFRESH_BUTTON)
        {
            RefreshProcessList(g_AppData.hwndListView);
        }
        else if (id == IDC_AUTO_REFRESH)
        {
            if (IsAutoRefreshEnabled())
                SetTimer(hwnd, ID_REFRESH_TIMER, 1000, NULL);
            else
                KillTimer(hwnd, ID_REFRESH_TIMER);

            UpdateManualRefreshControls();
            InvalidateRect(g_AppData.hwndListView, NULL, TRUE);
            UpdateWindow(g_AppData.hwndListView);
        }
        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == ID_REFRESH_TIMER)
            RefreshProcessList(g_AppData.hwndListView);
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
        break;
    }

    case WM_CONTEXTMENU:
    {
        if ((HWND)wParam == g_AppData.hwndListView)
        {
            POINT screenPoint;
            screenPoint.x = GET_X_LPARAM(lParam);
            screenPoint.y = GET_Y_LPARAM(lParam);
            ShowListViewContextMenu(hwnd, g_AppData.hwndListView, screenPoint);
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
        SaveProcessesToFile();
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
    (void)hPrevInstance;
    (void)lpCmdLine;

    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
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
        HWND hwnd = CreateWindowEx(
            0,
            CLASS_NAME,
            "ProcessWatcher",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 640, 270,
            NULL, NULL, hInstance, NULL);

        if (!hwnd)
        {
            MessageBox(NULL, "Window creation failed!", "Error", MB_OK | MB_ICONERROR);
            return 1;
        }

        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hLargeIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmallIcon);

        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }

    {
        MSG msg = {0};
        while (GetMessage(&msg, NULL, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        return (int)msg.wParam;
    }
}
