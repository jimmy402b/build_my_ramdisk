#pragma once

#include <functional>
#include <ctime>
#include <memory>

class SaveScheduler {
public:
    using SaveCallback = std::function<bool()>;

    SaveScheduler();
    ~SaveScheduler();

    SaveScheduler(const SaveScheduler&) = delete;
    SaveScheduler& operator=(const SaveScheduler&) = delete;

    bool Start(int intervalSeconds);
    void Stop();
    bool SaveNow();
    void RegisterShutdownHandler();

    time_t GetLastSaveTime() const;
    bool IsRunning() const;
    int  GetInterval() const;
    void SetSaveCallback(SaveCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
