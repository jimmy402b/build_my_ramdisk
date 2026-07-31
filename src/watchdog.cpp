#include "watchdog.h"
#include "utils.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

struct Watchdog::Impl {
    std::string workerPath, workerArgs, pipeName;
    int crashLimit = 5, crashWindowSec = 60;
    std::atomic<bool> running{false};
    HANDLE hWorker = nullptr;
    std::thread monitorThread;
    Logger logger;
    std::deque<time_t> crashTimes;
    std::mutex crashMutex;
};

Watchdog::Watchdog() : pImpl(new Impl()) {}
Watchdog::~Watchdog() { Stop(); }

bool Watchdog::Start(const std::string& workerPath, const std::string& workerArgs,
                      const std::string& pipeName, int crashLimit, int crashWindowSec) {
    pImpl->workerPath = workerPath; pImpl->workerArgs = workerArgs; pImpl->pipeName = pipeName;
    pImpl->crashLimit = crashLimit; pImpl->crashWindowSec = crashWindowSec;
    pImpl->running = true;

    auto launch = [this]() -> bool {
        if (pImpl->hWorker) { TerminateProcess(pImpl->hWorker, 0); CloseHandle(pImpl->hWorker); pImpl->hWorker = nullptr; }
        STARTUPINFOA si = { sizeof(STARTUPINFOA) };
        PROCESS_INFORMATION pi = {};
        std::vector<char> cmdBuf(pImpl->workerArgs.begin(), pImpl->workerArgs.end()); cmdBuf.push_back('\0');
        if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            if (pImpl->logger) pImpl->logger("Launch failed: " + GetLastWinErrorMessage()); return false;
        }
        pImpl->hWorker = pi.hProcess; CloseHandle(pi.hThread);
        if (pImpl->logger) pImpl->logger("Worker launched (PID " + std::to_string(pi.dwProcessId) + ")");
        return true;
    };

    if (!launch()) return false;

    pImpl->monitorThread = std::thread([this, launch]() {
        while (pImpl->running) {
            if (WaitForSingleObject(pImpl->hWorker, 2000) == WAIT_OBJECT_0) {
                DWORD ec = 0; GetExitCodeProcess(pImpl->hWorker, &ec);
                if (pImpl->logger) pImpl->logger("Worker exited (code=" + std::to_string(ec) + ")");

                time_t now = time(nullptr);
                { std::lock_guard<std::mutex> lk(pImpl->crashMutex);
                  pImpl->crashTimes.push_back(now);
                  while (!pImpl->crashTimes.empty() && now - pImpl->crashTimes.front() > pImpl->crashWindowSec)
                      pImpl->crashTimes.pop_front();
                  if ((int)pImpl->crashTimes.size() > pImpl->crashLimit) {
                      if (pImpl->logger) pImpl->logger("CRITICAL: " + std::to_string(pImpl->crashLimit) + " crashes in " + std::to_string(pImpl->crashWindowSec) + "s. Giving up.");
                      pImpl->running = false; break;
                  }
                }
                if (pImpl->logger) pImpl->logger("Restarting worker...");
                Sleep(1000);
                if (!launch()) { Sleep(3000); launch(); }
            }
        }
    });
    return true;
}

void Watchdog::Stop() {
    pImpl->running = false;
    if (pImpl->monitorThread.joinable()) pImpl->monitorThread.join();
    if (pImpl->hWorker) { TerminateProcess(pImpl->hWorker, 0); CloseHandle(pImpl->hWorker); pImpl->hWorker = nullptr; }
}
bool Watchdog::IsRunning() const { return pImpl->running; }
void Watchdog::SetLogger(Watchdog::Logger logger) { pImpl->logger = logger; }
