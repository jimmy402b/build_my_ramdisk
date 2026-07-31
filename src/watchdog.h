#pragma once
#include <string>
#include <functional>
#include <memory>

class Watchdog {
public:
    using Logger = std::function<void(const std::string&)>;
    Watchdog();
    ~Watchdog();
    bool Start(const std::string& workerPath, const std::string& workerArgs,
               const std::string& pipeName, int crashLimit = 5, int crashWindowSec = 60);
    void Stop();
    bool IsRunning() const;
    void SetLogger(Logger logger);
private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
