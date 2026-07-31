#include "daemon.h"
#include "utils.h"
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <mutex>
#include <vector>
#include <io.h>

static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;
static const char* UNMOUNT_EVENT_NAME = "Global\\RamDiskCli_Unmount";
static const char* DAEMON_MUTEX_NAME = "Global\\RamDiskCli_Daemon_Mutex";
static HANDLE g_hDaemonMutex = nullptr;

bool InitLogFile(const std::string& path) {
    g_logFile = fopen(path.c_str(), "a");
    if (!g_logFile) { LogError("Cannot open log file: " + path); return false; }
    time_t now = time(nullptr);
    char tb[32]; struct tm* tm = localtime(&now);
    strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(g_logFile, "[%s] === RamDisk CLI daemon started ===\n", tb);
    fflush(g_logFile);
    // Also flush OS buffers
    FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_logFile)));
    return true;
}

void WriteLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    time_t now = time(nullptr);
    char tb[32]; struct tm* tm = localtime(&now);
    strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tm);
    if (g_logFile) {
        fprintf(g_logFile, "[%s] %s\n", tb, msg.c_str());
        fflush(g_logFile);
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_logFile)));
    }
    LogInfo(msg);
}

void CloseLogFile() {
    if (g_logFile) {
        time_t now = time(nullptr);
        char tb[32]; struct tm* tm = localtime(&now);
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tm);
        fprintf(g_logFile, "[%s] === Daemon stopped ===\n", tb);
        fclose(g_logFile); g_logFile = nullptr;
    }
}

bool Daemonize(int argc, char* argv[]) {
    for (int i = 0; i < argc; i++) if (strcmp(argv[i], "--internal-daemon") == 0) return true;

    // Prevent multiple daemon instances
    g_hDaemonMutex = CreateMutexA(nullptr, FALSE, DAEMON_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogError("Another daemon is already running");
        CloseHandle(g_hDaemonMutex); g_hDaemonMutex = nullptr;
        return false;
    }

    // Get full path of current executable
    char myPath[MAX_PATH];
    GetModuleFileNameA(nullptr, myPath, sizeof(myPath));

    std::string cmdLine = "\"" + std::string(myPath) + "\"";
    for (int i = 1; i < argc; i++) { cmdLine += " " + std::string(argv[i]); }
    cmdLine += " --internal-daemon";
    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end()); cmdBuf.push_back('\0');
    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        LogError("Daemonize failed (" + cmdLine + "): " + GetLastWinErrorMessage()); return false;
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return false;
}

void* CreateUnmountEvent() {
    HANDLE h = CreateEventA(nullptr, TRUE, FALSE, UNMOUNT_EVENT_NAME);
    if (!h) LogError("CreateEvent failed: " + GetLastWinErrorMessage());
    return h;
}

bool IsUnmountSignaled(void* eventHandle) {
    if (!eventHandle) return false;
    return WaitForSingleObject((HANDLE)eventHandle, 0) == WAIT_OBJECT_0;
}

void SignalUnmount(void* eventHandle) {
    if (eventHandle) SetEvent((HANDLE)eventHandle);
    HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, UNMOUNT_EVENT_NAME);
    if (h) { SetEvent(h); CloseHandle(h); }
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;

    if (enable) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, sizeof(path));
        std::string cmd = "\"" + std::string(path) + "\" --auto-start";
        RegSetValueExA(hKey, "RamDiskCLI", 0, REG_SZ,
                       (BYTE*)cmd.c_str(), (DWORD)cmd.size() + 1);
    } else {
        RegDeleteValueA(hKey, "RamDiskCLI");
    }
    RegCloseKey(hKey);
}
