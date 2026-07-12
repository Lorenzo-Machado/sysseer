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

#define PORT            9090
#define BUFSIZE         16384
#define MAX_PATH_LEN    1024
#define MAX_RESP        16384
#define SOCK_TIMEOUT    5000
#define ACCEPT_BACKLOG  10

static const char* RESP_OK_400 = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n400 bad request";
static const char* RESP_OK_404 = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n404 not found\n\nendpoints:\n  /          - system info\n  /ps        - process list\n  /ps/verbose - processes with memory\n  /net       - network interfaces\n  /monitor   - live resource usage\n";
static const char* RESP_OK_413 = "HTTP/1.1 413 Too Long\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n413 request too long";
static const char* RESP_OK_500 = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n500 internal error";

typedef NTSTATUS (NTAPI *pNtQuerySystemInformation)(
    ULONG, PVOID, ULONG, PULONG);

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
} SYSTEM_PROCESS_INFORMATION;

typedef struct {
    CRITICAL_SECTION lock;
    ULONGLONG prevIdle;
    ULONGLONG prevKernel;
    ULONGLONG prevUser;
    int initialized;
    double lastCpu;
} CpuMonitor;

static CpuMonitor g_cpu = {0};

static int safe_send(SOCKET s, const char* data, int len) {
    if (!data || len <= 0) return -1;
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return sent;
}

static int send_response(SOCKET s, const char* body) {
    if (!body) return -1;
    int len = strlen(body);
    return safe_send(s, body, len);
}

static char* get_time_str(void) {
    static char buf[32];
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    if (tm) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    return buf;
}

static char* mem_status(DWORDLONG total, DWORDLONG free) {
    static char buf[64];
    double used_gb = (double)(total - free) / (1024.0*1024*1024);
    double tot_gb = (double)total / (1024.0*1024*1024);
    double pct = (total > 0) ? 100.0 * (1.0 - (double)free / total) : 0.0;
    snprintf(buf, sizeof(buf), "%.1f/%.1f GB (%.0f%%)", used_gb, tot_gb, pct);
    return buf;
}

static char* disk_info(void) {
    static char buf[MAX_RESP];
    char drives[256];
    DWORD len = GetLogicalDriveStringsA(sizeof(drives), drives);
    buf[0] = 0;
    if (len == 0 || len > sizeof(drives)) return buf;

    char* p = drives;
    int count = 0;
    while (*p && count < 6) {
        char type[16] = "?";
        UINT dt = GetDriveTypeA(p);
        switch(dt) {
            case DRIVE_FIXED:     strcpy(type, "SSD/HDD"); break;
            case DRIVE_REMOVABLE: strcpy(type, "USB");    break;
            case DRIVE_CDROM:     strcpy(type, "CDROM");  break;
            case DRIVE_RAMDISK:   strcpy(type, "RAM");    break;
            default:              strcpy(type, "NET");    break;
        }
        if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE) {
            char vol[64] = "";
            GetVolumeInformationA(p, vol, sizeof(vol), NULL, NULL, NULL, NULL, 0);
            char tmp[300];
            DWORDLONG total = 0, freeb = 0;
            if (GetDiskFreeSpaceExA(p, NULL, (PULARGE_INTEGER)&total,
                                    (PULARGE_INTEGER)&freeb) && total > 0) {
                double gb_total = (double)total / (1024.0*1024*1024);
                double gb_free = (double)freeb / (1024.0*1024*1024);
                double used_pct = 100.0 * (1.0 - gb_free / gb_total);
                if (vol[0]) {
                    snprintf(tmp, sizeof(tmp), "%s (%s) %.1f/%.1f GB (%.0f%%)  ",
                             p, vol, gb_total - gb_free, gb_total, used_pct);
                } else {
                    snprintf(tmp, sizeof(tmp), "%s %.1f/%.1f GB (%.0f%%)  ",
                             p, gb_total - gb_free, gb_total, used_pct);
                }
                strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
                count++;
            }
        }
        p += strlen(p) + 1;
    }
    return buf;
}

static void handle_status(SOCKET s) {
    char resp[MAX_RESP];
    SYSTEM_INFO si;
    GetSystemInfo(&si);

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    char compName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD compLen = sizeof(compName);
    GetComputerNameA(compName, &compLen);

    char userName[256];
    DWORD userLen = sizeof(userName);
    GetUserNameA(userName, &userLen);

    OSVERSIONINFOEXA osvi;
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA((LPOSVERSIONINFOA)&osvi);

    char arch[16] = "?";
    switch(si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: strcpy(arch, "x64"); break;
        case PROCESSOR_ARCHITECTURE_INTEL: strcpy(arch, "x86"); break;
        case PROCESSOR_ARCHITECTURE_ARM64: strcpy(arch, "ARM64"); break;
    }

    DWORD mhz = 0;
    DWORD cbData = sizeof(mhz);
    HKEY hKey;
    LONG regRet = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey);
    if (regRet == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "~MHz", NULL, NULL, (LPBYTE)&mhz, &cbData);
        RegCloseKey(hKey);
    }

    double cpu_load = 0.0;
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        EnterCriticalSection(&g_cpu.lock);
        ULONGLONG nowIdle = ((ULONGLONG)idle.dwHighDateTime << 32) | idle.dwLowDateTime;
        ULONGLONG nowKernel = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        ULONGLONG nowUser = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime;
        if (g_cpu.initialized) {
            ULONGLONG idleDiff = nowIdle - g_cpu.prevIdle;
            ULONGLONG kernelDiff = nowKernel - g_cpu.prevKernel;
            ULONGLONG userDiff = nowUser - g_cpu.prevUser;
            ULONGLONG total = kernelDiff + userDiff;
            if (total > 0) {
                cpu_load = 100.0 - (100.0 * (double)idleDiff / total);
                g_cpu.lastCpu = cpu_load;
            } else {
                cpu_load = g_cpu.lastCpu;
            }
        } else {
            cpu_load = g_cpu.lastCpu;
        }
        g_cpu.prevIdle = nowIdle;
        g_cpu.prevKernel = nowKernel;
        g_cpu.prevUser = nowUser;
        g_cpu.initialized = 1;
        LeaveCriticalSection(&g_cpu.lock);
    }

    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "\r\n"
        "=== sysseer v2.0 ===\n"
        "host:     %s\n"
        "user:     %s\n"
        "time:     %s\n"
        "os:       Windows %d.%d build %d\n"
        "arch:     %s\n"
        "cpu:      %d cores, ~%d MHz\n"
        "cpu load: %.1f%%\n"
        "ram:      %s\n"
        "disks:    %s\n",
        compName, userName, get_time_str(),
        osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber,
        arch,
        si.dwNumberOfProcessors, mhz,
        cpu_load,
        mem_status(ms.ullTotalPhys, ms.ullAvailPhys),
        disk_info());

    if (n > 0) send_response(s, resp);
    else send_response(s, RESP_OK_500);
}

static void handle_ps(SOCKET s, int verbose) {
    char resp[MAX_RESP];
    int offset = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n");

    DWORD procs[2048];
    DWORD cb;
    if (!EnumProcesses(procs, sizeof(procs), &cb)) {
        send_response(s, RESP_OK_500);
        return;
    }
    DWORD count = cb / sizeof(DWORD);
    if (count > 2048) count = 2048;

    for (DWORD i = 0; i < count && offset < (int)sizeof(resp) - 200; i++) {
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                               FALSE, procs[i]);
        if (!h) continue;

        DWORD nameSize = MAX_PATH_LEN;
        char name[MAX_PATH_LEN];
        BOOL gotName = QueryFullProcessImageNameA(h, 0, name, &nameSize);
        CloseHandle(h);

        if (!gotName) continue;

        char* fn = strrchr(name, '\\');
        fn = fn ? fn + 1 : name;

        if (verbose) {
            HANDLE hMem = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, procs[i]);
            double mb = 0.0;
            if (hMem) {
                PROCESS_MEMORY_COUNTERS pmc;
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hMem, &pmc, sizeof(pmc))) {
                    mb = (double)pmc.WorkingSetSize / (1024.0 * 1024);
                }
                CloseHandle(hMem);
            }
            offset += snprintf(resp + offset, sizeof(resp) - offset,
                "%-7lu %-28s %7.1f MB\n", procs[i], fn, mb);
        } else {
            offset += snprintf(resp + offset, sizeof(resp) - offset,
                "%-7lu %s\n", procs[i], fn);
        }
    }

    if (offset > 0) send_response(s, resp);
    else send_response(s, RESP_OK_500);
}

static void handle_net(SOCKET s) {
    char resp[MAX_RESP];
    int offset = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n");

    ULONG bufLen = sizeof(IP_ADAPTER_INFO) * 16;
    PIP_ADAPTER_INFO info = (PIP_ADAPTER_INFO)malloc(bufLen);
    if (!info) {
        send_response(s, RESP_OK_500);
        return;
    }

    DWORD ret = GetAdaptersInfo(info, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(info);
        info = (PIP_ADAPTER_INFO)malloc(bufLen);
        if (info) ret = GetAdaptersInfo(info, &bufLen);
    }

    if (ret == NO_ERROR && info) {
        PIP_ADAPTER_INFO p = info;
        while (p && offset < (int)sizeof(resp) - 300) {
            char mac[20];
            snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
                p->Address[0], p->Address[1], p->Address[2],
                p->Address[3], p->Address[4], p->Address[5]);
            offset += snprintf(resp + offset, sizeof(resp) - offset,
                "%s\n  ip: %s  gw: %s  mac: %s\n\n",
                p->Description,
                p->IpAddressList.IpAddress.String,
                p->GatewayList.IpAddress.String,
                mac);
            p = p->Next;
        }
    }
    free(info);

    if (offset > 0) send_response(s, resp);
    else send_response(s, RESP_OK_500);
}

static void handle_req(SOCKET s) {
    char req[BUFSIZE];
    int n = recv(s, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        closesocket(s);
        return;
    }
    req[n] = 0;

    if (n >= (int)sizeof(req) - 1) {
        send_response(s, RESP_OK_413);
        closesocket(s);
        return;
    }

    /* parse method and path */
    char method[16] = {0}, path[512] = {0};
    if (sscanf(req, "%15s %511s", method, path) < 2) {
        send_response(s, RESP_OK_400);
        closesocket(s);
        return;
    }

    /* only GET is supported */
    if (_stricmp(method, "GET") != 0) {
        send_response(s, RESP_OK_404);
        closesocket(s);
        return;
    }

    /* strip query string for routing */
    char* qmark = strchr(path, '?');
    if (qmark) *qmark = 0;

    if (strcmp(path, "/") == 0 || strcmp(path, "/status") == 0) {
        handle_status(s);
    } else if (strcmp(path, "/ps") == 0) {
        handle_ps(s, 0);
    } else if (strcmp(path, "/ps/verbose") == 0) {
        handle_ps(s, 1);
    } else if (strcmp(path, "/net") == 0) {
        handle_net(s);
    } else if (strcmp(path, "/monitor") == 0) {
        handle_status(s);
    } else {
        send_response(s, RESP_OK_404);
    }

    closesocket(s);
}

int main(void) {
    WSADATA wsa;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (err != 0) {
        printf("[sysseer] WSAStartup failed: %d\n", err);
        return 1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        printf("[sysseer] socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[sysseer] bind 127.0.0.1:%d failed: %d\n", PORT, WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (listen(server, ACCEPT_BACKLOG) == SOCKET_ERROR) {
        printf("[sysseer] listen() failed: %d\n", WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 1;
    }

    InitializeCriticalSection(&g_cpu.lock);

    printf("[sysseer] listening on http://localhost:%d\n", PORT);
    printf("[sysseer] pid %d\n", GetCurrentProcessId());
    printf("[sysseer] endpoints: / /ps /ps/verbose /net /monitor\n");

    while (1) {
        struct sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);
        SOCKET client = accept(server, (struct sockaddr*)&clientAddr, &addrLen);
        if (client == INVALID_SOCKET) {
            DWORD e = WSAGetLastError();
            if (e == WSAEINTR) continue;
            printf("[sysseer] accept() failed: %lu\n", e);
            break;
        }

        /* set socket timeouts */
        DWORD to = SOCK_TIMEOUT;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof(to));

        handle_req(client);
    }

    DeleteCriticalSection(&g_cpu.lock);
    closesocket(server);
    WSACleanup();
    return 0;
}
