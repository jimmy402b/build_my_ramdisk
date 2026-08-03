#pragma once

#include "src/common/result.h"
#include "src/proxy/object_names.h"
#include "src/proxy/shm_proxy_session.h"
#include "src/storage/block_device.h"
#include "src/storage/dynamic_block_types.h"

#include <chrono>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace myrd {

enum class M2ProviderState : std::uint8_t {
    constructed,
    starting,
    ready,
    stopping,
    stopped,
    failed,
};

struct M2ProviderConfig {
    DynamicBlockStoreCreateArgs store;
    std::chrono::milliseconds request_timeout{30000};
};

// Owns one block device and the storage-specific quiesce operation. The proxy
// receives only device(); it never sees this management interface.
class IM2StoreOwner {
public:
    virtual ~IM2StoreOwner() = default;
    virtual IBlockDevice& device() noexcept = 0;
    virtual Result<void> close() noexcept = 0;
    virtual DynamicBlockStoreStats stats() const = 0;
};

// Narrow lifecycle seam around the accepted M1 shared-memory session.
class IM2ProxyTransport {
public:
    virtual ~IM2ProxyTransport() = default;
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual proxy::SessionSnapshot snapshot() const = 0;
    virtual proxy::ProxyObjectNames names() const = 0;
};

struct M2ProviderFactories {
    using StoreFactory = std::function<
        Result<std::unique_ptr<IM2StoreOwner>>(DynamicBlockStoreCreateArgs)>;
    using TransportFactory = std::function<
        Result<std::unique_ptr<IM2ProxyTransport>>(
            IBlockDevice&,
            const proxy::IProxyNamespacePolicy&,
            proxy::ShmProxySessionOptions)>;

    StoreFactory store;
    TransportFactory transport;
};

class M2Provider final {
public:
    static Result<std::unique_ptr<M2Provider>> create(M2ProviderConfig config);
    static Result<std::unique_ptr<M2Provider>> create_for_test(
        const proxy::IProxyNamespacePolicy& namespace_policy,
        M2ProviderConfig config,
        M2ProviderFactories factories = {});

    ~M2Provider() noexcept;

    Result<void> start();
    Result<void> stop();

    bool ready() const noexcept;
    M2ProviderState state() const noexcept;
    std::wstring native_object_path() const;
    proxy::ProxyObjectNames names() const;
    proxy::SessionSnapshot snapshot() const;
    DynamicBlockStoreStats storage_stats() const;

    M2Provider(const M2Provider&) = delete;
    M2Provider& operator=(const M2Provider&) = delete;
    M2Provider(M2Provider&&) = delete;
    M2Provider& operator=(M2Provider&&) = delete;

private:
    M2Provider() = default;

    static Result<std::unique_ptr<M2Provider>> create_impl(
        const proxy::IProxyNamespacePolicy& namespace_policy,
        M2ProviderConfig config,
        M2ProviderFactories factories);
    Result<void> release_resources();
    void release_resources_noexcept() noexcept;
    void set_state(M2ProviderState state) noexcept;

    mutable std::mutex operation_mutex_;
    std::atomic<M2ProviderState> state_{M2ProviderState::constructed};
    std::unique_ptr<IM2StoreOwner> store_;
    std::unique_ptr<IM2ProxyTransport> transport_;
    proxy::ProxyObjectNames names_;
    std::optional<proxy::SessionSnapshot> final_snapshot_;
    std::optional<DynamicBlockStoreStats> final_stats_;
};

} // namespace myrd
