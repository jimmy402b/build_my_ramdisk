#pragma once

#include "imdproxy_protocol.h"
#include "object_names.h"
#include "src/common/result.h"
#include "src/common/unique_handle.h"
#include "src/common/unique_view.h"

#include <cstdint>
#include <functional>
#include <span>

namespace myrd::proxy {

enum class ChannelCreateStage : std::uint8_t {
    owner_mutex, mapping, map_view, request_event,
    response_event, cancel_event
};

using ChannelCreateHook =
    std::function<Result<void>(ChannelCreateStage)>;

class SharedMemoryChannel final {
public:
    static Result<SharedMemoryChannel> create(const ObjectNames& names,
                                              ChannelCreateHook hook = {});
    SharedMemoryChannel(SharedMemoryChannel&&) noexcept;
    SharedMemoryChannel& operator=(SharedMemoryChannel&&) noexcept;
    SharedMemoryChannel(const SharedMemoryChannel&) = delete;
    SharedMemoryChannel& operator=(const SharedMemoryChannel&) = delete;
    ~SharedMemoryChannel() noexcept;

    std::span<std::byte> mapping() noexcept;
    HANDLE request_event() const noexcept;
    HANDLE response_event() const noexcept;
    HANDLE cancel_event() const noexcept;
    const ObjectNames& names() const noexcept;

private:
    SharedMemoryChannel() = default;
    ObjectNames names_;
    UniqueHandle mapping_handle_;
    UniqueView  mapping_view_;
    UniqueHandle request_event_;
    UniqueHandle response_event_;
    UniqueHandle cancel_event_;
    UniqueHandle server_mutex_;
};

} // namespace myrd::proxy
