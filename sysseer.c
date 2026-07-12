#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")

#define PORT 9090
#define BUFSIZE 8192
#define MAX_PROCESSES 1024

typedef struct {
    DWORD pid;
    char name[260];
    DWORD64 workingSet;
    DWORD64 peakWorkingSet;
    double cpuPercent;
    FILETIME createTime;
    FILETIME userTime;
    FILETIME kernelTime;
} ProcessInfo;

typedef NTSTATUS (NTAPI *pNtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

#define SystemProcessInformation 5

typedef struct {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER SpareLi1;
    LARGE_INTEGER SpareLi2;
    LARGE_INTEGER SpareLi3;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} SYSTEM_PROCESS_INFORMATION, *PSYSTEM_PROCESS_INFORMATION;

char* get_memory_status(DWORDLONG total, DWORDLONG free) {
    static char buf[64];
    double used = (double)(total - free) / (1024*1024*1024);
    double tot = (double)total / (1024*1024*1024);
    double pct = 100.0 * (1.0 - (double)free / total);
    snprintf(buf, sizeof(buf), "%.1f/%.1f GB (%.0f%%)", used, tot, pct);
    return buf;
}

char* get_disk_info(void) {
    static char buf[1024];
    char drives[256];
    DWORD len = GetLogicalDriveStringsA(sizeof(drives), drives);
    buf[0] = 0;
    char* p = drives;
    int count = 0;
    while (*p && count < 6) {
        char type[32] = "?";
        UINT dt = GetDriveTypeA(p);
        switch(dt) {
            case DRIVE_FIXED: strcpy(type, "SSD/HDD"); break;
            case DRIVE_REMOVABLE: strcpy(type, "USB"); break;
            case DRIVE_CDROM: strcpy(type, "CDROM"); break;
            case DRIVE_RAMDISK: strcpy(type, "RAM"); break;
            default: strcpy(type, "NET"); break;
        }
        if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE) {
            char vol[64];
            if (GetVolumeInformationA(p, vol, sizeof(vol), NULL, NULL, NULL, NULL, 0)) {
                char tmp[256];
                DWORDLONG total = 0, freeb = 0;
                GetDiskFreeSpaceExA(p, NULL, (PULARGE_INTEGER)&total, (PULARGE_INTEGER)&freeb);
                double gb_total = (double)total / (1024*1024*1024);
                double gb_free = (double)freeb / (1024*1024*1024);
                double used_pct = 100.0 * (1.0 - gb_free / gb_total);
                snprintf(tmp, sizeof(tmp), "%s (%s) %.1f/%.1f GB (%.0f%%)  ", p, vol, gb_total-gb_free, gb_total, used_pct);
                strcat(buf, tmp);
                count++;
            }
        }
        p += strlen(p) + 1;
    }
    return buf;
}

void get_system_id(SOCKET client) {
    char resp[BUFSIZE];
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    char compName[MAX_COMPUTERNAME_LENGTH+1];
    DWORD compLen = sizeof(compName);
    GetComputerNameA(compName, &compLen);

    char userName[256];
    DWORD userLen = sizeof(userName);
    GetUserNameA(userName, &userLen);

    OSVERSIONINFOEXA osvi = {sizeof(osvi)};
    GetVersionExA((LPOSVERSIONINFOA)&osvi);

    char arch[16];
    switch(si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: strcpy(arch, "x64"); break;
        case PROCESSOR_ARCHITECTURE_INTEL: strcpy(arch, "x86"); break;
        case PROCESSOR_ARCHITECTURE_ARM64: strcpy(arch, "ARM64"); break;
        default: strcpy(arch, "?"); break;
    }

    char timebuf[64];
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

    DWORD mhz = 0;
    DWORD cbData = sizeof(mhz);
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "~MHz", NULL, NULL, (LPBYTE)&mhz, &cbData);
        RegCloseKey(hKey);
    }

    snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n\r\n"
        "=== sysseer v1.0 ===\n"
        "host: %s\n"
        "user: %s\n"
        "time: %s\n"
        "os: Windows %d.%d build %d\n"
        "arch: %s\n"
        "cpu: %d cores, %d logical, ~%d MHz\n"
        "ram: %s\n"
        "disks: %s\n",
        compName, userName, timebuf,
        osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber,
        arch,
        si.dwNumberOfProcessors, si.dwNumberOfProcessors, mhz,
        get_memory_status(ms.ullTotalPhys, ms.ullAvailPhys),
        get_disk_info()
    );
    send(client, resp, strlen(resp), 0);
}

void get_running_processes(SOCKET client, int detailed) {
    char buf[BUFSIZE];
    int offset = 0;
    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");

    HMODULE hModules[1024];
    DWORD needed;
    DWORD procs[1024];
    DWORD cb;
    if (!EnumProcesses(procs, sizeof(procs), &cb)) {
        send(client, "HTTP/1.1 500 ERR\r\n\r\nfailed", 27, 0);
        return;
    }
    DWORD count = cb / sizeof(DWORD);

    for (DWORD i = 0; i < count && i < 80; i++) {
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, procs[i]);
        if (h) {
            char name[260];
            DWORD nameSize = sizeof(name);
            if (QueryFullProcessImageNameA(h, 0, name, &nameSize)) {
                char* fn = strrchr(name, '\\');
                fn = fn ? fn+1 : name;
                PROCESS_MEMORY_COUNTERS pmc;
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
                    double mb = (double)pmc.WorkingSetSize / (1024*1024);
                    if (detailed) {
                        offset += snprintf(buf + offset, sizeof(buf) - offset,
                            "%-6d %-25s %7.1f MB\n", procs[i], fn, mb);
                    } else {
                        offset += snprintf(buf + offset, sizeof(buf) - offset,
                            "%-6d %s\n", procs[i], fn);
                    }
                }
            }
            CloseHandle(h);
        }
        if (offset > sizeof(buf) - 500) break;
    }
    send(client, buf, offset, 0);
}

void get_network_info(SOCKET client) {
    char buf[BUFSIZE];
    int offset = 0;
    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");

    PIP_ADAPTER_INFO pAdapterInfo = NULL;
    ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
    pAdapterInfo = (IP_ADAPTER_INFO*)malloc(ulOutBufLen);
    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO*)malloc(ulOutBufLen);
    }
    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == NO_ERROR) {
        PIP_ADAPTER_INFO p = pAdapterInfo;
        while (p) {
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                "%s (%s)\n  IP: %s\n  MAC: %02X-%02X-%02X-%02X-%02X-%02X\n  GW: %s\n\n",
                p->Description, p->AdapterName,
                p->IpAddressList.IpAddress.String,
                p->Address[0], p->Address[1], p->Address[2],
                p->Address[3], p->Address[4], p->Address[5],
                p->GatewayList.IpAddress.String);
            p = p->Next;
            if (offset > sizeof(buf) - 500) break;
        }
    }
    free(pAdapterInfo);
    send(client, buf, offset, 0);
}

void get_resource_monitor(SOCKET client) {
    char buf[BUFSIZE];
    int offset = 0;
    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "=== RESOURCE MONITOR (live) ===\n"
        "Memory: %s\n",
        get_memory_status(ms.ullTotalPhys, ms.ullAvailPhys));

    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    static FILETIME prevIdle, prevKernel, prevUser;
    static int firstRun = 1;

    if (!firstRun) {
        ULONGLONG idleDiff = ((ULONGLONG)idle.dwHighDateTime << 32 | idle.dwLowDateTime) -
                             ((ULONGLONG)prevIdle.dwHighDateTime << 32 | prevIdle.dwLowDateTime);
        ULONGLONG kernelDiff = ((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) -
                               ((ULONGLONG)prevKernel.dwHighDateTime << 32 | prevKernel.dwLowDateTime);
        ULONGLONG userDiff = ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime) -
                             ((ULONGLONG)prevUser.dwHighDateTime << 32 | prevUser.dwLowDateTime);
        ULONGLONG total = kernelDiff + userDiff;
        if (total > 0) {
            double cpu = 100.0 - (100.0 * idleDiff / total);
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                "CPU: %.1f%%\n", cpu);
        }
    } else {
        firstRun = 0;
    }

    prevIdle = idle;
    prevKernel = kernel;
    prevUser = user;

    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "Disks: %s\n", get_disk_info());

    send(client, buf, offset, 0);
}

void handle_client(SOCKET client) {
    char req[BUFSIZE];
    int n = recv(client, req, sizeof(req)-1, 0);
    if (n <= 0) { closesocket(client); return; }
    req[n] = 0;

    char method[16], path[256];
    sscanf(req, "%15s %255s", method, path);

    if (strcmp(path, "/") == 0 || strcmp(path, "/status") == 0) {
        get_system_id(client);
    } else if (strcmp(path, "/ps") == 0) {
        get_running_processes(client, 0);
    } else if (strcmp(path, "/ps/verbose") == 0) {
        get_running_processes(client, 1);
    } else if (strcmp(path, "/net") == 0) {
        get_network_info(client);
    } else if (strcmp(path, "/monitor") == 0) {
        get_resource_monitor(client);
    } else {
        char* resp = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\navailable endpoints:\n"
                     "  /          - system info\n"
                     "  /ps        - running processes\n"
                     "  /ps/verbose - processes with memory\n"
                     "  /net       - network interfaces\n"
                     "  /monitor   - live resource usage\n";
        send(client, resp, strlen(resp), 0);
    }
    closesocket(client);
}

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("[sysseer] WSAStartup failed\n");
        return 1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        printf("[sysseer] socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[sysseer] bind failed on port %d: %d\n", PORT, WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 1;
    }

    listen(server, SOMAXCONN);
    printf("[sysseer] listening on http://localhost:%d\n", PORT);
    printf("[sysseer] api:\n");
    printf("  GET /          - system overview\n");
    printf("  GET /ps        - process list\n");
    printf("  GET /net       - network interfaces\n");
    printf("  GET /monitor   - live resource usage\n");

    while (1) {
        SOCKET client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        handle_client(client);
    }

    closesocket(server);
    WSACleanup();
    return 0;
}
