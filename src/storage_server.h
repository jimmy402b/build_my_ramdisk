#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <memory>

class StorageServer {
public:
    using Logger = std::function<void(const std::string&)>;
    StorageServer();
    ~StorageServer();
    bool Start(const std::string& pipeName, const std::string& vhdPath, uint64_t diskSizeBytes);
    void Stop();
    void SetLogger(Logger logger);
    uint64_t SaveDirty();
    uint64_t GetDirtyCount() const;
    bool IsRunning() const;
private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
