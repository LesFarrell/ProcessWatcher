#include <windows.h>
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

AppData g_AppData = {0};

BOOL IsAutoRefreshEnabled(void)
{
    return g_AppData.hwndAutoRefresh &&
           SendMessage(g_AppData.hwndAutoRefresh, BM_GETCHECK, 0, 0) == BST_CHECKED;
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

void ExtractProcessNameFromDisplayText(const char *displayText, char *processName, size_t processNameSize)
{
    const char *suffix = strrchr(displayText, '(');

    if (strcpy_s(processName, processNameSize, displayText) != 0)
        return;

    if (suffix != NULL && suffix > displayText && suffix[-1] == ' ')
    {
        size_t nameLength = (size_t)(suffix - displayText - 1);
        if (nameLength < processNameSize)
            processName[nameLength] = '\0';
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

void RefreshProcessList(HWND hwndListView)
{
    if (!hwndListView)
        return;

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

void PopulateComboBoxFiltered(HWND hwndCombo, const char *filterText)
{
    char currentText[256] = {0};
    size_t filterLength = 0;

    GetWindowTextA(hwndCombo, currentText, sizeof(currentText));
    if (filterText != NULL)
        filterLength = strlen(filterText);

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
                    char displayText[320];

                    if (!GetProcessExecutableNameByPid(pe32.th32ProcessID, processName, sizeof(processName)))
                        strcpy_s(processName, sizeof(processName), pe32.szExeFile);

                    if (filterLength > 0 && _strnicmp(processName, filterText, filterLength) != 0)
                        continue;

                    sprintf_s(displayText, sizeof(displayText), "%s (%lu)",
                              processName, pe32.th32ProcessID);

                    {
                        LRESULT index = SendMessage(hwndCombo, CB_ADDSTRING, 0, (LPARAM)displayText);
                        if (index != CB_ERR && index != CB_ERRSPACE)
                            SendMessage(hwndCombo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)pe32.th32ProcessID);
                    }
                } while (Process32Next(hSnapshot, &pe32));
            }

            CloseHandle(hSnapshot);
        }
    }

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
                                                  WS_CHILD | WS_VISIBLE | WS_BORDER | CBS_DROPDOWN | CBS_AUTOHSCROLL,
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
                int typedLength;

                GetWindowTextA(hwndCombo, typedText, sizeof(typedText));
                typedLength = (int)strlen(typedText);

                g_AppData.bUpdatingComboText = TRUE;
                PopulateComboBoxFiltered(hwndCombo, typedText);
                g_AppData.bUpdatingComboText = FALSE;

                if (typedLength > 0)
                {
                    int itemCount = (int)SendMessage(hwndCombo, CB_GETCOUNT, 0, 0);

                    for (int i = 0; i < itemCount; i++)
                    {
                        char matchedDisplayText[320] = {0};
                        char matchedProcessName[256] = {0};
                        SendMessage(hwndCombo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)matchedDisplayText);
                        ExtractProcessNameFromDisplayText(matchedDisplayText,
                                                          matchedProcessName, sizeof(matchedProcessName));

                        if (_strnicmp(matchedProcessName, typedText, typedLength) == 0)
                        {
                            g_AppData.bUpdatingComboText = TRUE;
                            SetWindowTextA(hwndCombo, matchedProcessName);
                            SendMessage(hwndCombo, CB_SETEDITSEL, 0, MAKELPARAM(typedLength, -1));
                            SendMessage(hwndCombo, CB_SETCURSEL, (WPARAM)i, 0);
                            SendMessage(hwndCombo, CB_SETTOPINDEX, (WPARAM)i, 0);
                            SendMessage(hwndCombo, CB_SHOWDROPDOWN, TRUE, 0);
                            g_AppData.bUpdatingComboText = FALSE;
                            break;
                        }
                    }
                }
                else
                {
                    SendMessage(hwndCombo, CB_SHOWDROPDOWN, FALSE, 0);
                }
            }
        }
        else if (id == IDC_REMOVE_BUTTON)
        {
            int index = (int)SendMessage(g_AppData.hwndListView, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
            if (index != -1)
                RemoveProcess(index);
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

    case WM_SIZE:
    {
        (void)lParam;
        LayoutControls(hwnd);
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
            CW_USEDEFAULT, CW_USEDEFAULT, 640, 370,
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
