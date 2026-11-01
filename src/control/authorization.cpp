#include "src/control/authorization.h"
#include "src/control/ipc_codec.h"

#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <new>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace myrd::ipc {

namespace {

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~NativeHandle() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
        }
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_{};
};

struct LocalFreeDeleter final {
    void operator()(void* value) const noexcept {
        if (value != nullptr) static_cast<void>(LocalFree(value));
    }
};

using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

class ImpersonationGuard final {
public:
    ImpersonationGuard() noexcept = default;
    ~ImpersonationGuard() noexcept {
        if (active_ && !RevertToSelf()) std::terminate();
    }
    ImpersonationGuard(const ImpersonationGuard&) = delete;
    ImpersonationGuard& operator=(const ImpersonationGuard&) = delete;

    void activate() noexcept { active_ = true; }

    Result<void> revert() {
        if (!active_) return Result<void>::success();
        if (!RevertToSelf()) {
            const auto error = GetLastError();
            return Result<void>::failure(Error{
                ErrorDomain::win32, error,
                L"authorization.revert",
                L"Failed to revert named pipe client impersonation", error});
        }
        active_ = false;
        return Result<void>::success();
    }

private:
    bool active_{};
};

Result<std::vector<std::byte>> read_token_user(HANDLE token) {
    DWORD bytes = 0;
    const BOOL sized = GetTokenInformation(
        token, TokenUser, nullptr, 0, &bytes);
    const auto size_error = sized ? ERROR_SUCCESS : GetLastError();
    if (sized || size_error != ERROR_INSUFFICIENT_BUFFER
        || bytes < sizeof(TOKEN_USER) || bytes > 64U * 1024U) {
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorDomain::win32, size_error,
            L"authorization.token_user",
            L"Failed to size the caller token user", size_error});
    }
    std::vector<std::byte> buffer(bytes);
    if (!GetTokenInformation(
            token, TokenUser, buffer.data(), bytes, &bytes)) {
        const auto error = GetLastError();
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.token_user",
            L"Failed to read the caller token user", error});
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (!IsValidSid(user->User.Sid)) {
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"authorization.token_user",
            L"The caller token contains an invalid primary SID"});
    }
    return Result<std::vector<std::byte>>::success(std::move(buffer));
}

Result<std::wstring> sid_to_string(PSID sid) {
    LPWSTR raw_sid = nullptr;
    if (!ConvertSidToStringSidW(sid, &raw_sid)) {
        const auto error = GetLastError();
        return Result<std::wstring>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.sid",
            L"Failed to encode a Windows SID", error});
    }
    LocalMemory raw_sid_owner(raw_sid);
    const std::wstring text(static_cast<const wchar_t*>(raw_sid_owner.get()));
    return Result<std::wstring>::success(text);
}

Result<bool> token_has_sid(HANDLE token, PSID sid) {
    BOOL member = FALSE;
    if (!CheckTokenMembership(token, sid, &member)) {
        const auto error = GetLastError();
        return Result<bool>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.token_membership",
            L"Failed to inspect caller token group membership", error});
    }
    return Result<bool>::success(member != FALSE);
}

Result<bool> token_has_well_known_sid(
    HANDLE token, WELL_KNOWN_SID_TYPE type) {
    std::byte storage[SECURITY_MAX_SID_SIZE]{};
    DWORD bytes = sizeof(storage);
    if (!CreateWellKnownSid(type, nullptr, storage, &bytes)) {
        const auto error = GetLastError();
        return Result<bool>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.well_known_sid",
            L"Failed to construct a Windows well-known SID", error});
    }
    return token_has_sid(token, storage);
}

Result<std::wstring> current_process_user_sid_string() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        const auto error = GetLastError();
        return Result<std::wstring>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.process_token",
            L"Failed to open the public pipe server process token", error});
    }
    NativeHandle token(raw_token);

    auto user = read_token_user(token.get());
    if (!user.ok()) return Result<std::wstring>::failure(user.error());
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(
        user.value().data());
    return sid_to_string(token_user->User.Sid);
}

} // namespace

std::optional<AuthorizationRule> public_command_authorization_rule(
    std::uint32_t command_id) noexcept {
    switch (static_cast<CommandId>(command_id)) {
    case CommandId::status:
    case CommandId::disk_list:
    case CommandId::operation_list:
    case CommandId::diagnostics_collect:
        return AuthorizationRule::caller_filtered;
    case CommandId::disk_show:
    case CommandId::disk_start:
    case CommandId::disk_stop:
    case CommandId::disk_checkpoint:
    case CommandId::disk_compact:
    case CommandId::disk_config_get:
    case CommandId::disk_config_set:
    case CommandId::operation_show:
    case CommandId::operation_wait:
    case CommandId::operation_cancel:
        return AuthorizationRule::owner_or_administrator;
    // **Export is administrator-only (B3).**  The file is created by the SYSTEM worker at a
    // path the caller names, after only a lexical absolute-path check; a profile owner who is
    // not an administrator could therefore make SYSTEM create a file in a directory the owner
    // cannot write (including through a junction parent).  Requiring an administrator removes
    // that escalation: the export feature writes across a trust boundary, so it belongs in the
    // same group as create/import/forget rather than in the owner-scoped group.
    case CommandId::disk_export:
    case CommandId::disk_create:
    case CommandId::disk_import:
    case CommandId::disk_repair_profile:
    case CommandId::disk_rebind:
    case CommandId::disk_forget:
        return AuthorizationRule::administrator_only;
    case CommandId::driver_status:
        return AuthorizationRule::authenticated_read_only;
    }
    return std::nullopt;
}

AuthorizationDecision authorize_public_command(
    std::uint32_t command_id,
    const CallerIdentity& caller,
    std::optional<std::wstring_view> resource_owner_sid) noexcept {
    auto parse_sid = [](std::wstring_view text, PSID* sid) noexcept {
        *sid = nullptr;
        if (text.empty() || text.find(L'\0') != std::wstring_view::npos) {
            return false;
        }
        try {
            const std::wstring terminated(text);
            return ConvertStringSidToSidW(terminated.c_str(), sid) != FALSE;
        } catch (...) {
            if (*sid != nullptr) {
                static_cast<void>(LocalFree(*sid));
                *sid = nullptr;
            }
            return false;
        }
    };

    PSID caller_sid = nullptr;
    if (!caller.authenticated || caller.anonymous || caller.network
        || !parse_sid(caller.primary_sid, &caller_sid)) {
        if (caller_sid != nullptr) static_cast<void>(LocalFree(caller_sid));
        return {};
    }
    LocalMemory caller_sid_owner(caller_sid);
    const auto rule = public_command_authorization_rule(command_id);
    if (!rule.has_value()) return {};

    const bool trusted_administrator = caller.local_system
        || caller.service_identity
        || (caller.administrator && caller.elevated);
    AuthorizationDecision decision{};
    switch (rule.value()) {
    case AuthorizationRule::caller_filtered:
        decision = {true, trusted_administrator
            ? AuthorizationVisibility::all
            : AuthorizationVisibility::caller_only};
        break;
    case AuthorizationRule::authenticated_read_only:
        decision = {true, AuthorizationVisibility::all};
        break;
    case AuthorizationRule::administrator_only:
        decision = {trusted_administrator,
                    trusted_administrator ? AuthorizationVisibility::all
                                          : AuthorizationVisibility::none};
        break;
    case AuthorizationRule::owner_or_administrator:
        if (trusted_administrator) {
            decision = {true, AuthorizationVisibility::all};
            break;
        }
        if (!resource_owner_sid.has_value()) break;
        PSID owner_sid = nullptr;
        if (!parse_sid(resource_owner_sid.value(), &owner_sid)) break;
        LocalMemory owner_sid_owner(owner_sid);
        const bool same = EqualSid(caller_sid, owner_sid) != FALSE;
        decision = {same, same ? AuthorizationVisibility::caller_only
                               : AuthorizationVisibility::none};
        break;
    }
    return decision;
}

namespace {

Result<CallerIdentity> capture_named_pipe_caller_identity_impl(
    void* connected_pipe) {
    HANDLE existing = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &existing)) {
        static_cast<void>(CloseHandle(existing));
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"authorization.impersonate",
            L"Public pipe worker already has an impersonation token"});
    }
    if (GetLastError() != ERROR_NO_TOKEN) {
        const auto error = GetLastError();
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.impersonate",
            L"Failed to inspect the public pipe worker token state", error});
    }

    if (!ImpersonateNamedPipeClient(static_cast<HANDLE>(connected_pipe))) {
        const auto error = GetLastError();
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.impersonate",
            L"Failed to impersonate the public pipe caller", error});
    }
    ImpersonationGuard impersonation;
    impersonation.activate();

    HANDLE raw_token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
        const auto error = GetLastError();
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.thread_token",
            L"Failed to open the impersonated caller token", error});
    }
    NativeHandle token(raw_token);
    auto user = read_token_user(token.get());
    if (!user.ok()) return Result<CallerIdentity>::failure(user.error());
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(
        user.value().data());
    auto primary_sid = sid_to_string(token_user->User.Sid);
    if (!primary_sid.ok()) {
        return Result<CallerIdentity>::failure(primary_sid.error());
    }

    std::byte anonymous_storage[SECURITY_MAX_SID_SIZE]{};
    DWORD anonymous_bytes = sizeof(anonymous_storage);
    if (!CreateWellKnownSid(
            WinAnonymousSid, nullptr, anonymous_storage, &anonymous_bytes)) {
        const auto error = GetLastError();
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.anonymous_sid",
            L"Failed to construct the anonymous SID", error});
    }
    const bool anonymous = EqualSid(
        token_user->User.Sid, anonymous_storage) != FALSE;

    auto network = token_has_well_known_sid(token.get(), WinNetworkSid);
    auto authenticated = token_has_well_known_sid(
        token.get(), WinAuthenticatedUserSid);
    auto administrator = token_has_well_known_sid(
        token.get(), WinBuiltinAdministratorsSid);
    auto system = token_has_well_known_sid(token.get(), WinLocalSystemSid);
    if (!network.ok()) return Result<CallerIdentity>::failure(network.error());
    if (!authenticated.ok()) {
        return Result<CallerIdentity>::failure(authenticated.error());
    }
    if (!administrator.ok()) {
        return Result<CallerIdentity>::failure(administrator.error());
    }
    if (!system.ok()) return Result<CallerIdentity>::failure(system.error());

    PSID raw_service_sid = nullptr;
    if (!ConvertStringSidToSidW(kMyRamDiskServiceSid.data(), &raw_service_sid)) {
        const auto error = GetLastError();
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.service_sid",
            L"Failed to construct the MyRamDisk service SID", error});
    }
    LocalMemory service_sid_owner(raw_service_sid);
    auto service = token_has_sid(token.get(), raw_service_sid);
    if (!service.ok()) return Result<CallerIdentity>::failure(service.error());

    TOKEN_ELEVATION elevation{};
    DWORD elevation_bytes = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation,
                             sizeof(elevation), &elevation_bytes)) {
        const auto error = GetLastError();
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::win32, error,
            L"authorization.elevation",
            L"Failed to inspect caller token elevation", error});
    }

    if (anonymous || network.value() || !authenticated.value()) {
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::win32, ERROR_ACCESS_DENIED,
            L"authorization.identity",
            L"Anonymous, network, or unauthenticated callers are denied",
            ERROR_ACCESS_DENIED});
    }

    CallerIdentity identity{
        primary_sid.move_value(),
        true,
        anonymous,
        network.value(),
        administrator.value(),
        elevation.TokenIsElevated != 0,
        system.value(),
        service.value()};
    auto reverted = impersonation.revert();
    if (!reverted.ok()) {
        return Result<CallerIdentity>::failure(reverted.error());
    }
    return Result<CallerIdentity>::success(std::move(identity));
}

} // namespace

Result<CallerIdentity> capture_named_pipe_caller_identity(
    void* connected_pipe) {
    try {
        return capture_named_pipe_caller_identity_impl(connected_pipe);
    } catch (...) {
        return Result<CallerIdentity>::failure(Error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"authorization.identity",
            L"Failed to allocate caller authorization state"});
    }
}

struct PublicPipeSecurityDescriptor::Impl final {
    explicit Impl(PSECURITY_DESCRIPTOR value) noexcept
        : descriptor(value) {
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
    }

    ~Impl() noexcept {
        if (descriptor != nullptr) LocalFree(descriptor);
    }

    PSECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};
};

PublicPipeSecurityDescriptor::PublicPipeSecurityDescriptor(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PublicPipeSecurityDescriptor::PublicPipeSecurityDescriptor(
    PublicPipeSecurityDescriptor&&) noexcept = default;

PublicPipeSecurityDescriptor& PublicPipeSecurityDescriptor::operator=(
    PublicPipeSecurityDescriptor&&) noexcept = default;

PublicPipeSecurityDescriptor::~PublicPipeSecurityDescriptor() noexcept = default;

void* PublicPipeSecurityDescriptor::native_descriptor() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->descriptor;
}

void* PublicPipeSecurityDescriptor::native_security_attributes() noexcept {
    return impl_ == nullptr ? nullptr : &impl_->attributes;
}

const void* PublicPipeSecurityDescriptor::native_security_attributes()
    const noexcept {
    return impl_ == nullptr ? nullptr : &impl_->attributes;
}

Result<PublicPipeSecurityDescriptor>
create_public_pipe_security_descriptor(
    PublicPipeMandatoryLabel mandatory_label) {
    try {
        static_assert((kPublicPipeClientAccessMask & 0x00000004U) == 0U);
        static_assert((kPublicPipeClientAccessMask & 0xF0000000U) == 0U);

        auto process_sid = current_process_user_sid_string();
        if (!process_sid.ok()) {
            return Result<PublicPipeSecurityDescriptor>::failure(
                process_sid.error());
        }

        std::wstring sddl =
            L"O:" + process_sid.value() + L"G:BAD:P"
            L"(D;;GA;;;AN)"
            L"(D;;GA;;;NU)"
            L"(A;;GA;;;SY)"
            L"(A;;GA;;;" + std::wstring(kMyRamDiskServiceSid) + L")"
            L"(A;;GA;;;" + process_sid.value() + L")"
            L"(A;;0x00100183;;;BA)"
            L"(A;;0x00100183;;;AU)";
        if (mandatory_label == PublicPipeMandatoryLabel::include) {
            sddl += L"S:P(ML;;NW;;;ME)";
        }

        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            const auto error = GetLastError();
            return Result<PublicPipeSecurityDescriptor>::failure(Error{
                ErrorDomain::win32,
                error,
                L"authorization.security_descriptor",
                L"Failed to build the public pipe security descriptor",
                error});
        }
        LocalMemory descriptor_owner(descriptor);

        auto impl = std::unique_ptr<PublicPipeSecurityDescriptor::Impl>(
            new (std::nothrow) PublicPipeSecurityDescriptor::Impl(descriptor));
        if (impl == nullptr) {
            return Result<PublicPipeSecurityDescriptor>::failure(Error{
                ErrorDomain::internal,
                static_cast<std::uint32_t>(
                    OperationErrorCode::internal_invariant),
                L"authorization.security_descriptor",
                L"Failed to allocate public pipe security state"});
        }
        static_cast<void>(descriptor_owner.release());
        return Result<PublicPipeSecurityDescriptor>::success(
            PublicPipeSecurityDescriptor(std::move(impl)));
    } catch (...) {
        return Result<PublicPipeSecurityDescriptor>::failure(Error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"authorization.security_descriptor",
            L"Failed to allocate public pipe security state"});
    }
}

} // namespace myrd::ipc
