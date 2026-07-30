#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

// --- Logging ---

void LogInfo(const std::string& msg) {
    fprintf(stderr, "[INFO] %s\n", msg.c_str());
    fflush(stderr);
}

void LogError(const std::string& msg) {
    fprintf(stderr, "[ERROR] %s\n", msg.c_str());
    fflush(stderr);
}

// --- Windows error ---

std::string GetLastWinErrorMessage() {
    DWORD err = GetLastError();
    if (err == 0) return "No error";
    LPSTR buf = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (LPSTR)&buf, 0, nullptr);
    std::string msg(buf, len);
    LocalFree(buf);
    return msg;
}

std::string GetErrorMessage(const std::string& context) {
    return context + ": " + GetLastWinErrorMessage();
}

// --- Formatting ---

std::string FormatSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && idx < 4) {
        size /= 1024.0;
        idx++;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %s", size, units[idx]);
    return buf;
}

std::string FormatTime(time_t t) {
    char buf[16];
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return "--:--:--";
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return buf;
}

std::string FormatTimeAgo(time_t t, time_t now) {
    if (t == 0) return "never";
    int64_t diff = static_cast<int64_t>(now - t);
    if (diff < 0) diff = 0;
    if (diff < 5)   return "just now";
    if (diff < 60)  return std::to_string(diff) + "s ago";
    if (diff < 3600) return std::to_string(diff / 60) + "min ago";
    return std::to_string(diff / 3600) + "h ago";
}

// --- String helpers ---

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}
