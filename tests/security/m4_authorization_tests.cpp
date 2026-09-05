#include "src/control/authorization.h"
#include "src/control/ipc_codec.h"
#include "src/control/pipe_transport.h"

#include "tests/support/test_framework.h"

#define NOMINMAX
#include <Windows.h>
#include <AclAPI.h>
#include <Sddl.h>

#include <cstdint>
#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class LocalSid final {
public:
    explicit LocalSid(std::wstring_view text) {
        REQUIRE(ConvertStringSidToSidW(std::wstring(text).c_str(), &sid_));
    }
    ~LocalSid() {
        if (sid_ != nullptr) LocalFree(sid_);
    }
    LocalSid(const LocalSid&) = delete;
    LocalSid& operator=(const LocalSid&) = delete;
    PSID get() const noexcept { return sid_; }

private:
    PSID sid_{};
};

bool has_ace(const ACL* acl,
             BYTE ace_type,
             ACCESS_MASK mask,
             PSID expected_sid) {
    ACL_SIZE_INFORMATION size{};
    REQUIRE(GetAclInformation(
        const_cast<ACL*>(acl), &size, sizeof(size), AclSizeInformation));
    for (DWORD index = 0; index < size.AceCount; ++index) {
        void* raw = nullptr;
        REQUIRE(GetAce(const_cast<ACL*>(acl), index, &raw));
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ace_type) continue;

        ACCESS_MASK actual_mask = 0;
        const void* sid = nullptr;
        if (ace_type == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            actual_mask = ace->Mask;
            sid = &ace->SidStart;
        } else if (ace_type == ACCESS_DENIED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_DENIED_ACE*>(raw);
            actual_mask = ace->Mask;
            sid = &ace->SidStart;
        } else {
            continue;
        }
        if (actual_mask == mask
            && EqualSid(const_cast<void*>(sid), expected_sid)) {
            return true;
        }
    }
    return false;
}

bool has_medium_no_write_up_label(const ACL* acl) {
    ACL_SIZE_INFORMATION size{};
    REQUIRE(GetAclInformation(
        const_cast<ACL*>(acl), &size, sizeof(size), AclSizeInformation));
    LocalSid medium(L"S-1-16-8192");
    for (DWORD index = 0; index < size.AceCount; ++index) {
        void* raw = nullptr;
        REQUIRE(GetAce(const_cast<ACL*>(acl), index, &raw));
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
        const auto* ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(raw);
        if (ace->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP
            && EqualSid(const_cast<DWORD*>(&ace->SidStart), medium.get())) {
            return true;
        }
    }
    return false;
}

std::vector<std::byte> current_process_token_user() {
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD bytes = 0;
    REQUIRE(!GetTokenInformation(token, TokenUser, nullptr, 0, &bytes));
    REQUIRE(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    std::vector<std::byte> buffer(bytes);
    REQUIRE(GetTokenInformation(
        token, TokenUser, buffer.data(), bytes, &bytes));
    REQUIRE(CloseHandle(token));
    return buffer;
}

bool current_thread_has_token() {
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        REQUIRE(CloseHandle(token));
        return true;
    }
    REQUIRE(GetLastError() == ERROR_NO_TOKEN);
    return false;
}

std::wstring unique_pipe_name() {
    static std::atomic_uint32_t sequence{0};
    return L"\\\\.\\pipe\\MyRamDisk.AuthTest."
        + std::to_wstring(GetCurrentProcessId()) + L"."
        + std::to_wstring(sequence.fetch_add(1));
}

myrd::Result<myrd::ipc::CallerIdentity> capture_same_process_client() {
    const auto name = unique_pipe_name();
    HANDLE release_client = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(release_client != nullptr);
    HANDLE server = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    REQUIRE(server != INVALID_HANDLE_VALUE);

    std::thread client([&] {
        HANDLE handle = INVALID_HANDLE_VALUE;
        while (handle == INVALID_HANDLE_VALUE) {
            handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                REQUIRE(GetLastError() == ERROR_PIPE_BUSY);
                REQUIRE(WaitNamedPipeW(name.c_str(), 1000));
            }
        }
        const std::byte marker{0x5A};
        DWORD written = 0;
        REQUIRE(WriteFile(handle, &marker, 1, &written, nullptr));
        REQUIRE(written == 1);
        REQUIRE(WaitForSingleObject(release_client, 5000) == WAIT_OBJECT_0);
        REQUIRE(CloseHandle(handle));
    });

    const BOOL connected = ConnectNamedPipe(server, nullptr);
    REQUIRE(connected || GetLastError() == ERROR_PIPE_CONNECTED);
    std::byte marker{};
    DWORD read = 0;
    REQUIRE(ReadFile(server, &marker, 1, &read, nullptr));
    REQUIRE(read == 1);
    REQUIRE(marker == std::byte{0x5A});

    auto identity = myrd::ipc::capture_named_pipe_caller_identity(server);
    REQUIRE(SetEvent(release_client));
    client.join();
    REQUIRE(CloseHandle(server));
    REQUIRE(CloseHandle(release_client));
    return identity;
}

bool current_process_is_enabled_admin() {
    LocalSid administrators(L"S-1-5-32-544");
    BOOL member = FALSE;
    REQUIRE(CheckTokenMembership(nullptr, administrators.get(), &member));
    return member != FALSE;
}

bool current_process_is_elevated() {
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    REQUIRE(GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &bytes));
    REQUIRE(CloseHandle(token));
    return elevation.TokenIsElevated != 0;
}

} // namespace

TEST_CASE(public_pipe_security_descriptor_has_exact_owner_and_dacl) {
    auto descriptor = myrd::ipc::create_public_pipe_security_descriptor();
    REQUIRE(descriptor.ok());

    auto* security = static_cast<PSECURITY_DESCRIPTOR>(
        descriptor.value().native_descriptor());
    REQUIRE(IsValidSecurityDescriptor(security));

    PSID owner = nullptr;
    BOOL owner_defaulted = TRUE;
    REQUIRE(GetSecurityDescriptorOwner(
        security, &owner, &owner_defaulted));
    REQUIRE(!owner_defaulted);
    auto token_user_buffer = current_process_token_user();
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(
        token_user_buffer.data());
    REQUIRE(EqualSid(owner, token_user->User.Sid));

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    REQUIRE(GetSecurityDescriptorControl(security, &control, &revision));
    REQUIRE((control & SE_DACL_PRESENT) != 0);
    REQUIRE((control & SE_DACL_PROTECTED) != 0);

    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = TRUE;
    PACL dacl = nullptr;
    REQUIRE(GetSecurityDescriptorDacl(
        security, &dacl_present, &dacl, &dacl_defaulted));
    REQUIRE(dacl_present);
    REQUIRE(dacl != nullptr);
    REQUIRE(!dacl_defaulted);

    LocalSid service(myrd::ipc::kMyRamDiskServiceSid);
    LocalSid system(L"S-1-5-18");
    LocalSid administrators(L"S-1-5-32-544");
    LocalSid authenticated_users(L"S-1-5-11");
    LocalSid anonymous(L"S-1-5-7");
    LocalSid network(L"S-1-5-2");

    REQUIRE(has_ace(dacl, ACCESS_DENIED_ACE_TYPE, GENERIC_ALL,
                    anonymous.get()));
    REQUIRE(has_ace(dacl, ACCESS_DENIED_ACE_TYPE, GENERIC_ALL,
                    network.get()));
    REQUIRE(has_ace(dacl, ACCESS_ALLOWED_ACE_TYPE, GENERIC_ALL,
                    system.get()));
    REQUIRE(has_ace(dacl, ACCESS_ALLOWED_ACE_TYPE, GENERIC_ALL,
                    service.get()));
    REQUIRE(has_ace(dacl, ACCESS_ALLOWED_ACE_TYPE, GENERIC_ALL,
                    token_user->User.Sid));
    REQUIRE(has_ace(dacl, ACCESS_ALLOWED_ACE_TYPE,
                    myrd::ipc::kPublicPipeClientAccessMask,
                    administrators.get()));
    REQUIRE(has_ace(dacl, ACCESS_ALLOWED_ACE_TYPE,
                    myrd::ipc::kPublicPipeClientAccessMask,
                    authenticated_users.get()));

    const auto* attributes = static_cast<const SECURITY_ATTRIBUTES*>(
        descriptor.value().native_security_attributes());
    REQUIRE(attributes != nullptr);
    REQUIRE(attributes->nLength == sizeof(SECURITY_ATTRIBUTES));
    REQUIRE(attributes->lpSecurityDescriptor == security);
    REQUIRE(!attributes->bInheritHandle);
    REQUIRE((myrd::ipc::kPublicPipeModeFlags & PIPE_REJECT_REMOTE_CLIENTS) != 0);
}

TEST_CASE(public_pipe_security_descriptor_has_protected_medium_sacl) {
    auto descriptor = myrd::ipc::create_public_pipe_security_descriptor();
    REQUIRE(descriptor.ok());
    auto* security = static_cast<PSECURITY_DESCRIPTOR>(
        descriptor.value().native_descriptor());

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    REQUIRE(GetSecurityDescriptorControl(security, &control, &revision));
    REQUIRE((control & SE_SACL_PRESENT) != 0);
    REQUIRE((control & SE_SACL_PROTECTED) != 0);

    BOOL sacl_present = FALSE;
    BOOL sacl_defaulted = TRUE;
    PACL sacl = nullptr;
    REQUIRE(GetSecurityDescriptorSacl(
        security, &sacl_present, &sacl, &sacl_defaulted));
    REQUIRE(sacl_present);
    REQUIRE(sacl != nullptr);
    REQUIRE(!sacl_defaulted);
    REQUIRE(has_medium_no_write_up_label(sacl));
}

TEST_CASE(public_pipe_security_descriptor_has_narrow_no_sacl_fallback) {
    auto descriptor = myrd::ipc::create_public_pipe_security_descriptor(
        myrd::ipc::PublicPipeMandatoryLabel::omit_for_privilege_fallback);
    REQUIRE(descriptor.ok());
    auto* security = static_cast<PSECURITY_DESCRIPTOR>(
        descriptor.value().native_descriptor());

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    REQUIRE(GetSecurityDescriptorControl(security, &control, &revision));
    REQUIRE((control & SE_DACL_PRESENT) != 0);
    REQUIRE((control & SE_DACL_PROTECTED) != 0);
    REQUIRE((control & SE_SACL_PRESENT) == 0);
}

TEST_CASE(caller_capture_rejects_impersonation_failure_without_token_leak) {
    REQUIRE(!current_thread_has_token());
    auto identity = myrd::ipc::capture_named_pipe_caller_identity(
        INVALID_HANDLE_VALUE);
    REQUIRE(!identity.ok());
    REQUIRE(identity.error().domain == myrd::ErrorDomain::win32);
    REQUIRE(!current_thread_has_token());
}

TEST_CASE(caller_capture_uses_primary_client_sid_and_always_reverts) {
    REQUIRE(!current_thread_has_token());
    auto identity = capture_same_process_client();
    REQUIRE(identity.ok());
    REQUIRE(!current_thread_has_token());

    auto token_user_buffer = current_process_token_user();
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(
        token_user_buffer.data());
    LPWSTR expected_sid = nullptr;
    REQUIRE(ConvertSidToStringSidW(token_user->User.Sid, &expected_sid));
    REQUIRE_EQ(identity.value().primary_sid, std::wstring(expected_sid));
    LocalFree(expected_sid);

    REQUIRE(identity.value().authenticated);
    REQUIRE(!identity.value().anonymous);
    REQUIRE(!identity.value().network);
    REQUIRE(identity.value().administrator
            == current_process_is_enabled_admin());
    REQUIRE(identity.value().elevated == current_process_is_elevated());
}

TEST_CASE(caller_capture_refuses_to_overwrite_a_preexisting_thread_token) {
    HANDLE process_token = nullptr;
    REQUIRE(OpenProcessToken(
        GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process_token));
    HANDLE impersonation_token = nullptr;
    REQUIRE(DuplicateTokenEx(
        process_token, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
        SecurityImpersonation, TokenImpersonation, &impersonation_token));
    REQUIRE(CloseHandle(process_token));
    REQUIRE(SetThreadToken(nullptr, impersonation_token));
    REQUIRE(current_thread_has_token());

    auto identity = myrd::ipc::capture_named_pipe_caller_identity(
        INVALID_HANDLE_VALUE);
    REQUIRE(!identity.ok());
    REQUIRE(identity.error().domain == myrd::ErrorDomain::internal);
    REQUIRE(current_thread_has_token());

    REQUIRE(RevertToSelf());
    REQUIRE(CloseHandle(impersonation_token));
    REQUIRE(!current_thread_has_token());
}

TEST_CASE(command_authorization_rules_cover_the_exact_public_matrix) {
    using myrd::ipc::AuthorizationRule;
    using myrd::ipc::CommandId;
    struct Expected {
        CommandId command;
        AuthorizationRule rule;
    };
    constexpr Expected expected[] = {
        {CommandId::status, AuthorizationRule::caller_filtered},
        {CommandId::disk_list, AuthorizationRule::caller_filtered},
        {CommandId::disk_show, AuthorizationRule::owner_or_administrator},
        {CommandId::disk_create, AuthorizationRule::administrator_only},
        {CommandId::disk_import, AuthorizationRule::administrator_only},
        {CommandId::disk_start, AuthorizationRule::owner_or_administrator},
        {CommandId::disk_stop, AuthorizationRule::owner_or_administrator},
        {CommandId::disk_checkpoint, AuthorizationRule::owner_or_administrator},
        {CommandId::disk_compact, AuthorizationRule::owner_or_administrator},
        {CommandId::disk_config_get, AuthorizationRule::owner_or_administrator},
        {CommandId::disk_config_set, AuthorizationRule::owner_or_administrator},
        {CommandId::disk_repair_profile, AuthorizationRule::administrator_only},
        {CommandId::disk_rebind, AuthorizationRule::administrator_only},
        {CommandId::disk_forget, AuthorizationRule::administrator_only},
        {CommandId::operation_list, AuthorizationRule::caller_filtered},
        {CommandId::operation_show, AuthorizationRule::owner_or_administrator},
        {CommandId::operation_wait, AuthorizationRule::owner_or_administrator},
        {CommandId::operation_cancel, AuthorizationRule::owner_or_administrator},
        {CommandId::diagnostics_collect, AuthorizationRule::caller_filtered},
        {CommandId::driver_status, AuthorizationRule::authenticated_read_only},
    };
    for (const auto& item : expected) {
        const auto rule = myrd::ipc::public_command_authorization_rule(
            static_cast<std::uint32_t>(item.command));
        REQUIRE(rule.has_value());
        REQUIRE(rule.value() == item.rule);
    }
    REQUIRE(!myrd::ipc::public_command_authorization_rule(0).has_value());
    REQUIRE(!myrd::ipc::public_command_authorization_rule(21).has_value());
}

TEST_CASE(standard_user_is_filtered_can_use_owned_resources_and_cannot_admin) {
    const myrd::ipc::CallerIdentity caller{
        L"S-1-5-21-100-200-300-1001", true, false, false,
        false, false, false, false};
    auto filtered = myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_list),
        caller, std::nullopt);
    REQUIRE(filtered.allowed);
    REQUIRE(filtered.visibility == myrd::ipc::AuthorizationVisibility::caller_only);

    auto owned = myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_checkpoint),
        caller, std::wstring_view(L"S-1-5-21-100-200-300-1001"));
    REQUIRE(owned.allowed);
    auto foreign = myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_checkpoint),
        caller, std::wstring_view(L"S-1-5-21-100-200-300-1002"));
    REQUIRE(!foreign.allowed);
    auto missing = myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_checkpoint),
        caller, std::nullopt);
    REQUIRE(!missing.allowed);
    auto create = myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_create),
        caller, std::nullopt);
    REQUIRE(!create.allowed);
    auto elevated_non_admin = caller;
    elevated_non_admin.elevated = true;
    REQUIRE(!myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_create),
        elevated_non_admin, std::nullopt).allowed);
}

TEST_CASE(admin_requires_elevation_while_system_and_service_are_trusted) {
    auto filtered_admin = myrd::ipc::CallerIdentity{
        L"S-1-5-21-100-200-300-1001", true, false, false,
        true, false, false, false};
    auto denied = myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_rebind),
        filtered_admin, std::nullopt);
    REQUIRE(!denied.allowed);

    auto elevated_admin = filtered_admin;
    elevated_admin.elevated = true;
    auto allowed = myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_rebind),
        elevated_admin, std::nullopt);
    REQUIRE(allowed.allowed);
    REQUIRE(allowed.visibility == myrd::ipc::AuthorizationVisibility::all);

    auto system = filtered_admin;
    system.administrator = false;
    system.local_system = true;
    REQUIRE(myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_forget),
        system, std::nullopt).allowed);
    auto service = filtered_admin;
    service.administrator = false;
    service.service_identity = true;
    REQUIRE(myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_forget),
        service, std::nullopt).allowed);
}

TEST_CASE(anonymous_network_invalid_sid_and_unknown_commands_fail_closed) {
    auto caller = myrd::ipc::CallerIdentity{
        L"S-1-5-21-100-200-300-1001", true, false, false,
        false, false, false, false};
    for (const auto command : {1U, 3U, 4U, 20U, 999U}) {
        auto anonymous = caller;
        anonymous.anonymous = true;
        REQUIRE(!myrd::ipc::authorize_public_command(
            command, anonymous, caller.primary_sid).allowed);
        auto network = caller;
        network.network = true;
        REQUIRE(!myrd::ipc::authorize_public_command(
            command, network, caller.primary_sid).allowed);
        auto unauthenticated = caller;
        unauthenticated.authenticated = false;
        REQUIRE(!myrd::ipc::authorize_public_command(
            command, unauthenticated, caller.primary_sid).allowed);
    }
    caller.primary_sid = L"not-a-sid";
    REQUIRE(!myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::driver_status),
        caller, std::nullopt).allowed);

    std::wstring embedded_nul = L"S-1-5-21-100-200-300-1001";
    embedded_nul.push_back(L'\0');
    embedded_nul += L"S-1-5-18";
    caller.primary_sid = embedded_nul;
    REQUIRE(!myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::driver_status),
        caller, std::nullopt).allowed);

    caller.primary_sid = L"S-1-5-21-100-200-300-1001";
    REQUIRE(!myrd::ipc::authorize_public_command(
        static_cast<std::uint32_t>(myrd::ipc::CommandId::disk_checkpoint),
        caller, embedded_nul).allowed);
}
