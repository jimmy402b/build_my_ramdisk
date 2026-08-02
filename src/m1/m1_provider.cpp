#include "m1_provider.h"

namespace myrd {

Result<std::unique_ptr<M1Provider>> M1Provider::create(
    M1ProviderConfig config) {
    proxy::KernelVisibleNamespacePolicy policy;

    auto session_id = proxy::generate_session_id();
    if (!session_id) {
        return Result<std::unique_ptr<M1Provider>>::failure(session_id.error());
    }

    auto names = policy.build_names(session_id.value());
    if (!names) {
        return Result<std::unique_ptr<M1Provider>>::failure(names.error());
    }

    auto device = std::make_unique<MemoryBlockDevice>(
        config.size_bytes, config.sector_size, false);

    auto processor = std::make_unique<proxy::RequestProcessor>(*device);

    auto channel = proxy::SharedMemoryChannel::create(names.value());
    if (!channel) {
        return Result<std::unique_ptr<M1Provider>>::failure(channel.error());
    }
    proxy::ShmProxySessionOptions opts{config.request_timeout, 64};
    auto session = std::make_unique<proxy::ShmProxySession>(
        std::move(channel.value()), *processor, opts);

    auto provider = std::unique_ptr<M1Provider>(new M1Provider());
    provider->names_ = names.value();
    provider->device_ = std::move(device);
    provider->processor_ = std::move(processor);
    provider->session_ = std::move(session);

    return Result<std::unique_ptr<M1Provider>>::success(std::move(provider));
}

Result<std::unique_ptr<M1Provider>> M1Provider::create_for_test(
    const proxy::IProxyNamespacePolicy& namespace_policy,
    M1ProviderConfig config) {
    auto session_id = proxy::generate_session_id();
    if (!session_id) {
        return Result<std::unique_ptr<M1Provider>>::failure(session_id.error());
    }

    auto names = namespace_policy.build_names(session_id.value());
    if (!names) {
        return Result<std::unique_ptr<M1Provider>>::failure(names.error());
    }

    auto device = std::make_unique<MemoryBlockDevice>(
        config.size_bytes, config.sector_size, false);

    auto processor = std::make_unique<proxy::RequestProcessor>(*device);

    auto channel = proxy::SharedMemoryChannel::create(names.value());
    if (!channel) {
        return Result<std::unique_ptr<M1Provider>>::failure(channel.error());
    }
    proxy::ShmProxySessionOptions opts{config.request_timeout, 64};
    auto session = std::make_unique<proxy::ShmProxySession>(
        std::move(channel.value()), *processor, opts);

    auto provider = std::unique_ptr<M1Provider>(new M1Provider());
    provider->names_ = names.value();
    provider->device_ = std::move(device);
    provider->processor_ = std::move(processor);
    provider->session_ = std::move(session);

    return Result<std::unique_ptr<M1Provider>>::success(std::move(provider));
}

M1Provider::~M1Provider() noexcept {
    stop();
}

Result<void> M1Provider::start() {
    if (!session_) {
        return Result<void>::failure({ErrorDomain::internal, 0,
            L"M1Provider::start", L"provider not fully constructed"});
    }
    return session_->start();
}

Result<void> M1Provider::stop() {
    if (session_) {
        auto r = session_->stop();
        final_snapshot_ = session_->snapshot();
        session_.reset();
        return r;
    }
    return Result<void>::success();
}

std::wstring M1Provider::native_object_path() const {
    return names_.driver_path;
}

proxy::ProxyObjectNames M1Provider::names() const {
    return names_;
}

proxy::SessionSnapshot M1Provider::snapshot() const {
    if (session_) {
        return session_->snapshot();
    }
    if (final_snapshot_) return *final_snapshot_;
    return proxy::SessionSnapshot{};
}

} // namespace myrd
