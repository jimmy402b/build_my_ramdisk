#pragma once

#include "src/proxy/object_names.h"
#include "src/proxy/shm_proxy_session.h"
#include "src/storage/memory_block_device.h"

#include <chrono>
#include <memory>
#include <string>

namespace myrd {

struct M1ProviderConfig {
    std::uint64_t size_bytes = 64ull * 1024ull * 1024ull;
    std::uint32_t sector_size = 512;
    std::chrono::milliseconds request_timeout{30000};
};

class M1Provider final {
public:
    static Result<std::unique_ptr<M1Provider>> create(
        M1ProviderConfig config = {});
    static Result<std::unique_ptr<M1Provider>> create_for_test(
        const proxy::IProxyNamespacePolicy& namespace_policy,
        M1ProviderConfig config = {});
    ~M1Provider() noexcept;

    Result<void> start();
    Result<void> stop();
    std::wstring native_object_path() const;
    proxy::ProxyObjectNames names() const;
    proxy::SessionSnapshot snapshot() const;

    M1Provider(const M1Provider&) = delete;
    M1Provider& operator=(const M1Provider&) = delete;

    M1Provider() = default;

private:
    std::unique_ptr<MemoryBlockDevice> device_;
    std::unique_ptr<proxy::RequestProcessor> processor_;
    std::unique_ptr<proxy::SharedMemoryChannel> channel_;
    std::unique_ptr<proxy::ShmProxySession> session_;
    proxy::ProxyObjectNames names_;
};

} // namespace myrd
