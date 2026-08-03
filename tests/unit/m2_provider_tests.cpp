#include "test_framework.h"

#include "src/m2/m2_provider.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using myrd::DataRange;
using myrd::DeviceFeatures;
using myrd::DynamicBlockStoreCreateArgs;
using myrd::DynamicBlockStoreStats;
using myrd::Error;
using myrd::ErrorDomain;
using myrd::IBlockDevice;
using myrd::OperationContext;
using myrd::Result;
using myrd::M2Provider;
using myrd::M2ProviderConfig;
using myrd::M2ProviderFactories;
using myrd::IM2ProxyTransport;
using myrd::IM2StoreOwner;

struct Probe {
    std::vector<std::string> events;
    M2Provider* provider = nullptr;
    bool stop_saw_not_ready = false;
    bool store_closed = false;
};

Error injected_error(const wchar_t* operation) {
    return {ErrorDomain::internal, 0xE017u, operation, L"injected failure"};
}

DynamicBlockStoreCreateArgs valid_store_args() {
    return {
        8ull * 1024ull * 1024ull,
        512,
        {2ull * 1024ull * 1024ull, 4ull * 1024ull * 1024ull,
         1ull * 1024ull * 1024ull, 64ull * 1024ull, 64, 64},
        myrd::InitialBlockState::CleanZero,
        nullptr,
        0,
    };
}

struct FakeDevice final : IBlockDevice {
    std::vector<std::byte> bytes = std::vector<std::byte>(8ull * 1024ull * 1024ull);

    std::uint64_t size_bytes() const noexcept override { return bytes.size(); }
    std::uint32_t sector_size() const noexcept override { return 512; }
    DeviceFeatures features() const noexcept override { return {false, true, true}; }

    Result<void> read(const OperationContext&, std::uint64_t offset,
                      std::span<std::byte> output) override {
        if (offset > bytes.size() || output.size() > bytes.size() - offset) {
            return Result<void>::failure({ErrorDomain::storage, 1,
                L"FakeDevice::read", L"out of range"});
        }
        std::memcpy(output.data(), bytes.data() + offset, output.size());
        return Result<void>::success();
    }

    Result<void> write(const OperationContext&, std::uint64_t offset,
                       std::span<const std::byte> input) override {
        if (offset > bytes.size() || input.size() > bytes.size() - offset) {
            return Result<void>::failure({ErrorDomain::storage, 1,
                L"FakeDevice::write", L"out of range"});
        }
        std::memcpy(bytes.data() + offset, input.data(), input.size());
        return Result<void>::success();
    }

    Result<void> unmap(const OperationContext&, std::span<const DataRange> ranges) override {
        for (const auto& range : ranges) {
            std::fill_n(bytes.begin() + static_cast<std::size_t>(range.offset),
                        static_cast<std::size_t>(range.length), std::byte{0});
        }
        return Result<void>::success();
    }

    Result<void> zero(const OperationContext& context,
                      std::span<const DataRange> ranges) override {
        return unmap(context, ranges);
    }
};

struct FakeStoreOwner final : IM2StoreOwner {
    explicit FakeStoreOwner(std::shared_ptr<Probe> probe)
        : probe_(std::move(probe)) {
        probe_->events.emplace_back("store_create");
    }
    ~FakeStoreOwner() override { probe_->events.emplace_back("store_destroy"); }

    IBlockDevice& device() noexcept override { return device_; }
    Result<void> close() noexcept override {
        if (!closed_) {
            closed_ = true;
            probe_->events.emplace_back("store_close");
            probe_->store_closed = true;
        }
        return Result<void>::success();
    }
    DynamicBlockStoreStats stats() const override { return {}; }

    bool closed_ = false;
    FakeDevice device_;
    std::shared_ptr<Probe> probe_;
};

struct FakeTransport final : IM2ProxyTransport {
    FakeTransport(std::shared_ptr<Probe> probe, bool fail_start,
                  bool throw_stop, bool throw_snapshot, bool throw_start)
        : probe_(std::move(probe)), fail_start_(fail_start),
          throw_stop_(throw_stop), throw_snapshot_(throw_snapshot),
          throw_start_(throw_start) {
        probe_->events.emplace_back("transport_create");
    }
    ~FakeTransport() override { probe_->events.emplace_back("transport_destroy"); }

    Result<void> start() override {
        probe_->events.emplace_back("transport_start");
        if (throw_start_) throw std::runtime_error("injected start exception");
        if (fail_start_) return Result<void>::failure(injected_error(L"start"));
        return Result<void>::success();
    }
    Result<void> stop() override {
        probe_->events.emplace_back("transport_stop");
        probe_->stop_saw_not_ready = probe_->provider != nullptr &&
                                      !probe_->provider->ready();
        if (throw_stop_) throw std::runtime_error("injected stop exception");
        return Result<void>::success();
    }
    myrd::proxy::SessionSnapshot snapshot() const override {
        probe_->events.emplace_back("transport_snapshot");
        if (throw_snapshot_) {
            throw std::runtime_error("injected snapshot exception");
        }
        return {};
    }
    myrd::proxy::ProxyObjectNames names() const override { return {}; }

    std::shared_ptr<Probe> probe_;
    bool fail_start_ = false;
    bool throw_stop_ = false;
    bool throw_snapshot_ = false;
    bool throw_start_ = false;
};

M2ProviderFactories make_factories(const std::shared_ptr<Probe>& probe,
                                   bool fail_store = false,
                                   bool fail_transport_create = false,
                                   bool fail_transport_start = false,
                                   bool throw_transport_stop = false,
                                   bool throw_transport_snapshot = false,
                                   bool throw_transport_start = false) {
    M2ProviderFactories factories;
    factories.store = [probe, fail_store](DynamicBlockStoreCreateArgs) {
        if (fail_store) {
            return Result<std::unique_ptr<IM2StoreOwner>>::failure(
                injected_error(L"store_create"));
        }
        return Result<std::unique_ptr<IM2StoreOwner>>::success(
            std::make_unique<FakeStoreOwner>(probe));
    };
    factories.transport = [probe, fail_transport_create, fail_transport_start,
                            throw_transport_stop, throw_transport_snapshot,
                            throw_transport_start](
        IBlockDevice&, const myrd::proxy::IProxyNamespacePolicy&,
        myrd::proxy::ShmProxySessionOptions) {
        probe->events.emplace_back("transport_create");
        if (fail_transport_create) {
            return Result<std::unique_ptr<IM2ProxyTransport>>::failure(
                injected_error(L"transport_create"));
        }
        probe->events.pop_back();
        return Result<std::unique_ptr<IM2ProxyTransport>>::success(
            std::make_unique<FakeTransport>(
                probe, fail_transport_start, throw_transport_stop,
                throw_transport_snapshot, throw_transport_start));
    };
    return factories;
}

M2ProviderConfig valid_config() {
    return {valid_store_args(), std::chrono::seconds(2)};
}

} // namespace

TEST_CASE(m2_provider_store_failure_does_not_create_transport) {
    auto probe = std::make_shared<Probe>();
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(
        policy, valid_config(), make_factories(probe, true));
    REQUIRE(provider.has_error());
    REQUIRE(probe->events.empty());
}

TEST_CASE(m2_provider_transport_creation_failure_closes_store) {
    auto probe = std::make_shared<Probe>();
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(
        policy, valid_config(), make_factories(probe, false, true));
    REQUIRE(provider.has_error());
    REQUIRE(probe->store_closed);
    REQUIRE((probe->events == std::vector<std::string>{
        "store_create", "transport_create", "store_close", "store_destroy"}));
}

TEST_CASE(m2_provider_ready_is_published_only_after_transport_start) {
    auto probe = std::make_shared<Probe>();
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(
        policy, valid_config(), make_factories(probe));
    REQUIRE(provider.ok());
    probe->provider = provider.value().get();
    REQUIRE(!provider.value()->ready());
    REQUIRE(provider.value()->start().ok());
    REQUIRE(provider.value()->ready());
    REQUIRE(provider.value()->start().has_error());
    REQUIRE(provider.value()->stop().ok());
    REQUIRE(!provider.value()->ready());
    REQUIRE(probe->stop_saw_not_ready);
    REQUIRE((probe->events == std::vector<std::string>{
        "store_create", "transport_create", "transport_start",
        "transport_stop", "transport_snapshot", "transport_destroy",
        "store_close", "store_destroy"}));
}

TEST_CASE(m2_provider_transport_start_failure_rolls_back_everything) {
    auto probe = std::make_shared<Probe>();
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(
        policy, valid_config(), make_factories(probe, false, false, true));
    REQUIRE(provider.ok());
    probe->provider = provider.value().get();
    REQUIRE(provider.value()->start().has_error());
    REQUIRE(!provider.value()->ready());
    REQUIRE((probe->events == std::vector<std::string>{
        "store_create", "transport_create", "transport_start",
        "transport_stop", "transport_snapshot", "transport_destroy",
        "store_close", "store_destroy"}));
}

TEST_CASE(m2_provider_transport_start_exception_rolls_back_everything) {
    auto probe = std::make_shared<Probe>();
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(
        policy, valid_config(),
        make_factories(probe, false, false, false, false, false, true));
    REQUIRE(provider.ok());
    probe->provider = provider.value().get();
    REQUIRE(provider.value()->start().has_error());
    REQUIRE(!provider.value()->ready());
    REQUIRE(probe->store_closed);
    REQUIRE((probe->events == std::vector<std::string>{
        "store_create", "transport_create", "transport_start",
        "transport_stop", "transport_snapshot", "transport_destroy",
        "store_close", "store_destroy"}));
}

TEST_CASE(m2_provider_stop_before_start_and_repeated_stop_are_idempotent) {
    auto probe = std::make_shared<Probe>();
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(
        policy, valid_config(), make_factories(probe));
    REQUIRE(provider.ok());
    probe->provider = provider.value().get();
    REQUIRE(provider.value()->stop().ok());
    REQUIRE(provider.value()->stop().ok());
    REQUIRE((probe->events == std::vector<std::string>{
        "store_create", "transport_create", "transport_stop",
        "transport_snapshot", "transport_destroy", "store_close",
        "store_destroy"}));
}

TEST_CASE(m2_provider_cleanup_contains_transport_exceptions_and_closes_store) {
    auto probe = std::make_shared<Probe>();
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(
        policy, valid_config(),
        make_factories(probe, false, false, false, true, true));
    REQUIRE(provider.ok());
    probe->provider = provider.value().get();
    REQUIRE(provider.value()->start().ok());
    REQUIRE(provider.value()->stop().has_error());
    REQUIRE(probe->store_closed);
    REQUIRE((probe->events == std::vector<std::string>{
        "store_create", "transport_create", "transport_start",
        "transport_stop", "transport_snapshot", "transport_destroy",
        "store_close", "store_destroy"}));
}
