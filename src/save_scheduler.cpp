#include "save_scheduler.h"
#include "utils.h"

#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

struct SaveScheduler::Impl {
    SaveCallback callback;
    std::thread timerThread;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    int intervalSeconds = 0;
    time_t lastSaveTime = 0;
    SaveScheduler* owner = nullptr;
};

static SaveScheduler* g_activeScheduler = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_SHUTDOWN_EVENT || ctrlType == CTRL_LOGOFF_EVENT) {
        if (g_activeScheduler) {
            LogInfo("Shutdown signal received (type=" + std::to_string(ctrlType) + "), saving...");
            g_activeScheduler->SaveNow();
        }
        if (ctrlType == CTRL_C_EVENT) return TRUE;
    }
    return FALSE;
}

SaveScheduler::SaveScheduler() : pImpl(new Impl()) { pImpl->owner = this; }

SaveScheduler::~SaveScheduler() { Stop(); }

bool SaveScheduler::Start(int intervalSeconds) {
    if (pImpl->running) { LogError("SaveScheduler already running"); return false; }
    pImpl->intervalSeconds = intervalSeconds;
    pImpl->stopRequested = false;

    if (intervalSeconds > 0) {
        pImpl->running = true;
        pImpl->timerThread = std::thread([this]() {
            LogInfo("Save timer started (interval=" + std::to_string(pImpl->intervalSeconds) + "s)");
            while (!pImpl->stopRequested) {
                std::unique_lock<std::mutex> lock(pImpl->mutex);
                pImpl->cv.wait_for(lock, std::chrono::seconds(pImpl->intervalSeconds));
                if (pImpl->stopRequested) break;
                lock.unlock();
                LogInfo("Auto-save triggered by timer");
                SaveNow();
            }
        });
    } else {
        pImpl->running = true;
        LogInfo("Save timer disabled (manual/shutdown save only)");
    }
    return true;
}

void SaveScheduler::Stop() {
    if (!pImpl->running) return;
    pImpl->stopRequested = true;
    pImpl->cv.notify_all();
    if (pImpl->timerThread.joinable()) pImpl->timerThread.join();
    pImpl->running = false;
    if (g_activeScheduler == this) { SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE); g_activeScheduler = nullptr; }
}

bool SaveScheduler::SaveNow() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->callback) { LogError("Save callback not set"); return false; }
    LogInfo("Save started...");
    bool ok = pImpl->callback();
    if (ok) { pImpl->lastSaveTime = time(nullptr); LogInfo("Save completed at " + FormatTime(pImpl->lastSaveTime)); }
    else { LogError("Save failed"); }
    return ok;
}

void SaveScheduler::RegisterShutdownHandler() {
    g_activeScheduler = this;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    LogInfo("Shutdown handler registered (Ctrl+C / close / shutdown)");
}

time_t SaveScheduler::GetLastSaveTime() const { return pImpl->lastSaveTime; }
bool   SaveScheduler::IsRunning() const { return pImpl->running; }
int    SaveScheduler::GetInterval() const { return pImpl->intervalSeconds; }
void   SaveScheduler::SetSaveCallback(SaveCallback callback) { pImpl->callback = std::move(callback); }
