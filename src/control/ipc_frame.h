#pragma once

#include "src/common/result.h"
#include "src/product/uuid.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace myrd::ipc {

inline constexpr std::uint32_t kMagic = 0x4452594d; // "MYRD" on the wire
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::uint32_t kMaxPayloadBytes = 1024U * 1024U;
inline constexpr std::size_t kFrameHeaderBytes = 54;

enum class MessageType : std::uint16_t {
    request = 1,
    response = 2,
};

struct Frame {
    std::uint16_t protocol_major{kProtocolMajor};
    std::uint16_t protocol_minor{kProtocolMinor};
    MessageType message_type;
    std::uint32_t flags{};
    product::Uuid request_id;
    product::Uuid operation_id;
    std::uint32_t command_id{};
    std::vector<std::byte> payload;
};

Result<std::vector<std::byte>> encode_frame(const Frame& frame);
Result<Frame> decode_frame(std::span<const std::byte> bytes);

} // namespace myrd::ipc
