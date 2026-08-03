#include "test_framework.h"

#include "src/common/unique_handle.h"
#include "src/common/unique_view.h"
#include "src/m2/m2_provider.h"
#include "src/proxy/imdproxy_protocol.h"
#include "src/proxy/object_names.h"
#include "src/storage/zero_backing_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>

namespace {

using namespace std::chrono_literals;
using myrd::DataRange;
using myrd::DeviceFeatures;
using myrd::ErrorDomain;
using myrd::IBlockDevice;
using myrd::IM2StoreOwner;
using myrd::M2Provider;
using myrd::M2ProviderConfig;
using myrd::M2ProviderFactories;
using myrd::OperationContext;
using myrd::Result;

constexpr std::uint64_t kSize = 8ull * 1024ull * 1024ull;

M2ProviderConfig config(std::chrono::milliseconds timeout = 2s) {
    return {{kSize, 512,
             {2ull * 1024ull * 1024ull, 4ull * 1024ull * 1024ull,
              1ull * 1024ull * 1024ull, 64ull * 1024ull, 64, 64},
             myrd::InitialBlockState::CleanZero,
             std::make_shared<myrd::ZeroBackingStore>(), 0}, timeout};
}

bool absent(const myrd::proxy::ProxyObjectNames& names) {
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, names.server.c_str());
    if (handle) {
        CloseHandle(handle);
        return false;
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    handle = OpenFileMappingW(FILE_MAP_READ, FALSE, names.mapping.c_str());
    if (handle) {
        CloseHandle(handle);
        return false;
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    for (const auto* name : {names.request.c_str(), names.response.c_str(),
                             names.cancel.c_str()}) {
        handle = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (handle) {
            CloseHandle(handle);
            return false;
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    }
    return true;
}

struct Client {
    myrd::UniqueHandle mapping;
    myrd::UniqueView view;
    myrd::UniqueHandle request;
    myrd::UniqueHandle response;
    myrd::UniqueHandle cancel;

    static Client open(const myrd::proxy::ProxyObjectNames& names) {
        Client client;
        client.mapping = myrd::UniqueHandle(OpenFileMappingW(
            FILE_MAP_READ | FILE_MAP_WRITE, FALSE, names.mapping.c_str()));
        REQUIRE(client.mapping);
        client.view = myrd::UniqueView(MapViewOfFile(
            client.mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            myrd::proxy::kMappingSize));
        REQUIRE(client.view);
        client.request = myrd::UniqueHandle(OpenEventW(
            EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, names.request.c_str()));
        client.response = myrd::UniqueHandle(OpenEventW(
            SYNCHRONIZE, FALSE, names.response.c_str()));
        client.cancel = myrd::UniqueHandle(OpenEventW(
            EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, names.cancel.c_str()));
        REQUIRE(client.request && client.response && client.cancel);
        return client;
    }

    std::span<std::byte> bytes() noexcept {
        return {static_cast<std::byte*>(view.get()), myrd::proxy::kMappingSize};
    }
};

template <class Request>
void send(Client& client, const Request& request) {
    auto result = myrd::proxy::write_wire(
        client.bytes().first(myrd::proxy::kHeaderSize), 0, request);
    REQUIRE(result.ok());
    REQUIRE(SetEvent(client.request.get()) != FALSE);
    REQUIRE(WaitForSingleObject(client.response.get(), 5000) == WAIT_OBJECT_0);
}

void write_range(Client& client, std::uint64_t offset,
                 std::uint64_t length) {
    auto result = myrd::proxy::write_wire(
        client.bytes().subspan(myrd::proxy::kHeaderSize), 0,
        myrd::proxy::DataSetRangeWire{offset, length});
    REQUIRE(result.ok());
}

void verify_info_round_trip(Client& client) {
    send(client, myrd::proxy::kReqInfo);
    auto response = myrd::proxy::read_wire<myrd::proxy::InfoResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(response.ok());
    REQUIRE_EQ(response.value().file_size, kSize);
    REQUIRE_EQ(response.value().req_alignment, 512u);
}

struct BlockingDevice final : IBlockDevice {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::uint64_t calls = 0;

    std::uint64_t size_bytes() const noexcept override { return kSize; }
    std::uint32_t sector_size() const noexcept override { return 512; }
    DeviceFeatures features() const noexcept override { return {false, true, true}; }

    Result<void> read(const OperationContext& context, std::uint64_t,
                      std::span<std::byte> output) override {
        std::unique_lock lock(mutex);
        ++calls;
        condition.notify_all();
        std::stop_callback callback(context.stop_token,
            [this] { condition.notify_all(); });
        condition.wait_until(lock, context.deadline, [&] {
            return context.stop_token.stop_requested();
        });
        auto live = myrd::check_operation_context(context);
        if (!live) return Result<void>::failure(live.error());
        std::fill(output.begin(), output.end(), std::byte{0});
        return Result<void>::success();
    }

    Result<void> write(const OperationContext&, std::uint64_t,
                       std::span<const std::byte>) override {
        return Result<void>::failure({ErrorDomain::storage, 1,
            L"BlockingDevice::write", L"not used"});
    }
    Result<void> unmap(const OperationContext&,
                       std::span<const DataRange>) override {
        return Result<void>::success();
    }
    Result<void> zero(const OperationContext&,
                      std::span<const DataRange>) override {
        return Result<void>::success();
    }

    bool wait_for_calls(std::uint64_t count,
                        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [&] { return calls >= count; });
    }
};

struct BlockingOwner final : IM2StoreOwner {
    explicit BlockingOwner(std::shared_ptr<BlockingDevice> device)
        : device_(std::move(device)) {}
    IBlockDevice& device() noexcept override { return *device_; }
    Result<void> close() noexcept override {
        device_->condition.notify_all();
        return Result<void>::success();
    }
    myrd::DynamicBlockStoreStats stats() const override { return {}; }
    std::shared_ptr<BlockingDevice> device_;
};

M2ProviderFactories blocking_factories(
    const std::shared_ptr<BlockingDevice>& device) {
    M2ProviderFactories factories;
    factories.store = [device](myrd::DynamicBlockStoreCreateArgs) {
        return Result<std::unique_ptr<IM2StoreOwner>>::success(
            std::make_unique<BlockingOwner>(device));
    };
    return factories;
}

} // namespace

TEST_CASE(m2_shm_dynamic_store_cross_block_write_read_zero_and_unmap) {
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(policy, config());
    REQUIRE(provider.ok());
    const auto names = provider.value()->names();
    REQUIRE(provider.value()->start().ok());
    Client client = Client::open(names);

    constexpr std::uint64_t offset = 64ull * 1024ull - 512;
    constexpr std::uint64_t length = 1024;
    for (std::uint64_t index = 0; index < length; ++index) {
        client.bytes()[myrd::proxy::kHeaderSize + index] =
            static_cast<std::byte>(static_cast<unsigned char>(index & 0xff));
    }
    send(client, myrd::proxy::WriteRequest{
        myrd::proxy::kReqWrite, offset, length});
    auto write_response = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(write_response.ok());
    REQUIRE_EQ(write_response.value().errorno, 0u);
    REQUIRE_EQ(write_response.value().length, length);

    std::fill_n(client.bytes().data() + myrd::proxy::kHeaderSize,
                static_cast<std::size_t>(length), std::byte{0});
    send(client, myrd::proxy::ReadRequest{
        myrd::proxy::kReqRead, offset, length});
    auto read_response = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(read_response.ok());
    REQUIRE_EQ(read_response.value().errorno, 0u);
    REQUIRE_EQ(read_response.value().length, length);
    for (std::uint64_t index = 0; index < length; ++index) {
        REQUIRE(client.bytes()[myrd::proxy::kHeaderSize + index] ==
            static_cast<std::byte>(static_cast<unsigned char>(index & 0xff)));
    }

    write_range(client, offset, length);
    send(client, myrd::proxy::ZeroRequest{
        myrd::proxy::kReqZero, sizeof(myrd::proxy::DataSetRangeWire)});
    auto zero_response = myrd::proxy::read_wire<myrd::proxy::ZeroResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(zero_response.ok());
    REQUIRE_EQ(zero_response.value().errorno, 0u);
    send(client, myrd::proxy::ReadRequest{
        myrd::proxy::kReqRead, offset, length});
    for (std::uint64_t index = 0; index < length; ++index) {
        REQUIRE(client.bytes()[myrd::proxy::kHeaderSize + index] == std::byte{0});
    }

    std::fill_n(client.bytes().data() + myrd::proxy::kHeaderSize,
                static_cast<std::size_t>(length), std::byte{0x5a});
    send(client, myrd::proxy::WriteRequest{
        myrd::proxy::kReqWrite, offset, length});
    write_response = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(write_response.ok());
    REQUIRE_EQ(write_response.value().errorno, 0u);
    REQUIRE_EQ(write_response.value().length, length);
    send(client, myrd::proxy::ReadRequest{
        myrd::proxy::kReqRead, offset, length});
    read_response = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(read_response.ok());
    REQUIRE_EQ(read_response.value().errorno, 0u);
    REQUIRE_EQ(read_response.value().length, length);
    for (std::uint64_t index = 0; index < length; ++index) {
        REQUIRE(client.bytes()[myrd::proxy::kHeaderSize + index] ==
                std::byte{0x5a});
    }
    write_range(client, offset, length);
    send(client, myrd::proxy::UnmapRequest{
        myrd::proxy::kReqUnmap, sizeof(myrd::proxy::DataSetRangeWire)});
    auto unmap_response = myrd::proxy::read_wire<myrd::proxy::UnmapResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(unmap_response.ok());
    REQUIRE_EQ(unmap_response.value().errorno, 0u);
    send(client, myrd::proxy::ReadRequest{
        myrd::proxy::kReqRead, offset, length});
    for (std::uint64_t index = 0; index < length; ++index) {
        REQUIRE(client.bytes()[myrd::proxy::kHeaderSize + index] == std::byte{0});
    }

    REQUIRE(provider.value()->stop().ok());
    client = {};
    REQUIRE(absent(names));
}

TEST_CASE(m2_shm_malformed_request_is_rejected_before_storage_mutation) {
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = M2Provider::create_for_test(policy, config());
    REQUIRE(provider.ok());
    const auto names = provider.value()->names();
    REQUIRE(provider.value()->start().ok());
    Client client = Client::open(names);
    const auto before = provider.value()->storage_stats();

    std::fill_n(client.bytes().data() + myrd::proxy::kHeaderSize,
                512, std::byte{0x7f});
    send(client, myrd::proxy::WriteRequest{
        myrd::proxy::kReqWrite, 1, 512});
    auto response = myrd::proxy::read_wire<myrd::proxy::WriteResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(response.ok());
    REQUIRE_EQ(response.value().errorno, ERROR_INVALID_PARAMETER);
    REQUIRE_EQ(response.value().length, 0u);

    send(client, myrd::proxy::ReadRequest{
        myrd::proxy::kReqRead, 0, 1024});
    auto readback = myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
        std::span<const std::byte>(client.bytes().data(),
                                  myrd::proxy::kHeaderSize), 0);
    REQUIRE(readback.ok());
    REQUIRE_EQ(readback.value().errorno, 0u);
    REQUIRE_EQ(readback.value().length, 1024u);
    for (std::size_t index = 0; index < 1024; ++index) {
        REQUIRE(client.bytes()[myrd::proxy::kHeaderSize + index] ==
                std::byte{0});
    }

    const auto after = provider.value()->storage_stats();
    REQUIRE_EQ(after.committed_bytes, before.committed_bytes);
    REQUIRE(provider.value()->stop().ok());
}

TEST_CASE(m2_shm_cancel_and_deadline_are_visible_at_protocol_boundary) {
    myrd::proxy::LocalNamespacePolicy policy;
    auto device = std::make_shared<BlockingDevice>();
    auto provider = M2Provider::create_for_test(
        policy, config(200ms), blocking_factories(device));
    REQUIRE(provider.ok());
    const auto names = provider.value()->names();
    REQUIRE(provider.value()->start().ok());
    Client client = Client::open(names);

    auto send_read = [&] {
        auto result = myrd::proxy::write_wire(
            client.bytes().first(myrd::proxy::kHeaderSize), 0,
            myrd::proxy::ReadRequest{myrd::proxy::kReqRead, 0, 512});
        REQUIRE(result.ok());
        REQUIRE(SetEvent(client.request.get()) != FALSE);
        REQUIRE(WaitForSingleObject(client.response.get(), 5000) == WAIT_OBJECT_0);
        return myrd::proxy::read_wire<myrd::proxy::ReadResponse>(
            std::span<const std::byte>(client.bytes().data(),
                                      myrd::proxy::kHeaderSize), 0);
    };

    auto deadline = send_read();
    REQUIRE(deadline.ok());
    REQUIRE_EQ(deadline.value().errorno, ERROR_TIMEOUT);
    REQUIRE_EQ(deadline.value().length, 0u);

    auto future = std::async(std::launch::async, send_read);
    REQUIRE(device->wait_for_calls(2, 2s));
    REQUIRE(SetEvent(client.cancel.get()) != FALSE);
    REQUIRE(future.wait_for(2s) == std::future_status::ready);
    auto cancelled = future.get();
    REQUIRE(cancelled.ok());
    REQUIRE_EQ(cancelled.value().errorno, ERROR_OPERATION_ABORTED);
    REQUIRE_EQ(cancelled.value().length, 0u);
    REQUIRE(provider.value()->stop().ok());
}

TEST_CASE(m2_shm_disconnect_and_restart_releases_provider_objects) {
    myrd::proxy::LocalNamespacePolicy policy;
    auto first = M2Provider::create_for_test(policy, config());
    REQUIRE(first.ok());
    const auto first_names = first.value()->names();
    REQUIRE(first.value()->start().ok());
    Client first_client = Client::open(first_names);
    verify_info_round_trip(first_client);
    first_client = {};
    REQUIRE(first.value()->ready());
    Client reconnected_client = Client::open(first_names);
    verify_info_round_trip(reconnected_client);
    reconnected_client = {};
    REQUIRE(first.value()->stop().ok());
    REQUIRE(absent(first_names));

    auto second = M2Provider::create_for_test(policy, config());
    REQUIRE(second.ok());
    const auto second_names = second.value()->names();
    REQUIRE(second_names.mapping != first_names.mapping);
    REQUIRE(second.value()->start().ok());
    Client second_client = Client::open(second_names);
    verify_info_round_trip(second_client);
    second_client = {};
    REQUIRE(second.value()->stop().ok());
    REQUIRE(absent(second_names));
}
