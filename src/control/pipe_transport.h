#pragma once

#include "src/common/operation_context.h"
#include "src/control/authorization.h"
#include "src/control/ipc_frame.h"

#include <memory>
#include <cstdint>
#include <string_view>

namespace myrd::ipc {

inline constexpr std::wstring_view kPublicControlPipeName =
    L"\\\\.\\pipe\\MyRamDisk.Control.v1";
inline constexpr std::size_t kMaximumPublicPipeInstances = 8;
inline constexpr std::uint32_t kPublicPipeModeFlags = 0x00000008U;

class PipeTransport final {
public:
    static Result<std::unique_ptr<PipeTransport>> connect(
        const OperationContext& context,
        std::wstring_view pipe_name = kPublicControlPipeName);
    static Result<std::unique_ptr<PipeTransport>> adopt_connected_handle(
        void* native_handle);

    ~PipeTransport() noexcept;
    PipeTransport(const PipeTransport&) = delete;
    PipeTransport& operator=(const PipeTransport&) = delete;
    PipeTransport(PipeTransport&&) = delete;
    PipeTransport& operator=(PipeTransport&&) = delete;

    Result<void> write_frame(const OperationContext& context,
                             const Frame& frame);
    Result<Frame> read_frame(const OperationContext& context);
    Result<CallerIdentity> capture_caller_identity() const;
    void close() noexcept;

private:
    explicit PipeTransport(void* native_handle) noexcept;

    void* handle_{};
};

} // namespace myrd::ipc
