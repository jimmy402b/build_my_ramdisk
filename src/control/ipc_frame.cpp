#include "src/control/ipc_frame.h"

#include <array>
#include <limits>
#include <string>
#include <utility>

namespace myrd::ipc {

namespace {

constexpr std::uint32_t kInvalidFrameCode = 0x2001;

template <class T>
Result<T> frame_error(const wchar_t* operation, const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol, kInvalidFrameCode, operation, message});
}

void write_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void write_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

bool read_u16(std::span<const std::byte> bytes, std::size_t& offset,
              std::uint16_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(
        static_cast<unsigned>(bytes[offset])
        | (static_cast<unsigned>(bytes[offset + 1]) << 8U));
    offset += 2;
    return true;
}

bool read_u32(std::span<const std::byte> bytes, std::size_t& offset,
              std::uint32_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(
        static_cast<unsigned>(bytes[offset])
        | (static_cast<unsigned>(bytes[offset + 1]) << 8U)
        | (static_cast<unsigned>(bytes[offset + 2]) << 16U)
        | (static_cast<unsigned>(bytes[offset + 3]) << 24U));
    offset += 4;
    return true;
}

std::string uuid_text(std::span<const std::byte, 16> bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        const auto value = static_cast<unsigned char>(bytes[index]);
        result.push_back(hex[value >> 4U]);
        result.push_back(hex[value & 0x0fU]);
    }
    return result;
}

Result<product::Uuid> read_uuid(std::span<const std::byte> bytes,
                                std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < 16) {
        return frame_error<product::Uuid>(
            L"decode_frame", L"Frame UUID is truncated");
    }
    std::array<std::byte, 16> raw{};
    for (std::size_t index = 0; index < raw.size(); ++index) {
        raw[index] = bytes[offset + index];
    }
    offset += raw.size();
    auto parsed = product::Uuid::parse(uuid_text(raw));
    if (!parsed.ok()) {
        return frame_error<product::Uuid>(
            L"decode_frame", L"Frame UUID is invalid");
    }
    return parsed;
}

} // namespace

Result<std::vector<std::byte>> encode_frame(const Frame& frame) {
    if (frame.payload.size() > kMaxPayloadBytes
        || frame.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return frame_error<std::vector<std::byte>>(
            L"encode_frame", L"Frame payload exceeds the protocol limit");
    }
    if (frame.message_type != MessageType::request
        && frame.message_type != MessageType::response) {
        return frame_error<std::vector<std::byte>>(
            L"encode_frame", L"Frame message type is unknown");
    }
    std::vector<std::byte> encoded;
    encoded.reserve(kFrameHeaderBytes + frame.payload.size());
    write_u32(encoded, kMagic);
    write_u16(encoded, frame.protocol_major);
    write_u16(encoded, frame.protocol_minor);
    write_u16(encoded, static_cast<std::uint16_t>(frame.message_type));
    write_u32(encoded, frame.flags);
    write_u32(encoded, static_cast<std::uint32_t>(frame.payload.size()));
    for (const auto byte : frame.request_id.bytes()) encoded.push_back(byte);
    for (const auto byte : frame.operation_id.bytes()) encoded.push_back(byte);
    write_u32(encoded, frame.command_id);
    encoded.insert(encoded.end(), frame.payload.begin(), frame.payload.end());
    return Result<std::vector<std::byte>>::success(std::move(encoded));
}

Result<Frame> decode_frame(std::span<const std::byte> bytes) {
    if (bytes.size() < kFrameHeaderBytes) {
        return frame_error<Frame>(L"decode_frame", L"Frame header is truncated");
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t message_type = 0;
    std::uint32_t flags = 0;
    std::uint32_t payload_length = 0;
    std::uint32_t command_id = 0;
    if (!read_u32(bytes, offset, magic) || !read_u16(bytes, offset, major)
        || !read_u16(bytes, offset, minor)
        || !read_u16(bytes, offset, message_type)
        || !read_u32(bytes, offset, flags)
        || !read_u32(bytes, offset, payload_length)) {
        return frame_error<Frame>(L"decode_frame", L"Frame header is truncated");
    }
    if (magic != kMagic) {
        return frame_error<Frame>(L"decode_frame", L"Frame magic is invalid");
    }
    if (major != kProtocolMajor) {
        return frame_error<Frame>(
            L"decode_frame", L"Frame protocol major version is incompatible");
    }
    if (message_type != static_cast<std::uint16_t>(MessageType::request)
        && message_type != static_cast<std::uint16_t>(MessageType::response)) {
        return frame_error<Frame>(L"decode_frame", L"Frame message type is unknown");
    }
    if (payload_length > kMaxPayloadBytes
        || bytes.size() - offset < 36) {
        return frame_error<Frame>(L"decode_frame", L"Frame payload is invalid");
    }
    auto request_id = read_uuid(bytes, offset);
    auto operation_id = read_uuid(bytes, offset);
    if (!request_id.ok() || !operation_id.ok()
        || !read_u32(bytes, offset, command_id)) {
        return frame_error<Frame>(L"decode_frame", L"Frame identity is truncated");
    }
    if (bytes.size() - offset != payload_length) {
        return frame_error<Frame>(L"decode_frame", L"Frame length is inconsistent");
    }
    Frame frame{
        major,
        minor,
        static_cast<MessageType>(message_type),
        flags,
        request_id.move_value(),
        operation_id.move_value(),
        command_id,
        std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                               bytes.end())};
    return Result<Frame>::success(std::move(frame));
}

} // namespace myrd::ipc
