#include "m2_provider.h"

#include "src/proxy/request_processor.h"
#include "src/proxy/shared_memory_channel.h"
#include "src/storage/dynamic_block_store.h"

#include <utility>

namespace myrd {

namespace {

Error provider_error(const wchar_t* operation, const wchar_t* message) {
    return {
        ErrorDomain::internal,
        static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
        operation,
        message,
    };
}

class DynamicStoreOwner final : public IM2StoreOwner {
public:
    explicit DynamicStoreOwner(std::unique_ptr<DynamicBlockStore> store)
        : store_(std::move(store)) {}

    IBlockDevice& device() noexcept override { return *store_; }

    Result<void> close() noexcept override {
        try {
            store_->close();
            return Result<void>::success();
        } catch (...) {
            return Result<void>::failure(provider_error(
                L"DynamicStoreOwner::close", L"store close threw"));
        }
    }

    DynamicBlockStoreStats stats() const override { return store_->stats(); }

private:
    std::unique_ptr<DynamicBlockStore> store_;
};

class ShmSessionTransport final : public IM2ProxyTransport {
public:
    ShmSessionTransport(proxy::SharedMemoryChannel channel,
                        IBlockDevice& device,
                        proxy::ShmProxySessionOptions options)
        : processor_(device),
          session_(std::move(channel), processor_, options) {}

    Result<void> start() override { return session_.start(); }
    Result<void> stop() override { return session_.stop(); }
    proxy::SessionSnapshot snapshot() const override {
        return session_.snapshot();
    }
    proxy::ProxyObjectNames names() const override { return session_.names(); }

private:
    // Declaration order matters: session must be destroyed before processor.
    proxy::RequestProcessor processor_;
    proxy::ShmProxySession session_;
};

Result<std::unique_ptr<IM2StoreOwner>> create_dynamic_store(
    DynamicBlockStoreCreateArgs args) {
    auto store = DynamicBlockStore::create(std::move(args));
    if (!store) {
        return Result<std::unique_ptr<IM2StoreOwner>>::failure(store.error());
    }
    return Result<std::unique_ptr<IM2StoreOwner>>::success(
        std::make_unique<DynamicStoreOwner>(store.move_value()));
}

Result<std::unique_ptr<IM2ProxyTransport>> create_shm_transport(
    IBlockDevice& device,
    const proxy::IProxyNamespacePolicy& namespace_policy,
    proxy::ShmProxySessionOptions options) {
    auto session_id = proxy::generate_session_id();
    if (!session_id) {
        return Result<std::unique_ptr<IM2ProxyTransport>>::failure(
            session_id.error());
    }
    auto names = namespace_policy.build_names(session_id.value());
    if (!names) {
        return Result<std::unique_ptr<IM2ProxyTransport>>::failure(
            names.error());
    }
    auto channel = proxy::SharedMemoryChannel::create(names.value());
    if (!channel) {
        return Result<std::unique_ptr<IM2ProxyTransport>>::failure(
            channel.error());
    }
    return Result<std::unique_ptr<IM2ProxyTransport>>::success(
        std::make_unique<ShmSessionTransport>(
            channel.move_value(), device, options));
}

M2ProviderFactories normalized_factories(M2ProviderFactories factories) {
    if (!factories.store) {
        factories.store = create_dynamic_store;
    }
    if (!factories.transport) {
        factories.transport = create_shm_transport;
    }
    return factories;
}

} // namespace

Result<std::unique_ptr<M2Provider>> M2Provider::create(
    M2ProviderConfig config) {
    proxy::KernelVisibleNamespacePolicy policy;
    return create_impl(policy, std::move(config), {});
}

Result<std::unique_ptr<M2Provider>> M2Provider::create_for_test(
    const proxy::IProxyNamespacePolicy& namespace_policy,
    M2ProviderConfig config,
    M2ProviderFactories factories) {
    return create_impl(
        namespace_policy, std::move(config), std::move(factories));
}

Result<std::unique_ptr<M2Provider>> M2Provider::create_impl(
    const proxy::IProxyNamespacePolicy& namespace_policy,
    M2ProviderConfig config,
    M2ProviderFactories factories) {
    factories = normalized_factories(std::move(factories));

    auto store = factories.store(std::move(config.store));
    if (!store) {
        return Result<std::unique_ptr<M2Provider>>::failure(store.error());
    }

    auto transport = factories.transport(
        store.value()->device(), namespace_policy,
        proxy::ShmProxySessionOptions{config.request_timeout, 64});
    if (!transport) {
        const auto transport_error = transport.error();
        auto close_result = store.value()->close();
        store.value().reset();
        if (!close_result) {
            return Result<std::unique_ptr<M2Provider>>::failure(
                close_result.error());
        }
        return Result<std::unique_ptr<M2Provider>>::failure(transport_error);
    }

    auto provider = std::unique_ptr<M2Provider>(new M2Provider());
    provider->store_ = store.move_value();
    provider->transport_ = transport.move_value();
    provider->names_ = provider->transport_->names();
    return Result<std::unique_ptr<M2Provider>>::success(std::move(provider));
}

M2Provider::~M2Provider() noexcept {
    // Destruction is single-owner cleanup. Do not acquire operation_mutex_:
    // even a lock failure must not skip transport teardown or store quiesce.
    set_state(M2ProviderState::stopping);
    release_resources_noexcept();
    set_state(M2ProviderState::stopped);
}

Result<void> M2Provider::start() {
    std::scoped_lock operation_lock(operation_mutex_);
    if (state() != M2ProviderState::constructed || !transport_ || !store_) {
        return Result<void>::failure(provider_error(
            L"M2Provider::start", L"provider cannot be started"));
    }
    set_state(M2ProviderState::starting);

    try {
        auto started = transport_->start();
        if (!started) {
            const auto start_error = started.error();
            set_state(M2ProviderState::failed);
            release_resources_noexcept();
            return Result<void>::failure(start_error);
        }
    } catch (...) {
        set_state(M2ProviderState::failed);
        release_resources_noexcept();
        return Result<void>::failure(provider_error(
            L"IM2ProxyTransport::start", L"transport start threw"));
    }

    set_state(M2ProviderState::ready);
    return Result<void>::success();
}

Result<void> M2Provider::stop() {
    std::scoped_lock operation_lock(operation_mutex_);
    if (state() == M2ProviderState::stopped) {
        return Result<void>::success();
    }
    set_state(M2ProviderState::stopping);

    auto released = release_resources();
    set_state(released ? M2ProviderState::stopped : M2ProviderState::failed);
    return released;
}

Result<void> M2Provider::release_resources() {
    std::optional<Error> first_error;
    bool unreported_exception = false;

    const auto record_error = [&](const Error& error) noexcept {
        if (first_error) return;
        try {
            first_error = error;
        } catch (...) {
            unreported_exception = true;
        }
    };
    const auto record_exception = [&](const wchar_t* operation) noexcept {
        if (first_error) return;
        try {
            first_error = provider_error(
                operation, L"cleanup operation threw");
        } catch (...) {
            unreported_exception = true;
        }
    };

    if (transport_) {
        try {
            auto stopped = transport_->stop();
            if (!stopped) record_error(stopped.error());
        } catch (...) {
            record_exception(L"IM2ProxyTransport::stop");
        }
        try {
            final_snapshot_ = transport_->snapshot();
        } catch (...) {
            record_exception(L"IM2ProxyTransport::snapshot");
        }
        transport_.reset();
    }

    if (store_) {
        try {
            final_stats_ = store_->stats();
        } catch (...) {
            record_exception(L"IM2StoreOwner::stats");
        }
        auto closed = store_->close();
        if (!closed) record_error(closed.error());
        store_.reset();
    }

    if (first_error) {
        return Result<void>::failure(std::move(*first_error));
    }
    if (unreported_exception) {
        return Result<void>::failure(provider_error(
            L"M2Provider::release_resources",
            L"cleanup failed and its detailed error could not be retained"));
    }
    return Result<void>::success();
}

void M2Provider::release_resources_noexcept() noexcept {
    try {
        (void)release_resources();
    } catch (...) {
        // Every cleanup phase and resource reset occurs before error reporting,
        // so an allocation failure while constructing the Result cannot leave
        // the transport or store owned by this provider.
    }
}

void M2Provider::set_state(M2ProviderState state) noexcept {
    state_.store(state, std::memory_order_release);
}

bool M2Provider::ready() const noexcept {
    return state() == M2ProviderState::ready;
}

M2ProviderState M2Provider::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

std::wstring M2Provider::native_object_path() const {
    return names_.driver_path;
}

proxy::ProxyObjectNames M2Provider::names() const {
    return names_;
}

proxy::SessionSnapshot M2Provider::snapshot() const {
    std::scoped_lock operation_lock(operation_mutex_);
    if (transport_) return transport_->snapshot();
    if (final_snapshot_) return *final_snapshot_;
    return {};
}

DynamicBlockStoreStats M2Provider::storage_stats() const {
    std::scoped_lock operation_lock(operation_mutex_);
    if (store_) return store_->stats();
    if (final_stats_) return *final_stats_;
    return {};
}

} // namespace myrd
