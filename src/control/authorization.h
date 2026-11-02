#pragma once

#include "src/common/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace myrd::ipc {

inline constexpr std::wstring_view kMyRamDiskServiceSid =
    L"S-1-5-80-1675415408-3648040842-263802843-130580959-3433450740";

// FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
// FILE_WRITE_ATTRIBUTES | SYNCHRONIZE | READ_CONTROL. In particular this excludes
// FILE_CREATE_PIPE_INSTANCE and all generic rights.
//
// **READ_CONTROL is there for the client's server-identity check (B7, R-11).**  A non-elevated
// client cannot open the service's process, so it falls back to the pipe's *owner*; reading the
// owner needs READ_CONTROL on the pipe, and nothing else is granted by it.
inline constexpr std::uint32_t kPublicPipeClientAccessMask = 0x00120183U;

enum class PublicPipeMandatoryLabel : std::uint8_t {
    include,
    omit_for_privilege_fallback,
};

struct CallerIdentity {
    std::wstring primary_sid;
    bool authenticated{};
    bool anonymous{};
    bool network{};
    bool administrator{};
    bool elevated{};
    bool local_system{};
    bool service_identity{};
};

enum class AuthorizationRule : std::uint8_t {
    caller_filtered,
    owner_or_administrator,
    administrator_only,
    authenticated_read_only,
};

enum class AuthorizationVisibility : std::uint8_t {
    none,
    caller_only,
    all,
};

struct AuthorizationDecision {
    bool allowed{};
    AuthorizationVisibility visibility{AuthorizationVisibility::none};
};

std::optional<AuthorizationRule> public_command_authorization_rule(
    std::uint32_t command_id) noexcept;
AuthorizationDecision authorize_public_command(
    std::uint32_t command_id,
    const CallerIdentity& caller,
    std::optional<std::wstring_view> resource_owner_sid) noexcept;

Result<CallerIdentity> capture_named_pipe_caller_identity(
    void* connected_pipe);

class PublicPipeSecurityDescriptor final {
public:
    PublicPipeSecurityDescriptor(PublicPipeSecurityDescriptor&&) noexcept;
    PublicPipeSecurityDescriptor& operator=(
        PublicPipeSecurityDescriptor&&) noexcept;
    ~PublicPipeSecurityDescriptor() noexcept;

    PublicPipeSecurityDescriptor(const PublicPipeSecurityDescriptor&) = delete;
    PublicPipeSecurityDescriptor& operator=(
        const PublicPipeSecurityDescriptor&) = delete;

    void* native_descriptor() const noexcept;
    void* native_security_attributes() noexcept;
    const void* native_security_attributes() const noexcept;

private:
    struct Impl;
    explicit PublicPipeSecurityDescriptor(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend Result<PublicPipeSecurityDescriptor>
        create_public_pipe_security_descriptor(PublicPipeMandatoryLabel);
};

Result<PublicPipeSecurityDescriptor>
    create_public_pipe_security_descriptor(
        PublicPipeMandatoryLabel mandatory_label =
            PublicPipeMandatoryLabel::include);

} // namespace myrd::ipc
