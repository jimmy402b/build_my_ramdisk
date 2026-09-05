#include "src/management/windows_service.h"
#include "src/management/windows_worker_recovery.h"

#include "src/host/host_ipc.h"
#include "src/control/pipe_transport.h"
#include "src/profile/profile_store.h"
#include "src/profile/profile_validation.h"
#include "src/product/product_version.h"
#include "src/setup/driver_manifest.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <AclAPI.h>
#include <ShlObj.h>
#include <sddl.h>
#include <winsvc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace myrd::management::windows {
namespace {

template <class T>
Result<T> service_error(
    const WindowsServiceError code,
    const wchar_t* operation,
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return Result<T>::failure({
        native_code == 0 ? ErrorDomain::protocol : ErrorDomain::win32,
        static_cast<std::uint32_t>(code),
        operation,
        message,
        native_code});
}

bool is_zero_uuid(const product::Uuid& id) noexcept {
    return std::all_of(
        id.bytes().begin(), id.bytes().end(),
        [](const std::byte value) { return value == std::byte{}; });
}

std::uint64_t file_time_value(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U)
        | value.dwLowDateTime;
}

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~NativeHandle() noexcept { reset(); }
    NativeHandle(NativeHandle&& other) noexcept : value_(other.release()) {}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (*this) static_cast<void>(CloseHandle(value_));
        value_ = replacement;
    }

private:
    HANDLE value_{};
};

class LocalSecurityDescriptor final {
public:
    explicit LocalSecurityDescriptor(PSECURITY_DESCRIPTOR value = nullptr)
        noexcept : value_(value) {}
    ~LocalSecurityDescriptor() noexcept {
        if (value_ != nullptr) LocalFree(value_);
    }
    LocalSecurityDescriptor(LocalSecurityDescriptor&& other) noexcept
        : value_(other.release()) {}
    LocalSecurityDescriptor& operator=(
        LocalSecurityDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) LocalFree(value_);
            value_ = other.release();
        }
        return *this;
    }
    LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
    LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) =
        delete;
    PSECURITY_DESCRIPTOR get() const noexcept { return value_; }

private:
    PSECURITY_DESCRIPTOR release() noexcept {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }
    PSECURITY_DESCRIPTOR value_{};
};

Result<NativeHandle> create_service_singleton(
    const bool system_owner) {
    const wchar_t* sddl = system_owner
        ? L"O:SYG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)"
        : L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)";
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &raw_descriptor, nullptr)) {
        return service_error<NativeHandle>(
            WindowsServiceError::startup_failed,
            L"windows_service.singleton_security",
            L"The service singleton security descriptor is invalid",
            GetLastError());
    }
    LocalSecurityDescriptor descriptor(raw_descriptor);
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor.get();
    NativeHandle singleton(CreateMutexW(
        &attributes, FALSE, std::wstring(kServiceSingletonName).c_str()));
    if (!singleton) {
        return service_error<NativeHandle>(
            WindowsServiceError::startup_failed,
            L"windows_service.singleton_create",
            L"The protected service singleton could not be created",
            GetLastError());
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return service_error<NativeHandle>(
            WindowsServiceError::singleton_exists,
            L"windows_service.singleton_create",
            L"Another service manager already owns the singleton");
    }
    return Result<NativeHandle>::success(std::move(singleton));
}

Result<std::wstring> trusted_program_data_root() {
    PWSTR raw = nullptr;
    const HRESULT status = SHGetKnownFolderPath(
        FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(status) || raw == nullptr) {
        return service_error<std::wstring>(
            WindowsServiceError::startup_failed,
            L"windows_service.program_data",
            L"The trusted ProgramData root could not be resolved",
            static_cast<std::uint32_t>(status));
    }
    struct KnownFolderOwner final {
        PWSTR value{};
        ~KnownFolderOwner() noexcept { CoTaskMemFree(value); }
    } owner{raw};
    try {
        std::filesystem::path root(raw);
        root = root.lexically_normal();
        if (!root.is_absolute()) {
            return service_error<std::wstring>(
                WindowsServiceError::startup_failed,
                L"windows_service.program_data",
                L"ProgramData did not resolve to an absolute path");
        }
        return Result<std::wstring>::success(root.wstring());
    } catch (...) {
        return service_error<std::wstring>(
            WindowsServiceError::startup_failed,
            L"windows_service.program_data",
            L"ProgramData path allocation failed");
    }
}

std::wstring module_path() {
    std::wstring result(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, result.data(), static_cast<DWORD>(result.size()));
    if (length == 0 || length >= result.size()) return {};
    result.resize(length);
    return result;
}

bool equal_path(const std::wstring& left, const std::wstring& right) noexcept {
    return CompareStringOrdinal(
        left.c_str(), static_cast<int>(left.size()),
        right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool has_reparse_component(const std::filesystem::path& path) {
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return true;
        }
    }
    return false;
}

bool sid_matches_account(PSID sid, const wchar_t* account) noexcept {
    DWORD sid_bytes = 0;
    DWORD domain_chars = 0;
    SID_NAME_USE use{};
    static_cast<void>(LookupAccountNameW(
        nullptr, account, nullptr, &sid_bytes, nullptr, &domain_chars, &use));
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sid_bytes == 0) {
        return false;
    }
    try {
        std::vector<std::byte> expected(sid_bytes);
        std::wstring domain(domain_chars, L'\0');
        if (!LookupAccountNameW(
                nullptr, account, expected.data(), &sid_bytes,
                domain.data(), &domain_chars, &use)) {
            return false;
        }
        return EqualSid(sid, expected.data()) != FALSE;
    } catch (...) {
        return false;
    }
}

bool trusted_security_principal(PSID sid) noexcept {
    if (sid == nullptr || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid)
        || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
        return true;
    }
    return sid_matches_account(sid, L"NT SERVICE\\TrustedInstaller");
}

enum class AclWriteScope : std::uint8_t {
    object,
    ancestor,
};

bool protected_from_untrusted_write(
    const std::wstring& path,
    const AclWriteScope scope) {
    PACL dacl = nullptr;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (status != ERROR_SUCCESS || descriptor == nullptr || dacl == nullptr
        || !trusted_security_principal(owner)) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    constexpr DWORD kObjectWriteMask = GENERIC_ALL | GENERIC_WRITE
        | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA
        | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;
    constexpr DWORD kAncestorWriteMask = GENERIC_ALL | GENERIC_WRITE
        | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER;
    const DWORD write_mask = scope == AclWriteScope::object
        ? kObjectWriteMask : kAncestorWriteMask;
    bool safe = true;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* ace_value = nullptr;
        if (!GetAce(dacl, index, &ace_value) || ace_value == nullptr) {
            safe = false;
            break;
        }
        const auto* header = static_cast<ACE_HEADER*>(ace_value);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0) {
            continue;
        }
        if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE
            || header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE
            || header->AceType
                == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            safe = false;
            break;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(ace_value);
        auto* ace_sid = const_cast<DWORD*>(&ace->SidStart);
        if ((ace->Mask & write_mask) != 0
            && !trusted_security_principal(ace_sid)) {
            safe = false;
            break;
        }
    }
    LocalFree(descriptor);
    return safe;
}

struct VerifiedWorkerExecutable final {
    std::wstring path;
    NativeHandle pinned_handle;
};

Result<VerifiedWorkerExecutable> validate_worker_path(
    const WorkerLaunchRequest& request) {
    if (request.executable_path.empty()
        || request.executable_path.rfind(L"\\\\", 0) == 0
        || request.executable_path.rfind(L"\\?\\", 0) == 0) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_path",
            L"Worker path must be a local absolute path");
    }
    const std::filesystem::path requested(request.executable_path);
    if (!requested.is_absolute()
        || requested.filename().wstring() != kWorkerExecutableName) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_path",
            L"Worker filename or path is not the installed product worker");
    }
    const auto service_module = module_path();
    if (service_module.empty()) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_path",
            L"Service module path could not be resolved",
            GetLastError());
    }
    const auto expected = (
        std::filesystem::path(service_module).parent_path()
        / std::wstring(kWorkerExecutableName)).lexically_normal();
    const auto normalized = requested.lexically_normal();
    if (!equal_path(normalized.wstring(), expected.wstring())
        || has_reparse_component(normalized)) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_path",
            L"Worker path escaped the non-reparse installation directory");
    }
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_path",
            L"Worker executable is absent or unsafe",
            GetLastError());
    }
    NativeHandle executable_handle(CreateFileW(
        normalized.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL
            | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!executable_handle) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_open",
            L"Worker executable could not be pinned for path verification",
            GetLastError());
    }
    const DWORD final_length = GetFinalPathNameByHandleW(
        executable_handle.get(), nullptr, 0, FILE_NAME_NORMALIZED);
    if (final_length == 0 || final_length > 32768) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_final_path",
            L"Worker final path could not be bounded",
            GetLastError());
    }
    std::wstring final_path(final_length, L'\0');
    const DWORD final_written = GetFinalPathNameByHandleW(
        executable_handle.get(), final_path.data(), final_length,
        FILE_NAME_NORMALIZED);
    if (final_written == 0 || final_written >= final_length) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_final_path",
            L"Worker final path could not be read exactly",
            GetLastError());
    }
    final_path.resize(final_written);
    constexpr std::wstring_view extended_prefix = L"\\\\?\\";
    if (final_path.rfind(extended_prefix, 0) == 0) {
        final_path.erase(0, extended_prefix.size());
    }
    if (!equal_path(final_path, normalized.wstring())) {
        return service_error<VerifiedWorkerExecutable>(
            WindowsServiceError::unsafe_worker_path,
            L"windows_service.worker_final_path",
            L"Worker final path differs from the installed executable");
    }
    bool require_protected_path = true;
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    require_protected_path = !request.allow_unprotected_engineering_path;
#endif
    if (require_protected_path) {
        bool protected_path = protected_from_untrusted_write(
            normalized.wstring(), AclWriteScope::object);
        auto current = normalized.parent_path();
        bool installation_directory = true;
        while (protected_path && !current.empty()) {
            protected_path = protected_from_untrusted_write(
                current.wstring(), installation_directory
                    ? AclWriteScope::object : AclWriteScope::ancestor);
            installation_directory = false;
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        if (!protected_path) {
            return service_error<VerifiedWorkerExecutable>(
                WindowsServiceError::unsafe_worker_path,
                L"windows_service.worker_acl",
                L"Worker executable path grants mutation rights to an "
                L"untrusted principal");
        }
    }
    return Result<VerifiedWorkerExecutable>::success({
        normalized.wstring(), std::move(executable_handle)});
}

std::wstring worker_command_line(
    const std::wstring& executable,
    const product::Uuid& profile_id
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    ,
    const bool engineering_once,
    const bool engineering_pipe_probe) {
#else
    ) {
#endif
    const auto profile = profile_id.to_string();
    std::wstring profile_text(profile.begin(), profile.end());
    std::wstring line = L"\"" + executable + L"\" --profile "
        + profile_text;
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    if (engineering_once) line += L" --engineering-once";
    if (engineering_pipe_probe) line += L" --engineering-pipe-probe";
#endif
    return line;
}

class ProcessAttributeList final {
public:
    Result<void> initialize(std::span<HANDLE> handles) {
        SIZE_T required = 0;
        static_cast<void>(InitializeProcThreadAttributeList(
            nullptr, 2, 0, &required));
        if (required == 0) {
            return service_error<void>(
                WindowsServiceError::worker_launch_failed,
                L"windows_service.attributes",
                L"Windows did not report an attribute-list size",
                GetLastError());
        }
        storage_.resize(required);
        list_ = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 2, 0, &required)) {
            list_ = nullptr;
            return service_error<void>(
                WindowsServiceError::worker_launch_failed,
                L"windows_service.attributes",
                L"Failed to initialize worker process attributes",
                GetLastError());
        }
        if (!UpdateProcThreadAttribute(
                list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handles.data(), handles.size_bytes(), nullptr, nullptr)) {
            return service_error<void>(
                WindowsServiceError::worker_launch_failed,
                L"windows_service.handle_allowlist",
                L"Failed to install worker inherited-handle allowlist",
                GetLastError());
        }
        mitigation_policy_ =
            PROCESS_CREATION_MITIGATION_POLICY_DEP_ENABLE
            | PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
        if (!UpdateProcThreadAttribute(
                list_, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                &mitigation_policy_, sizeof(mitigation_policy_),
                nullptr, nullptr)) {
            return service_error<void>(
                WindowsServiceError::worker_launch_failed,
                L"windows_service.mitigation",
                L"Failed to install worker process mitigations",
                GetLastError());
        }
        return Result<void>::success();
    }

    ~ProcessAttributeList() noexcept {
        if (list_ != nullptr) DeleteProcThreadAttributeList(list_);
    }
    PPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    std::vector<std::byte> storage_;
    PPROC_THREAD_ATTRIBUTE_LIST list_{};
    std::uint64_t mitigation_policy_{};
};

NativeHandle open_inheritable_null() {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    return NativeHandle(CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
thread_local bool capture_engineering_worker_stdio = false;
#endif

class WindowsProcessIdentityPin final : public IWorkerProcessIdentityPin {
public:
    WindowsProcessIdentityPin(
        NativeHandle process,
        const std::uint32_t process_id,
        const std::uint64_t creation_time) noexcept
        : process_(std::move(process)), process_id_(process_id),
          creation_time_(creation_time) {}

    std::uint32_t process_id() const noexcept override { return process_id_; }
    std::uint64_t process_creation_time() const noexcept override {
        return creation_time_;
    }
    bool is_running() const noexcept override {
        return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
    }
    Result<bool> wait_for_exit(const std::uint32_t timeout_ms) noexcept override {
        if (!process_) return Result<bool>::success(false);
        const DWORD waited = WaitForSingleObject(process_.get(), timeout_ms);
        if (waited == WAIT_OBJECT_0) return Result<bool>::success(true);
        if (waited == WAIT_TIMEOUT) return Result<bool>::success(false);
        return service_error<bool>(
            WindowsServiceError::worker_wait_failed,
            L"windows_service.pinned_worker_wait",
            L"The pinned worker process wait failed",
            GetLastError());
    }

private:
    NativeHandle process_;
    std::uint32_t process_id_{};
    std::uint64_t creation_time_{};
};

class NamedPipeWorkerTransport final
    : public host::IWorkerReattachmentTransport {
public:
    static Result<std::shared_ptr<NamedPipeWorkerTransport>> connect(
        const product::Uuid& profile_id,
        const std::uint32_t connect_timeout_ms,
        const std::uint32_t operation_timeout_ms) {
        if (connect_timeout_ms == 0 || operation_timeout_ms == 0) {
            return service_error<std::shared_ptr<NamedPipeWorkerTransport>>(
                WindowsServiceError::worker_startup_failed,
                L"windows_service.worker_pipe_timeout",
                L"Worker pipe connect and operation deadlines must be nonzero");
        }
        auto name = host::private_pipe_name(profile_id);
        if (!name.ok()) {
            return Result<std::shared_ptr<NamedPipeWorkerTransport>>::failure(
                name.error());
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(connect_timeout_ms);
        Result<std::unique_ptr<ipc::PipeTransport>> transport =
            Result<std::unique_ptr<ipc::PipeTransport>>::failure(Error{
                ErrorDomain::internal,
                static_cast<std::uint32_t>(
                    OperationErrorCode::internal_invariant),
                L"windows_service.worker_pipe_connect",
                L"Worker pipe connection was not attempted"});
        for (;;) {
            const OperationContext context{{}, deadline};
            transport = ipc::PipeTransport::connect(context, name.value());
            if (transport.ok()) break;
            const bool slice_expired = transport.error().code
                    == static_cast<std::uint32_t>(
                        OperationErrorCode::timed_out)
                && transport.error().operation == L"check_operation_context";
            if (!slice_expired
                || std::chrono::steady_clock::now() >= deadline) {
                return Result<
                    std::shared_ptr<NamedPipeWorkerTransport>>::failure(
                        transport.error());
            }
        }
        return Result<std::shared_ptr<NamedPipeWorkerTransport>>::success(
            std::shared_ptr<NamedPipeWorkerTransport>(
                new NamedPipeWorkerTransport(
                    transport.move_value(), operation_timeout_ms)));
    }

    Result<host::WorkerChallenge> begin_attachment(
        const host::HandshakeNonce& manager_instance_nonce,
        const host::ClientHello& hello) override {
        manager_nonce_ = manager_instance_nonce;
        auto payload = host::encode_client_hello(hello);
        if (!payload.ok()) {
            return Result<host::WorkerChallenge>::failure(payload.error());
        }
        auto response = transact(
            hello.binding,
            host::kPrivateWireClientHello,
            host::kPrivateWireWorkerChallenge,
            payload.move_value());
        if (!response.ok()) {
            return Result<host::WorkerChallenge>::failure(response.error());
        }
        return host::decode_worker_challenge(response.value().payload);
    }

    Result<void> authenticate_attachment(
        const host::HandshakeNonce& manager_instance_nonce,
        const host::ServiceProof& proof) override {
        if (!manager_nonce_.has_value()
            || manager_nonce_.value() != manager_instance_nonce) {
            return service_error<void>(
                WindowsServiceError::worker_startup_failed,
                L"windows_service.worker_pipe_auth",
                L"Worker pipe manager nonce changed during authentication");
        }
        auto payload = host::encode_service_proof(proof);
        if (!payload.ok()) return Result<void>::failure(payload.error());
        auto response = transact(
            proof.binding,
            host::kPrivateWireServiceProof,
            host::kPrivateWireAuthenticated,
            payload.move_value());
        if (!response.ok()) return Result<void>::failure(response.error());
        if (!response.value().payload.empty()) {
            return service_error<void>(
                WindowsServiceError::worker_startup_failed,
                L"windows_service.worker_pipe_auth",
                L"Worker authentication acknowledgement was malformed");
        }
        return Result<void>::success();
    }

    Result<host::AuthenticatedHostFrame> exchange(
        const host::HandshakeNonce& manager_instance_nonce,
        const host::AuthenticatedHostFrame& request) override {
        if (!manager_nonce_.has_value()
            || manager_nonce_.value() != manager_instance_nonce) {
            return service_error<host::AuthenticatedHostFrame>(
                WindowsServiceError::worker_startup_failed,
                L"windows_service.worker_pipe_exchange",
                L"Worker pipe manager nonce does not own this connection");
        }
        auto payload = host::encode_authenticated_host_frame(request);
        if (!payload.ok()) {
            return Result<host::AuthenticatedHostFrame>::failure(
                payload.error());
        }
        auto response = transact(
            request.binding,
            host::kPrivateWireExchangeRequest,
            host::kPrivateWireExchangeResponse,
            payload.move_value());
        if (!response.ok()) {
            return Result<host::AuthenticatedHostFrame>::failure(
                response.error());
        }
        return host::decode_authenticated_host_frame(response.value().payload);
    }

    void disconnect_attachment(
        const host::HandshakeNonce& manager_instance_nonce) noexcept override {
        if (manager_nonce_.has_value()
            && manager_nonce_.value() == manager_instance_nonce) {
            transport_->close();
            manager_nonce_.reset();
        }
    }

private:
    NamedPipeWorkerTransport(
        std::unique_ptr<ipc::PipeTransport> transport,
        const std::uint32_t operation_timeout_ms) noexcept
        : transport_(std::move(transport)),
          operation_timeout_ms_(operation_timeout_ms) {}

    Result<ipc::Frame> transact(
        const host::WorkerBinding& binding,
        const std::uint32_t command,
        const std::uint32_t expected_response,
        std::vector<std::byte> payload) {
        auto request_id = product::Uuid::random();
        if (!request_id.ok()) {
            return Result<ipc::Frame>::failure(request_id.error());
        }
        ipc::Frame request{
            ipc::kProtocolMajor,
            ipc::kProtocolMinor,
            ipc::MessageType::request,
            0,
            request_id.value(),
            binding.profile_id,
            command,
            std::move(payload)};
        OperationContext context{
            {}, std::chrono::steady_clock::now()
                + std::chrono::milliseconds(operation_timeout_ms_)};
        auto written = transport_->write_frame(context, request);
        if (!written.ok()) {
            return Result<ipc::Frame>::failure(written.error());
        }
        auto response = transport_->read_frame(context);
        if (!response.ok()) return response;
        if (response.value().message_type != ipc::MessageType::response
            || response.value().request_id != request.request_id
            || response.value().operation_id != request.operation_id
            || response.value().command_id != expected_response) {
            return service_error<ipc::Frame>(
                WindowsServiceError::worker_startup_failed,
                L"windows_service.worker_pipe_correlation",
                L"Worker pipe response correlation is invalid");
        }
        return response;
    }

    std::unique_ptr<ipc::PipeTransport> transport_;
    std::uint32_t operation_timeout_ms_{};
    std::optional<host::HandshakeNonce> manager_nonce_;
};

} // namespace

ScmContract scm_contract() {
    return {
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        false,
        true,
        kScmFailureResetSeconds,
        {5'000U, 15'000U, 60'000U},
        kPreshutdownTimeoutMs};
}

Result<StartupReport> run_service_startup(IServiceStartupActions& actions) {
    StartupReport report;
    const auto run_global = [&report](
        const StartupStage stage, Result<void> outcome)
        -> Result<void> {
        if (!outcome.ok()) return outcome;
        report.completed_stages.push_back(stage);
        return Result<void>::success();
    };
    const auto require_global = [&run_global](
        const StartupStage stage,
        Result<void> outcome) -> Result<void> {
        return run_global(stage, std::move(outcome));
    };
    auto outcome = require_global(
        StartupStage::logging, actions.initialize_logging());
    if (!outcome.ok()) {
        return Result<StartupReport>::failure(outcome.error());
    }
    outcome = require_global(
        StartupStage::singleton, actions.acquire_singleton());
    if (!outcome.ok()) {
        return Result<StartupReport>::failure(outcome.error());
    }
    outcome = require_global(
        StartupStage::worker_identity, actions.verify_worker_identity());
    if (!outcome.ok()) {
        return Result<StartupReport>::failure(outcome.error());
    }
    outcome = require_global(
        StartupStage::driver, actions.verify_driver());
        if (!outcome.ok()) {
            return Result<StartupReport>::failure(outcome.error());
        }
    auto profiles = actions.discover_profiles();
    if (!profiles.ok()) {
        return Result<StartupReport>::failure(profiles.error());
    }
    report.completed_stages.push_back(StartupStage::profiles);

    std::vector<StartupProfile> healthy;
    for (const auto& profile : profiles.value()) {
        auto validated = actions.validate_profile(profile.profile_id);
        if (!validated.ok()) {
            report.profile_failures.push_back({
                profile.profile_id, StartupStage::profiles,
                validated.error()});
            continue;
        }
        healthy.push_back(profile);
    }
    for (const auto& profile : healthy) {
        constexpr std::size_t kReattachAttemptLimit = 3;
        std::optional<Error> last_error;
        for (std::size_t attempt = 0;
             attempt < kReattachAttemptLimit; ++attempt) {
            auto attached = actions.reattach_profile(profile.profile_id);
            if (!attached.ok()) {
                last_error = attached.error();
                continue;
            }
            last_error.reset();
            if (attached.value()) {
                report.attached_profiles.push_back(profile.profile_id);
            }
            break;
        }
        if (last_error.has_value()) {
            report.profile_failures.push_back({
                profile.profile_id, StartupStage::reattach,
                std::move(*last_error)});
            continue;
        }
    }
    report.completed_stages.push_back(StartupStage::reattach);

    for (const auto& profile : healthy) {
        const bool failed = std::any_of(
            report.profile_failures.begin(), report.profile_failures.end(),
            [&profile](const StartupProfileFailure& value) {
                return value.profile_id == profile.profile_id;
            });
        const bool attached = std::find(
            report.attached_profiles.begin(), report.attached_profiles.end(),
            profile.profile_id) != report.attached_profiles.end();
        if (failed || attached || !profile.auto_start) continue;
        auto started = actions.start_profile(profile.profile_id);
        if (!started.ok()) {
            report.profile_failures.push_back({
                profile.profile_id, StartupStage::auto_start,
                started.error()});
            continue;
        }
        report.started_profiles.push_back(profile.profile_id);
    }
    report.completed_stages.push_back(StartupStage::auto_start);
    report.globally_ready = true;
    return Result<StartupReport>::success(std::move(report));
}

Result<BestEffortCheckpointResult> run_best_effort_checkpoints(
    IServiceShutdownActions& actions,
    const bool enabled,
    const std::uint64_t monotonic_deadline_ms) {
    BestEffortCheckpointResult report;
    if (!enabled) {
        return Result<BestEffortCheckpointResult>::success(std::move(report));
    }
    auto profiles = actions.running_profiles();
    if (!profiles.ok()) {
        return Result<BestEffortCheckpointResult>::failure(profiles.error());
    }
    for (std::size_t index = 0; index < profiles.value().size(); ++index) {
        const auto& profile = profiles.value()[index];
        if (actions.monotonic_time_ms() >= monotonic_deadline_ms) {
            report.deadline_exhausted = true;
            report.unresolved_profiles.insert(
                report.unresolved_profiles.end(),
                profiles.value().begin() + static_cast<std::ptrdiff_t>(index),
                profiles.value().end());
            break;
        }
        report.attempted_profiles.push_back(profile);
        auto outcome = actions.checkpoint_best_effort(
            profile, monotonic_deadline_ms);
        if (actions.monotonic_time_ms() >= monotonic_deadline_ms) {
            report.deadline_exhausted = true;
            report.unresolved_profiles.push_back(profile);
        } else if (outcome.ok() && outcome.value()) {
            report.confirmed_profiles.push_back(profile);
        } else {
            report.unresolved_profiles.push_back(profile);
        }
    }
    return Result<BestEffortCheckpointResult>::success(std::move(report));
}

Result<ControlledStopResult> run_controlled_stop(
    IServiceShutdownActions& actions,
    const std::uint64_t monotonic_deadline_ms) {
    ControlledStopResult report;
    auto profiles = actions.running_profiles();
    if (!profiles.ok()) {
        return Result<ControlledStopResult>::failure(profiles.error());
    }
    for (const auto& profile : profiles.value()) {
        report.attempted_profiles.push_back(profile);
        if (actions.monotonic_time_ms() >= monotonic_deadline_ms) {
            report.unsafe_profiles.push_back(profile);
            continue;
        }
        auto outcome = actions.stop_safely(profile, monotonic_deadline_ms);
        if (outcome.ok() && outcome.value()) {
            report.safely_stopped_profiles.push_back(profile);
        } else {
            report.unsafe_profiles.push_back(profile);
        }
    }
    report.resulting_state = report.unsafe_profiles.empty()
        ? ServiceSteadyState::stopped : ServiceSteadyState::running;
    return Result<ControlledStopResult>::success(std::move(report));
}

struct WorkerProcess::Impl final {
    Impl(
        NativeHandle process_value,
        NativeHandle job_value,
        host::WorkerBinding binding_value,
        host::SessionSecret startup_secret_value) noexcept
        : process(std::move(process_value)), job(std::move(job_value)),
          binding(std::move(binding_value)),
          startup_secret(std::move(startup_secret_value)) {}

    ~Impl() noexcept {
        if (!startup_authenticated && process
            && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
            static_cast<void>(TerminateProcess(process.get(), 0xE524U));
            static_cast<void>(WaitForSingleObject(process.get(), 5'000));
        }
    }

    NativeHandle process;
    NativeHandle job;
    host::WorkerBinding binding;
    std::optional<host::SessionSecret> startup_secret;
    bool startup_authenticated{};
};

WorkerProcess::WorkerProcess(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
WorkerProcess::WorkerProcess(WorkerProcess&&) noexcept = default;
WorkerProcess& WorkerProcess::operator=(WorkerProcess&&) noexcept = default;
WorkerProcess::~WorkerProcess() noexcept = default;
const host::WorkerBinding& WorkerProcess::binding() const noexcept {
    return impl_->binding;
}
bool WorkerProcess::is_running() const noexcept {
    return impl_ != nullptr && impl_->process
        && WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT;
}
Result<WorkerStartupAttachment> WorkerProcess::attach_startup(
    const std::uint32_t timeout_ms) {
    if (impl_ == nullptr || !impl_->startup_secret.has_value()
        || timeout_ms == 0) {
        return service_error<WorkerStartupAttachment>(
            WindowsServiceError::worker_startup_failed,
            L"windows_service.worker_startup_auth",
            L"Worker startup credential is absent or already consumed");
    }
    auto transport = NamedPipeWorkerTransport::connect(
        impl_->binding.profile_id, timeout_ms, timeout_ms);
    if (!transport.ok()) {
        return Result<WorkerStartupAttachment>::failure(
            transport.error());
    }
    auto nonce = host::generate_handshake_nonce();
    if (!nonce.ok()) {
        return Result<WorkerStartupAttachment>::failure(
            nonce.error());
    }
    auto startup_secret = std::move(impl_->startup_secret.value());
    impl_->startup_secret.reset();
    auto handshake = host::ServiceHandshake::begin(
        impl_->binding, std::move(startup_secret), nonce.value());
    if (!handshake.ok()) {
        return Result<WorkerStartupAttachment>::failure(
            handshake.error());
    }
    auto challenge = transport.value()->begin_attachment(
        nonce.value(), handshake.value().hello());
    if (!challenge.ok()) {
        return Result<WorkerStartupAttachment>::failure(
            challenge.error());
    }
    auto accepted = handshake.value().accept_challenge(challenge.value());
    if (!accepted.ok()) {
        return Result<WorkerStartupAttachment>::failure(
            accepted.error());
    }
    auto authenticated = transport.value()->authenticate_attachment(
        nonce.value(), accepted.value().proof);
    if (!authenticated.ok()) {
        transport.value()->disconnect_attachment(nonce.value());
        return Result<WorkerStartupAttachment>::failure(
            authenticated.error());
    }
    host::ServiceReattachmentResult connection(
        impl_->binding,
        nonce.value(),
        std::move(accepted.value().session),
        host::ServiceReattachmentDisposition::recovered_current,
        transport.value());
    auto response = connection.exchange(host::HostMessage{
        impl_->binding.profile_id,
        impl_->binding.image_id,
        host::kWorkerRuntimeSnapshotQuery,
        0,
        {}});
    if (!response.ok()) {
        return Result<WorkerStartupAttachment>::failure(
            response.error());
    }
    if (response.value().command_id
            != host::kWorkerRuntimeSnapshotResponse
        || response.value().request_id != impl_->binding.profile_id
        || response.value().operation_id != impl_->binding.image_id) {
        return service_error<WorkerStartupAttachment>(
            WindowsServiceError::worker_startup_failed,
            L"windows_service.worker_startup_status",
            L"Authenticated worker startup response was not correlated");
    }
    auto status = host::decode_worker_runtime_status(
        response.value().payload, impl_->binding);
    if (!status.ok()) {
        return Result<WorkerStartupAttachment>::failure(
            status.error());
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(
            impl_->job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        return service_error<WorkerStartupAttachment>(
            WindowsServiceError::worker_startup_failed,
            L"windows_service.worker_survival_job",
            L"The authenticated worker could not be detached from its "
            L"startup-only kill-on-close job policy",
            GetLastError());
    }
    impl_->startup_authenticated = true;
    return Result<WorkerStartupAttachment>::success(
        WorkerStartupAttachment{
            std::move(connection), status.move_value()});
}

Result<void> WorkerProcess::authenticate_startup(
    const std::uint32_t timeout_ms) {
    auto connection = attach_startup(timeout_ms);
    if (!connection.ok()) return Result<void>::failure(connection.error());
    return Result<void>::success();
}
Result<std::uint32_t> WorkerProcess::wait(
    const std::uint32_t timeout_ms) noexcept {
    if (impl_ == nullptr || !impl_->process) {
        return service_error<std::uint32_t>(
            WindowsServiceError::worker_wait_failed,
            L"windows_service.worker_wait",
            L"Worker process capability is absent");
    }
    const DWORD waited = WaitForSingleObject(impl_->process.get(), timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        return service_error<std::uint32_t>(
            WindowsServiceError::worker_wait_failed,
            L"windows_service.worker_wait",
            waited == WAIT_TIMEOUT ? L"Worker wait timed out"
                                   : L"Worker wait failed",
            waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(impl_->process.get(), &exit_code)) {
        return service_error<std::uint32_t>(
            WindowsServiceError::worker_wait_failed,
            L"windows_service.worker_wait",
            L"Worker exit code could not be queried",
            GetLastError());
    }
    return Result<std::uint32_t>::success(exit_code);
}

Result<WorkerProcess> launch_worker(const WorkerLaunchRequest& request) {
    if (is_zero_uuid(request.profile_id) || is_zero_uuid(request.image_id)
        || request.session_generation == 0 || request.profile_revision == 0) {
        return service_error<WorkerProcess>(
            WindowsServiceError::invalid_argument,
            L"windows_service.worker_launch",
            L"Worker launch binding is invalid");
    }
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    if (request.engineering_exit_after_startup
        && request.engineering_pipe_probe) {
        return service_error<WorkerProcess>(
            WindowsServiceError::invalid_argument,
            L"windows_service.worker_launch",
            L"Worker engineering modes are mutually exclusive");
    }
#endif
    auto executable = validate_worker_path(request);
    if (!executable.ok()) {
        return Result<WorkerProcess>::failure(executable.error());
    }
    auto transfer = host::prepare_staged_startup_secret_transfer();
    if (!transfer.ok()) {
        return Result<WorkerProcess>::failure(transfer.error());
    }
    bool capture_stdio = false;
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    capture_stdio = capture_engineering_worker_stdio;
#endif
    auto stdout_null = capture_stdio ? NativeHandle{} : open_inheritable_null();
    auto stderr_null = capture_stdio ? NativeHandle{} : open_inheritable_null();
    const HANDLE worker_stdout = capture_stdio
        ? GetStdHandle(STD_OUTPUT_HANDLE) : stdout_null.get();
    const HANDLE worker_stderr = capture_stdio
        ? GetStdHandle(STD_ERROR_HANDLE) : stderr_null.get();
    if (worker_stdout == nullptr || worker_stdout == INVALID_HANDLE_VALUE
        || worker_stderr == nullptr || worker_stderr == INVALID_HANDLE_VALUE) {
        return service_error<WorkerProcess>(
            WindowsServiceError::worker_launch_failed,
            L"windows_service.worker_stdio",
            L"Failed to create controlled worker standard handles",
            GetLastError());
    }
    std::array<HANDLE, 3> inherited{
        static_cast<HANDLE>(transfer.value().inherited_read_handle()),
        worker_stdout,
        worker_stderr};
    ProcessAttributeList attributes;
    auto initialized = attributes.initialize(inherited);
    if (!initialized.ok()) {
        return Result<WorkerProcess>::failure(initialized.error());
    }
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    auto line = worker_command_line(
        executable.value().path, request.profile_id,
        request.engineering_exit_after_startup,
        request.engineering_pipe_probe);
#else
    auto line = worker_command_line(
        executable.value().path, request.profile_id);
#endif
    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited[0];
    startup.StartupInfo.hStdOutput = inherited[1];
    startup.StartupInfo.hStdError = inherited[2];
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION raw{};
    constexpr DWORD kFlags = CREATE_SUSPENDED | CREATE_NO_WINDOW
        | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    const auto working_directory =
        std::filesystem::path(executable.value().path).parent_path().wstring();
    const BOOL created = CreateProcessW(
        executable.value().path.c_str(), mutable_line.data(),
        nullptr, nullptr, TRUE, kFlags, nullptr,
        working_directory.c_str(), &startup.StartupInfo, &raw);
    if (!created) {
        return service_error<WorkerProcess>(
            WindowsServiceError::worker_launch_failed,
            L"windows_service.CreateProcessW",
            L"Failed to create suspended worker process",
            GetLastError());
    }
    NativeHandle process(raw.hProcess);
    NativeHandle thread(raw.hThread);
    transfer.value().close_parent_read();
    const auto fail_suspended = [&process](Error original)
        -> Result<WorkerProcess> {
        if (!process) return Result<WorkerProcess>::failure(std::move(original));
        if (!TerminateProcess(process.get(), 0xE523U)) {
            return service_error<WorkerProcess>(
                WindowsServiceError::worker_launch_failed,
                L"windows_service.abort_suspended",
                L"Failed to terminate a rejected suspended worker",
                GetLastError());
        }
        const DWORD waited = WaitForSingleObject(process.get(), 5'000);
        if (waited != WAIT_OBJECT_0) {
            return service_error<WorkerProcess>(
                WindowsServiceError::worker_wait_failed,
                L"windows_service.abort_suspended_wait",
                L"A rejected suspended worker did not confirm exit",
                waited == WAIT_FAILED ? GetLastError() : WAIT_TIMEOUT);
        }
        return Result<WorkerProcess>::failure(std::move(original));
    };

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(
            process.get(), &creation, &exit, &kernel, &user)) {
        const auto error = GetLastError();
        return fail_suspended(service_error<WorkerProcess>(
            WindowsServiceError::worker_identity_failed,
            L"windows_service.GetProcessTimes",
            L"Failed to bind the suspended worker identity",
            error).error());
    }
    host::WorkerBinding binding{
        request.profile_id,
        request.image_id,
        raw.dwProcessId,
        file_time_value(creation),
        request.session_generation,
        request.profile_revision,
        product::kProductVersionMajor,
        product::kProductVersionMinor,
        product::kProductVersionPatch};
    auto secret = host::SessionSecret::generate();
    if (!secret.ok()) {
        return fail_suspended(secret.error());
    }
    auto published = transfer.value().publish(binding, secret.value());
    if (!published.ok()) {
        return fail_suspended(published.error());
    }

    NativeHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        const auto error = GetLastError();
        return fail_suspended(service_error<WorkerProcess>(
            WindowsServiceError::worker_launch_failed,
            L"windows_service.CreateJobObjectW",
            L"Failed to create the non-killing worker supervision job",
            error).error());
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
        | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))
        || !AssignProcessToJobObject(job.get(), process.get())) {
        const auto error = GetLastError();
        return fail_suspended(service_error<WorkerProcess>(
            WindowsServiceError::worker_launch_failed,
            L"windows_service.AssignProcessToJobObject",
            L"Failed to assign the worker to its startup supervision job",
            error).error());
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const auto error = GetLastError();
        return fail_suspended(service_error<WorkerProcess>(
            WindowsServiceError::worker_startup_failed,
            L"windows_service.ResumeThread",
            L"Failed to resume the identity-bound worker",
            error).error());
    }
    thread.reset();
    auto impl = std::make_unique<WorkerProcess::Impl>(
        std::move(process), std::move(job), std::move(binding),
        secret.move_value());
    return Result<WorkerProcess>::success(WorkerProcess(std::move(impl)));
}

Result<std::unique_ptr<IWorkerProcessIdentityPin>>
WindowsProcessIdentityQuery::pin_process(const std::uint32_t process_id) {
    if (process_id == 0) {
        return service_error<std::unique_ptr<IWorkerProcessIdentityPin>>(
            WindowsServiceError::invalid_argument,
            L"windows_service.pin_process",
            L"Process ID must be nonzero");
    }
    NativeHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, process_id));
    if (!process) {
        return service_error<std::unique_ptr<IWorkerProcessIdentityPin>>(
            WindowsServiceError::worker_identity_failed,
            L"windows_service.OpenProcess",
            L"Worker process could not be pinned",
            GetLastError());
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(
            process.get(), &creation, &exit, &kernel, &user)) {
        return service_error<std::unique_ptr<IWorkerProcessIdentityPin>>(
            WindowsServiceError::worker_identity_failed,
            L"windows_service.GetProcessTimes",
            L"Pinned worker creation time could not be queried",
            GetLastError());
    }
    std::unique_ptr<IWorkerProcessIdentityPin> pin =
        std::make_unique<WindowsProcessIdentityPin>(
            std::move(process), process_id, file_time_value(creation));
    return Result<std::unique_ptr<IWorkerProcessIdentityPin>>::success(
        std::move(pin));
}

namespace {

std::optional<std::wstring_view> option_value(
    const int argc,
    wchar_t** argv,
    const std::wstring_view name) noexcept {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wstring_view(argv[index]) == name) {
            return std::wstring_view(argv[index + 1]);
        }
    }
    return std::nullopt;
}

bool has_option(
    const int argc,
    wchar_t** argv,
    const std::wstring_view name) noexcept {
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == name) return true;
    }
    return false;
}

product::Uuid fixed_uuid(const char* text) {
    auto value = product::Uuid::parse(text);
    if (!value.ok()) std::terminate();
    return value.move_value();
}

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
Result<product::Uuid> parse_wide_uuid(const std::wstring_view value) {
    std::string text;
    text.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            return service_error<product::Uuid>(
                WindowsServiceError::invalid_argument,
                L"windows_service.uuid",
                L"UUID contains non-ASCII text");
        }
        text.push_back(static_cast<char>(character));
    }
    return product::Uuid::parse(text);
}

Error model_failure(const wchar_t* operation) {
    return {
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(WindowsServiceError::startup_failed),
        operation,
        L"injected deterministic service lifecycle failure"};
}

std::wstring stage_name(const StartupStage stage) {
    switch (stage) {
    case StartupStage::logging: return L"logging";
    case StartupStage::singleton: return L"singleton";
    case StartupStage::worker_identity: return L"worker_identity";
    case StartupStage::driver: return L"driver";
    case StartupStage::profiles: return L"profiles";
    case StartupStage::reattach: return L"reattach";
    case StartupStage::auto_start: return L"auto_start";
    }
    return L"unknown";
}

void write_uuid_array(const std::vector<product::Uuid>& values) {
    std::wcout << L"[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) std::wcout << L",";
        const auto text = values[index].to_string();
        std::wcout << L"\"" << std::wstring(text.begin(), text.end())
                   << L"\"";
    }
    std::wcout << L"]";
}

class StartupModel final : public IServiceStartupActions {
public:
    StartupModel()
        : first_(fixed_uuid("11111111-1111-4111-8111-111111111111")),
          bad_(fixed_uuid("22222222-2222-4222-8222-222222222222")),
          third_(fixed_uuid("33333333-3333-4333-8333-333333333333")),
          stale_(fixed_uuid("44444444-4444-4444-8444-444444444444")) {}

    Result<void> initialize_logging() override {
        return Result<void>::success();
    }
    Result<void> acquire_singleton() override {
        return Result<void>::success();
    }
    Result<void> verify_worker_identity() override {
        return Result<void>::success();
    }
    Result<void> verify_driver() override {
        return Result<void>::success();
    }
    Result<std::vector<StartupProfile>> discover_profiles() override {
        return Result<std::vector<StartupProfile>>::success({
            {first_, true}, {bad_, true}, {third_, true}, {stale_, true}});
    }
    Result<void> validate_profile(
        const product::Uuid& profile_id) override {
        if (profile_id == bad_) {
            return Result<void>::failure(
                model_failure(L"startup.validate_profile"));
        }
        return Result<void>::success();
    }
    Result<bool> reattach_profile(const product::Uuid& profile_id) override {
        if (profile_id == first_) {
            ++transient_reattach_attempts_;
            if (transient_reattach_attempts_ < 3) {
                return Result<bool>::failure(
                    model_failure(L"startup.transient_reattach"));
            }
            return Result<bool>::success(true);
        }
        if (profile_id == stale_) {
            ++permanent_reattach_attempts_;
            return Result<bool>::failure(
                model_failure(L"startup.stale_runtime_lease"));
        }
        return Result<bool>::success(false);
    }
    Result<void> start_profile(const product::Uuid&) override {
        return Result<void>::success();
    }

    std::size_t transient_reattach_attempts() const noexcept {
        return transient_reattach_attempts_;
    }
    std::size_t permanent_reattach_attempts() const noexcept {
        return permanent_reattach_attempts_;
    }

private:
    product::Uuid first_;
    product::Uuid bad_;
    product::Uuid third_;
    product::Uuid stale_;
    std::size_t transient_reattach_attempts_{};
    std::size_t permanent_reattach_attempts_{};
};

class StartupFailureModel final : public IServiceStartupActions {
public:
    Result<void> initialize_logging() override {
        calls.push_back(L"logging");
        return Result<void>::success();
    }
    Result<void> acquire_singleton() override {
        calls.push_back(L"singleton");
        return Result<void>::failure(
            model_failure(L"startup.singleton"));
    }
    Result<void> verify_worker_identity() override {
        calls.push_back(L"worker_identity");
        return Result<void>::success();
    }
    Result<void> verify_driver() override {
        calls.push_back(L"driver");
        return Result<void>::success();
    }
    Result<std::vector<StartupProfile>> discover_profiles() override {
        calls.push_back(L"profiles");
        return Result<std::vector<StartupProfile>>::success({});
    }
    Result<void> validate_profile(const product::Uuid&) override {
        return Result<void>::success();
    }
    Result<bool> reattach_profile(const product::Uuid&) override {
        return Result<bool>::success(false);
    }
    Result<void> start_profile(const product::Uuid&) override {
        return Result<void>::success();
    }

    std::vector<std::wstring> calls;
};

class ShutdownModel final : public IServiceShutdownActions {
public:
    explicit ShutdownModel(
        const bool three_profiles,
        const bool late_checkpoint = false)
        : profiles_{
              fixed_uuid("11111111-1111-4111-8111-111111111111"),
              fixed_uuid("22222222-2222-4222-8222-222222222222")},
          late_checkpoint_(late_checkpoint) {
        if (three_profiles) {
            profiles_.push_back(
                fixed_uuid("33333333-3333-4333-8333-333333333333"));
        }
    }

    std::uint64_t monotonic_time_ms() const noexcept override { return now_; }
    Result<std::vector<product::Uuid>> running_profiles() override {
        return Result<std::vector<product::Uuid>>::success(profiles_);
    }
    Result<bool> checkpoint_best_effort(
        const product::Uuid& profile_id, std::uint64_t) override {
        if (late_checkpoint_) now_ = 1'000;
        return Result<bool>::success(profile_id == profiles_.front());
    }
    Result<bool> stop_safely(
        const product::Uuid& profile_id, std::uint64_t) override {
        return Result<bool>::success(profile_id != profiles_[1]);
    }

private:
    std::vector<product::Uuid> profiles_;
    bool late_checkpoint_{};
    std::uint64_t now_{100};
};

int print_contract() {
    const auto contract = scm_contract();
    std::wcout
        << L"{\"service_name\":\"MyRamDiskService\","
        << L"\"account\":\"LocalSystem\","
        << L"\"start_type\":\"automatic\","
        << L"\"delayed_auto_start\":false,"
        << L"\"service_sid\":\"unrestricted\","
        << L"\"kill_on_job_close\":false,"
        << L"\"failure_reset_seconds\":"
        << contract.failure_reset_seconds << L","
        << L"\"restart_delays_ms\":[5000,15000,60000],"
        << L"\"preshutdown_timeout_ms\":"
        << contract.preshutdown_timeout_ms << L","
        << L"\"suspend_timeout_ms\":" << kSuspendTimeoutMs
        << L"}" << std::endl;
    return 0;
}

int print_startup_model() {
    StartupModel actions;
    auto report = run_service_startup(actions);
    if (!report.ok() || !report.value().globally_ready) return 1;
    std::wcout << L"{\"stages\":[";
    for (std::size_t index = 0;
         index < report.value().completed_stages.size(); ++index) {
        if (index != 0) std::wcout << L",";
        std::wcout << L"\""
                   << stage_name(report.value().completed_stages[index])
                   << L"\"";
    }
    std::wcout << L"],\"attached\":";
    write_uuid_array(report.value().attached_profiles);
    std::wcout << L",\"started\":";
    write_uuid_array(report.value().started_profiles);
    std::vector<product::Uuid> failed;
    for (const auto& failure : report.value().profile_failures) {
        failed.push_back(failure.profile_id);
    }
    std::wcout << L",\"failed\":";
    write_uuid_array(failed);
    std::wcout << L",\"transient_reattach_attempts\":"
               << actions.transient_reattach_attempts();
    std::wcout << L",\"permanent_reattach_attempts\":"
               << actions.permanent_reattach_attempts();
    std::wcout << L"}" << std::endl;
    return 0;
}

int print_startup_failure_model() {
    StartupFailureModel actions;
    const auto report = run_service_startup(actions);
    if (report.ok()) return 1;
    std::wcout << L"{\"calls\":[";
    for (std::size_t index = 0; index < actions.calls.size(); ++index) {
        if (index != 0) std::wcout << L",";
        std::wcout << L"\"" << actions.calls[index] << L"\"";
    }
    std::wcout << L"]}" << std::endl;
    return 0;
}

int print_shutdown_model() {
    ShutdownModel disabled_actions(false);
    auto disabled = run_best_effort_checkpoints(
        disabled_actions, false, 1'000);
    ShutdownModel checkpoint_actions(false);
    auto best_effort = run_best_effort_checkpoints(
        checkpoint_actions, true, 1'000);
    ShutdownModel stop_actions(true);
    auto stopped = run_controlled_stop(stop_actions, 1'000);
    ShutdownModel late_actions(false, true);
    auto late = run_best_effort_checkpoints(late_actions, true, 1'000);
    if (!disabled.ok() || !best_effort.ok() || !stopped.ok()
        || !late.ok()) return 1;
    std::wcout
        << L"{\"disabled_attempts\":"
        << disabled.value().attempted_profiles.size()
        << L",\"best_effort_attempts\":"
        << best_effort.value().attempted_profiles.size()
        << L",\"best_effort_confirmed\":"
        << best_effort.value().confirmed_profiles.size()
        << L",\"late_checkpoint_confirmed\":"
        << late.value().confirmed_profiles.size()
        << L",\"controlled_stop_attempts\":"
        << stopped.value().attempted_profiles.size()
        << L",\"controlled_stop_state\":\""
        << (stopped.value().resulting_state == ServiceSteadyState::running
                ? L"running" : L"stopped")
        << L"\"}" << std::endl;
    return 0;
}

bool valid_engineering_service_name(const std::wstring_view name) noexcept {
    constexpr std::wstring_view prefix = L"MyRamDiskTest-";
    if (name.size() <= prefix.size() || name.size() > 128
        || name.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return std::all_of(
        name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
        [](const wchar_t value) {
            return (value >= L'0' && value <= L'9')
                || (value >= L'a' && value <= L'z')
                || (value >= L'A' && value <= L'Z') || value == L'-';
        });
}

class ScHandle final {
public:
    explicit ScHandle(SC_HANDLE value = nullptr) noexcept : value_(value) {}
    ~ScHandle() noexcept {
        if (value_ != nullptr) static_cast<void>(CloseServiceHandle(value_));
    }
    ScHandle(ScHandle&& other) noexcept : value_(other.release()) {}
    ScHandle& operator=(ScHandle&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) CloseServiceHandle(value_);
            value_ = other.release();
        }
        return *this;
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    SC_HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
    SC_HANDLE release() noexcept {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }

private:
    SC_HANDLE value_{};
};

Result<void> configure_service(SC_HANDLE service) {
    SERVICE_DESCRIPTIONW description{
        const_cast<wchar_t*>(L"MyRamDisk per-disk worker manager")};
    SERVICE_DELAYED_AUTO_START_INFO delayed{FALSE};
    SERVICE_SID_INFO sid{SERVICE_SID_TYPE_UNRESTRICTED};
    std::array<SC_ACTION, 3> actions{{
        {SC_ACTION_RESTART, 5'000},
        {SC_ACTION_RESTART, 15'000},
        {SC_ACTION_RESTART, 60'000}}};
    SERVICE_FAILURE_ACTIONSW failures{
        kScmFailureResetSeconds,
        nullptr,
        nullptr,
        static_cast<DWORD>(actions.size()),
        actions.data()};
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
    SERVICE_PRESHUTDOWN_INFO preshutdown{kPreshutdownTimeoutMs};
    const std::array<std::pair<DWORD, void*>, 6> changes{{
        {SERVICE_CONFIG_DESCRIPTION, &description},
        {SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed},
        {SERVICE_CONFIG_SERVICE_SID_INFO, &sid},
        {SERVICE_CONFIG_FAILURE_ACTIONS, &failures},
        {SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag},
        {SERVICE_CONFIG_PRESHUTDOWN_INFO, &preshutdown}}};
    for (const auto& change : changes) {
        if (!ChangeServiceConfig2W(service, change.first, change.second)) {
            return service_error<void>(
                WindowsServiceError::scm_configuration_failed,
                L"windows_service.ChangeServiceConfig2W",
                L"Failed to apply the frozen service configuration",
                GetLastError());
        }
    }
    return Result<void>::success();
}

int install_engineering_service(const std::wstring_view name) {
    if (!valid_engineering_service_name(name)) return 2;
    ScHandle manager(OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (!manager) return 3;
    const auto executable = module_path();
    if (executable.empty()) return 4;
    const std::wstring service_name(name);
    const std::wstring command = L"\"" + executable
        + L"\" --engineering-service --service-name " + service_name;
    ScHandle service(CreateServiceW(
        manager.get(), service_name.c_str(), service_name.c_str(),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!service) return 5;
    auto configured = configure_service(service.get());
    if (!configured.ok()) {
        static_cast<void>(DeleteService(service.get()));
        return 6;
    }
    std::wcout << L"{\"installed\":true}" << std::endl;
    return 0;
}

struct QueriedService final {
    std::wstring account;
    DWORD start_type{};
    bool delayed{};
    DWORD sid_type{};
    DWORD reset_seconds{};
    bool failure_actions_on_non_crash{};
    std::vector<DWORD> delays;
    DWORD preshutdown_timeout_ms{};
    DWORD current_state{};
    DWORD controls_accepted{};
};

Result<QueriedService> query_engineering_service(
    const std::wstring_view name) {
    if (!valid_engineering_service_name(name)) {
        return service_error<QueriedService>(
            WindowsServiceError::invalid_service_name,
            L"windows_service.query",
            L"Engineering service name is invalid");
    }
    ScHandle manager(OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_open_failed,
            L"windows_service.OpenSCManagerW",
            L"SCM could not be opened",
            GetLastError());
    }
    const std::wstring service_name(name);
    ScHandle service(OpenServiceW(
        manager.get(), service_name.c_str(),
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (!service) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_open_failed,
            L"windows_service.OpenServiceW",
            L"Service could not be opened",
            GetLastError());
    }
    DWORD required = 0;
    static_cast<void>(QueryServiceConfigW(
        service.get(), nullptr, 0, &required));
    std::vector<std::byte> config_bytes(required);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(
        config_bytes.data());
    if (required == 0 || !QueryServiceConfigW(
            service.get(), config, required, &required)) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_configuration_failed,
            L"windows_service.QueryServiceConfigW",
            L"Base service configuration could not be queried",
            GetLastError());
    }
    QueriedService result;
    result.account = config->lpServiceStartName == nullptr
        ? L"" : config->lpServiceStartName;
    result.start_type = config->dwStartType;

    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    if (!QueryServiceConfig2W(
            service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
            reinterpret_cast<LPBYTE>(&delayed), sizeof(delayed), &required)) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_configuration_failed,
            L"windows_service.QueryDelayedStart",
            L"Delayed start configuration could not be queried",
            GetLastError());
    }
    result.delayed = delayed.fDelayedAutostart != FALSE;
    SERVICE_SID_INFO sid{};
    if (!QueryServiceConfig2W(
            service.get(), SERVICE_CONFIG_SERVICE_SID_INFO,
            reinterpret_cast<LPBYTE>(&sid), sizeof(sid), &required)) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_configuration_failed,
            L"windows_service.QueryServiceSid",
            L"Service SID configuration could not be queried",
            GetLastError());
    }
    result.sid_type = sid.dwServiceSidType;
    SERVICE_PRESHUTDOWN_INFO preshutdown{};
    if (!QueryServiceConfig2W(
            service.get(), SERVICE_CONFIG_PRESHUTDOWN_INFO,
            reinterpret_cast<LPBYTE>(&preshutdown), sizeof(preshutdown),
            &required)) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_configuration_failed,
            L"windows_service.QueryPreshutdown",
            L"Preshutdown configuration could not be queried",
            GetLastError());
    }
    result.preshutdown_timeout_ms = preshutdown.dwPreshutdownTimeout;
    required = 0;
    static_cast<void>(QueryServiceConfig2W(
        service.get(), SERVICE_CONFIG_FAILURE_ACTIONS,
        nullptr, 0, &required));
    std::vector<std::byte> failure_bytes(required);
    if (required == 0 || !QueryServiceConfig2W(
            service.get(), SERVICE_CONFIG_FAILURE_ACTIONS,
            reinterpret_cast<LPBYTE>(failure_bytes.data()),
            required, &required)) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_configuration_failed,
            L"windows_service.QueryFailureActions",
            L"Failure actions could not be queried",
            GetLastError());
    }
    const auto* failures =
        reinterpret_cast<SERVICE_FAILURE_ACTIONSW*>(failure_bytes.data());
    result.reset_seconds = failures->dwResetPeriod;
    for (DWORD index = 0; index < failures->cActions; ++index) {
        if (failures->lpsaActions[index].Type != SC_ACTION_RESTART) {
            return service_error<QueriedService>(
                WindowsServiceError::scm_configuration_failed,
                L"windows_service.QueryFailureActions",
                L"Unexpected non-restart failure action");
        }
        result.delays.push_back(failures->lpsaActions[index].Delay);
    }
    SERVICE_FAILURE_ACTIONS_FLAG failure_flag{};
    if (!QueryServiceConfig2W(
            service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
            reinterpret_cast<LPBYTE>(&failure_flag), sizeof(failure_flag),
            &required)) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_configuration_failed,
            L"windows_service.QueryFailureActionsFlag",
            L"Failure-actions flag could not be queried",
            GetLastError());
    }
    result.failure_actions_on_non_crash =
        failure_flag.fFailureActionsOnNonCrashFailures != FALSE;
    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatusEx(
            service.get(), SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &required)) {
        return service_error<QueriedService>(
            WindowsServiceError::scm_configuration_failed,
            L"windows_service.QueryServiceStatusEx",
            L"Service runtime status could not be queried",
            GetLastError());
    }
    result.current_state = status.dwCurrentState;
    result.controls_accepted = status.dwControlsAccepted;
    return Result<QueriedService>::success(std::move(result));
}

int print_engineering_service(const std::wstring_view name) {
    auto value = query_engineering_service(name);
    if (!value.ok()) return 3;
    std::wcout
        << L"{\"account\":\"" << value.value().account << L"\","
        << L"\"start_type\":\""
        << (value.value().start_type == SERVICE_AUTO_START
                ? L"automatic" : L"other") << L"\","
        << L"\"delayed_auto_start\":"
        << (value.value().delayed ? L"true" : L"false") << L","
        << L"\"service_sid\":\""
        << (value.value().sid_type == SERVICE_SID_TYPE_UNRESTRICTED
                ? L"unrestricted" : L"other") << L"\","
        << L"\"failure_reset_seconds\":"
        << value.value().reset_seconds
        << L",\"failure_actions_on_non_crash\":"
        << (value.value().failure_actions_on_non_crash
                ? L"true" : L"false")
        << L",\"preshutdown_timeout_ms\":"
        << value.value().preshutdown_timeout_ms
        << L",\"current_state\":\""
        << (value.value().current_state == SERVICE_RUNNING
                ? L"running" : value.value().current_state == SERVICE_STOPPED
                    ? L"stopped" : L"pending") << L"\","
        << L"\"accept_stop\":"
        << ((value.value().controls_accepted & SERVICE_ACCEPT_STOP) != 0
                ? L"true" : L"false") << L","
        << L"\"accept_preshutdown\":"
        << ((value.value().controls_accepted
                & SERVICE_ACCEPT_PRESHUTDOWN) != 0 ? L"true" : L"false")
        << L",\"accept_power_event\":"
        << ((value.value().controls_accepted
                & SERVICE_ACCEPT_POWEREVENT) != 0 ? L"true" : L"false")
        << L",\"restart_delays_ms\":[";
    for (std::size_t index = 0; index < value.value().delays.size(); ++index) {
        if (index != 0) std::wcout << L",";
        std::wcout << value.value().delays[index];
    }
    std::wcout << L"]}" << std::endl;
    return 0;
}

int delete_engineering_service(const std::wstring_view name) {
    if (!valid_engineering_service_name(name)) return 2;
    ScHandle manager(OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) return 3;
    const std::wstring service_name(name);
    ScHandle service(OpenServiceW(
        manager.get(), service_name.c_str(),
        DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : 4;
    }
    SERVICE_STATUS status{};
    static_cast<void>(ControlService(service.get(), SERVICE_CONTROL_STOP, &status));
    if (!DeleteService(service.get())
        && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
        return 5;
    }
    return 0;
}
#endif

Result<host::WorkerRuntimeStatus> query_worker_status(
    host::ServiceReattachmentResult& connection) {
    auto response = connection.exchange(host::HostMessage{
        connection.binding().profile_id,
        connection.binding().image_id,
        host::kWorkerRuntimeSnapshotQuery,
        0,
        {}});
    if (!response.ok()) {
        return Result<host::WorkerRuntimeStatus>::failure(response.error());
    }
    if (response.value().command_id != host::kWorkerRuntimeSnapshotResponse) {
        return service_error<host::WorkerRuntimeStatus>(
            WindowsServiceError::worker_startup_failed,
            L"windows_service.worker_status",
            L"The authenticated worker returned an unexpected status command");
    }
    return host::decode_worker_runtime_status(
        response.value().payload, connection.binding());
}

bool same_live_worker_binding(
    const host::WorkerBinding& previous,
    const host::WorkerBinding& next) noexcept {
    return host::is_valid(previous) && host::is_valid(next)
        && next.profile_id == previous.profile_id
        && next.image_id == previous.image_id
        && next.worker_pid == previous.worker_pid
        && next.process_creation_time == previous.process_creation_time
        && next.profile_revision == previous.profile_revision
        && next.product_major == previous.product_major
        && next.product_minor == previous.product_minor
        && next.product_patch == previous.product_patch;
}

bool same_live_runtime_and_forward_progress(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerRuntimeStatus& next) noexcept {
    return next.device == previous.device
        && next.aim_device_id == previous.aim_device_id
        && next.proxy_native_path == previous.proxy_native_path
        && next.snapshot_sequence > previous.snapshot_sequence
        && next.heartbeat_sequence >= previous.heartbeat_sequence
        && next.dirty_epoch >= previous.dirty_epoch
        && next.committed_generation >= previous.committed_generation;
}

bool safe_live_reconnect_binding(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerBinding& managed,
    const host::WorkerBinding& visible) noexcept {
    if (!same_live_worker_binding(previous.binding, managed)
        || !same_live_worker_binding(managed, visible)
        || managed.session_generation < previous.binding.session_generation) {
        return false;
    }
    if (visible == managed) return true;
    return managed.session_generation
            != (std::numeric_limits<std::uint64_t>::max)()
        && visible.session_generation == managed.session_generation + 1;
}

bool safe_managed_live_status(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerBinding& managed,
    const host::WorkerRuntimeStatus& next) noexcept {
    return next.binding == managed
        && same_live_worker_binding(previous.binding, managed)
        && managed.session_generation >= previous.binding.session_generation
        && same_live_runtime_and_forward_progress(previous, next);
}

bool exact_live_reconnect_successor(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerRuntimeStatus& next) noexcept {
    const auto& old_binding = previous.binding;
    const auto& new_binding = next.binding;
    return old_binding.session_generation
            != (std::numeric_limits<std::uint64_t>::max)()
        && new_binding.session_generation
            == old_binding.session_generation + 1
        && safe_managed_live_status(previous, new_binding, next);
}

WorkerRuntimeIdentity runtime_identity_from(
    const host::WorkerRuntimeStatus& status) {
    const auto& binding = status.binding;
    return {
        binding.profile_id, binding.image_id, binding.worker_pid,
        binding.process_creation_time, binding.session_generation,
        binding.profile_revision, binding.product_major,
        binding.product_minor, binding.product_patch};
}

WorkerRuntimeIdentity runtime_identity_from(
    const host::WorkerBinding& binding) {
    return {
        binding.profile_id, binding.image_id, binding.worker_pid,
        binding.process_creation_time, binding.session_generation,
        binding.profile_revision, binding.product_major,
        binding.product_minor, binding.product_patch};
}

WorkerDeviceIdentity device_identity_from(
    const host::WorkerRuntimeStatus& status) {
    const auto& device = status.device;
    return {
        device.physical_drive_number, device.length_bytes,
        device.port_number, device.path_id, device.target_id, device.lun,
        device.gpt_disk_id, device.partition_id, device.volume_id};
}

class ProductRecoveryActions final : public IWorkerFailureRecoveryActions {
public:
    using Ensure = std::function<Result<WorkerRestartExecutionProof>(
        const WorkerRestartRecoveryRequest&, std::uint64_t)>;

    ProductRecoveryActions(
        const profile::ProfileRecord& record,
        const host::WorkerRuntimeStatus& persisted,
        Ensure ensure)
        : record_(&record), persisted_(&persisted),
          ensure_(std::move(ensure)) {}

    Result<WorkerCheckpointRecoveryProof> reconcile_interrupted_checkpoint(
        const WorkerRestartRecoveryRequest& request) override {
        return windows::reconcile_interrupted_checkpoint(
            *record_, *persisted_, request);
    }

    Result<WorkerResidualDeviceRecoveryProof> reconcile_residual_device(
        const WorkerRestartRecoveryRequest& request) override {
        return reconcile_windows_residual_device(*persisted_, request);
    }

    Result<WorkerRestartExecutionProof> ensure_restarted_from_checkpoint(
        const WorkerRestartRecoveryRequest& request,
        const std::uint64_t generation) override {
        return ensure_(request, generation);
    }

private:
    const profile::ProfileRecord* record_{};
    const host::WorkerRuntimeStatus* persisted_{};
    Ensure ensure_;
};

class ProductServiceActions final
    : public IServiceStartupActions,
      public IServiceShutdownActions {
public:
    ProductServiceActions() {
        auto root = trusted_program_data_root();
        if (root.ok()) {
            program_data_ = root.move_value();
        } else {
            initialization_error_ = root.error();
        }
    }

    Result<void> initialize_logging() override {
        if (initialization_error_.has_value()) {
            return Result<void>::failure(initialization_error_.value());
        }
        OutputDebugStringW(L"MyRamDisk service startup\n");
        return Result<void>::success();
    }

    Result<void> acquire_singleton() override {
        auto singleton = create_service_singleton(true);
        if (!singleton.ok()) return Result<void>::failure(singleton.error());
        singleton_ = singleton.move_value();
        return Result<void>::success();
    }

    Result<void> verify_worker_identity() override {
        const auto service = module_path();
        if (service.empty()) {
            return service_error<void>(
                WindowsServiceError::unsafe_worker_path,
                L"windows_service.worker_identity",
                L"The installed service path could not be resolved",
                GetLastError());
        }
        worker_path_ = (
            std::filesystem::path(service).parent_path()
            / std::wstring(kWorkerExecutableName)).lexically_normal().wstring();
        WorkerLaunchRequest request{
            worker_path_,
            fixed_uuid("11111111-1111-4111-8111-111111111111"),
            fixed_uuid("22222222-2222-4222-8222-222222222222"),
            1,
            1
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            ,
            false,
            false,
            false
#endif
        };
        auto verified = validate_worker_path(request);
        if (!verified.ok()) return Result<void>::failure(verified.error());
        return Result<void>::success();
    }

    Result<void> verify_driver() override {
        const auto status = setup::inspect_installed_driver(
            setup::driver_manifest());
        if (status.status != setup::DriverStatus::running_compatible) {
            return service_error<void>(
                WindowsServiceError::startup_failed,
                L"windows_service.driver_identity",
                L"The pinned AIM driver is not running and compatible");
        }
        return Result<void>::success();
    }

    Result<std::vector<StartupProfile>> discover_profiles() override {
        entries_.clear();
        constexpr std::array<std::wstring_view, 2> components{
            L"MyRamDisk", L"profiles"};
        auto profiles = profile::open_managed_atomic_directory(
            program_data_, components);
        if (!profiles.ok()) {
            if (profiles.error().native_code == ERROR_FILE_NOT_FOUND
                || profiles.error().native_code == ERROR_PATH_NOT_FOUND
                || profiles.error().code == static_cast<std::uint32_t>(
                    profile::ProfileStoreError::path_not_found)) {
                return Result<std::vector<StartupProfile>>::success({});
            }
            return Result<std::vector<StartupProfile>>::failure(
                profiles.error());
        }
        auto names = profiles.value().directory->list_files();
        if (!names.ok()) {
            return Result<std::vector<StartupProfile>>::failure(names.error());
        }
        std::vector<StartupProfile> discovered;
        discovered.reserve(names.value().size());
        entries_.reserve(names.value().size());
        workers_.reserve(names.value().size());
        for (const auto& name : names.value()) {
            std::string narrow;
            narrow.reserve(name.size());
            bool ascii = true;
            for (const wchar_t value : name) {
                if (value < 0 || value > 0x7f) {
                    ascii = false;
                    break;
                }
                narrow.push_back(static_cast<char>(value));
            }
            if (!ascii) continue;
            auto id = product::Uuid::parse(narrow);
            if (!id.ok() || id.value().to_string() != narrow) continue;
            ProfileEntry entry{id.value(), std::nullopt, std::nullopt};
            auto store = profile::ProfileStore::open_existing(
                program_data_, id.value());
            if (!store.ok()) {
                entry.error = store.error();
            } else {
                auto record = store.value().load_current();
                if (!record.ok()) {
                    entry.error = record.error();
                } else {
                    entry.record = record.move_value();
                }
            }
            discovered.push_back({
                entry.id,
                entry.record.has_value()
                    && entry.record->profile.auto_start});
            entries_.push_back(std::move(entry));
        }
        return Result<std::vector<StartupProfile>>::success(
            std::move(discovered));
    }

    Result<void> validate_profile(
        const product::Uuid& profile_id) override {
        auto* entry = find_entry(profile_id);
        if (entry == nullptr) {
            return service_error<void>(
                WindowsServiceError::startup_failed,
                L"windows_service.profile_validate",
                L"The discovered profile disappeared");
        }
        if (entry->error.has_value()) {
            return Result<void>::failure(entry->error.value());
        }
        if (!entry->record.has_value()) {
            return service_error<void>(
                WindowsServiceError::startup_failed,
                L"windows_service.profile_validate",
                L"The discovered profile has no durable record");
        }
        const auto validated = profile::validate_profile(entry->record.value());
        if (!validated.ok()
            || entry->record->profile.lifecycle
                != profile::ProfileLifecycle::ready) {
            return service_error<void>(
                WindowsServiceError::startup_failed,
                L"windows_service.profile_validate",
                L"The durable profile is invalid or not ready");
        }
        return Result<void>::success();
    }

    Result<bool> reattach_profile(
        const product::Uuid& profile_id) override {
        auto* entry = find_entry(profile_id);
        if (entry == nullptr || !entry->record.has_value()) {
            return service_error<bool>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.reattach",
                L"The profile record is unavailable for reattachment");
        }
        auto directory = host::open_runtime_lease_directory(
            program_data_, profile_id);
        if (!directory.ok()) {
            return Result<bool>::failure(directory.error());
        }
        auto binding = host::inspect_runtime_lease_binding(
            *directory.value());
        if (!binding.ok()) {
            const auto native = binding.error().native_code;
            if (native == ERROR_FILE_NOT_FOUND
                || native == ERROR_PATH_NOT_FOUND) {
                return Result<bool>::success(false);
            }
            return Result<bool>::failure(binding.error());
        }
        const auto& record = entry->record.value();
        if (binding.value().profile_id != profile_id
            || binding.value().image_id != record.profile.image_uuid
            || binding.value().profile_revision != record.revision
            || binding.value().product_major != product::kProductVersionMajor
            || binding.value().product_minor != product::kProductVersionMinor
            || binding.value().product_patch != product::kProductVersionPatch) {
            return service_error<bool>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.reattach_binding",
                L"The runtime lease differs from durable profile truth");
        }
        WindowsProcessIdentityQuery process_query;
        auto pin = process_query.pin_process(binding.value().worker_pid);
        if (!pin.ok()) {
            const auto native = pin.error().native_code;
            if (native == ERROR_INVALID_PARAMETER
                || native == ERROR_NOT_FOUND) {
                WorkerFailureObservation observation{
                    runtime_identity_from(binding.value()),
                    true, false, false, 0, false, 0};
                return recover_dead_profile(
                    record, binding.value(), observation);
            }
            return Result<bool>::failure(pin.error());
        }
        if (!pin.value()->is_running()
            || pin.value()->process_creation_time()
                != binding.value().process_creation_time) {
            WorkerFailureObservation observation{
                runtime_identity_from(binding.value()),
                true, true, pin.value()->is_running(),
                pin.value()->process_creation_time(), false, 0};
            return recover_dead_profile(
                record, binding.value(), observation);
        }
        auto persisted = load_persisted_worker_status(
            program_data_, binding.value());
        if (!persisted.ok()) {
            return Result<bool>::failure(persisted.error());
        }
        // Connecting to an already-live worker is a short bounded discovery
        // step.  Once authenticated, checkpoint and controlled-stop requests
        // retain the full preshutdown budget just like a freshly launched
        // worker; a service restart must not silently shorten disk operations
        // to the reattachment handshake deadline.
        auto transport = NamedPipeWorkerTransport::connect(
            profile_id, 30'000, kPreshutdownTimeoutMs);
        if (!transport.ok()) return Result<bool>::failure(transport.error());
        auto connection = host::reattach_and_rotate_runtime_lease(
            *directory.value(), transport.move_value(), binding.value(),
            record.revision);
        if (!connection.ok()) {
            return Result<bool>::failure(connection.error());
        }
        workers_.emplace_back(
            profile_id, std::move(pin.value()),
            connection.move_value(), persisted.move_value());
        auto& adopted = workers_.back();
        auto status = query_worker_status(adopted.connection);
        if (!status.ok()
            || !safe_managed_live_status(
                adopted.last_status, adopted.connection.binding(),
                status.value())) {
            // Authentication, process pinning and lease rotation succeeded,
            // so the live disk must remain owned by this Service even when
            // runtime status is temporarily unavailable or contradictory.
            mark_runtime_unresolved(profile_id);
            return Result<bool>::success(true);
        }
        adopted.last_status = status.move_value();
        auto saved = persist_worker_status(adopted.last_status);
        if (!saved.ok()) {
            mark_runtime_unresolved(profile_id);
            return Result<bool>::success(true);
        }
        clear_runtime_unresolved(profile_id);
        return Result<bool>::success(true);
    }

    Result<void> start_profile(
        const product::Uuid& profile_id) override {
        auto* entry = find_entry(profile_id);
        if (entry == nullptr || !entry->record.has_value()) {
            return service_error<void>(
                WindowsServiceError::worker_launch_failed,
                L"windows_service.start_profile",
                L"The profile record is unavailable for worker launch");
        }
        const auto& record = entry->record.value();
        WorkerLaunchRequest request{
            worker_path_,
            profile_id,
            record.profile.image_uuid,
            1,
            record.revision
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            ,
            false,
            false,
            false
#endif
        };
        auto launched = launch_worker(request);
        if (!launched.ok()) return Result<void>::failure(launched.error());
        auto attachment = launched.value().attach_startup(120'000);
        if (!attachment.ok()) {
            auto exited = launched.value().wait(0);
            if (exited.ok()) {
                return service_error<void>(
                    WindowsServiceError::worker_startup_failed,
                    L"windows_service.worker_startup_exit",
                    L"Worker exited before publishing its authenticated endpoint",
                    exited.value());
            }
            return Result<void>::failure(attachment.error());
        }
        auto adopted = attachment.move_value();
        workers_.emplace_back(
            profile_id, std::move(launched.value()),
            std::move(adopted.connection), std::move(adopted.status));
        auto persisted = persist_worker_status(workers_.back().last_status);
        if (!persisted.ok()) return persisted;
        return Result<void>::success();
    }

    std::uint64_t monotonic_time_ms() const noexcept override {
        return GetTickCount64();
    }

    Result<std::vector<product::Uuid>> running_profiles() override {
        std::vector<product::Uuid> result;
        result.reserve(workers_.size() + unresolved_runtime_profiles_.size());
        for (const auto& worker : workers_) result.push_back(worker.profile_id);
        for (const auto& profile_id : unresolved_runtime_profiles_) {
            if (std::find(result.begin(), result.end(), profile_id)
                == result.end()) {
                result.push_back(profile_id);
            }
        }
        return Result<std::vector<product::Uuid>>::success(std::move(result));
    }

    Result<bool> checkpoint_best_effort(
        const product::Uuid& profile_id,
        const std::uint64_t monotonic_deadline_ms) override {
        auto* entry = find_entry(profile_id);
        auto* worker = find_worker(profile_id);
        if (entry == nullptr || !entry->record.has_value()
            || worker == nullptr || !worker->is_running()) {
            return Result<bool>::success(false);
        }
        const auto policy = checkpoint_reason_ == CheckpointReason::suspend
            ? entry->record->profile.checkpoint.on_suspend
            : entry->record->profile.checkpoint.on_shutdown;
        if (policy == profile::BestEffortPolicy::disabled) {
            return Result<bool>::success(true);
        }
        const auto now = monotonic_time_ms();
        if (now >= monotonic_deadline_ms) return Result<bool>::success(false);
        const auto remaining = std::min<std::uint64_t>(
            monotonic_deadline_ms - now, 120'000);
        auto operation_id = product::Uuid::random();
        if (!operation_id.ok()) return Result<bool>::failure(operation_id.error());
        auto response = worker->connection.exchange(host::HostMessage{
            profile_id,
            operation_id.move_value(),
            host::kWorkerCheckpointRequest,
            static_cast<std::uint32_t>(remaining),
            {}});
        if (!response.ok()) return Result<bool>::failure(response.error());
        if (response.value().command_id != host::kWorkerCheckpointResponse) {
            return service_error<bool>(
                WindowsServiceError::worker_startup_failed,
                L"windows_service.checkpoint",
                L"The worker returned an unexpected checkpoint response");
        }
        auto reply = host::decode_worker_checkpoint_reply(
            response.value().payload);
        if (!reply.ok()) return Result<bool>::failure(reply.error());
        if (reply.value().confirmed) {
            auto status = query_worker_status(worker->connection);
            if (!status.ok()) return Result<bool>::failure(status.error());
            worker->last_status = status.move_value();
            auto persisted = persist_worker_status(worker->last_status);
            if (!persisted.ok()) {
                return Result<bool>::failure(persisted.error());
            }
        }
        return Result<bool>::success(reply.value().confirmed);
    }

    Result<bool> stop_safely(
        const product::Uuid& profile_id,
        const std::uint64_t monotonic_deadline_ms) override {
        auto* worker = find_worker(profile_id);
        if (worker == nullptr) {
            return Result<bool>::success(
                std::find(
                    unresolved_runtime_profiles_.begin(),
                    unresolved_runtime_profiles_.end(), profile_id)
                == unresolved_runtime_profiles_.end());
        }
        if (worker->safe_stop_confirmed) {
            const auto now = monotonic_time_ms();
            if (now >= monotonic_deadline_ms) {
                return Result<bool>::success(false);
            }
            auto exited = worker->wait_for_exit(static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    monotonic_deadline_ms - now, 30'000)));
            if (!exited.ok()) return Result<bool>::failure(exited.error());
            if (!exited.value()) return Result<bool>::success(false);
            auto removed = remove_runtime_metadata(profile_id);
            if (!removed.ok()) return Result<bool>::failure(removed.error());
            erase_worker(profile_id);
            return Result<bool>::success(true);
        }
        if (!worker->is_running()) return Result<bool>::success(false);
        const auto now = monotonic_time_ms();
        if (now >= monotonic_deadline_ms) return Result<bool>::success(false);
        const auto remaining = std::min<std::uint64_t>(
            monotonic_deadline_ms - now, 120'000);
        auto operation_id = product::Uuid::random();
        if (!operation_id.ok()) return Result<bool>::failure(operation_id.error());
        auto response = worker->connection.exchange(host::HostMessage{
            profile_id,
            operation_id.move_value(),
            host::kWorkerControlledStopRequest,
            static_cast<std::uint32_t>(remaining),
            {}});
        if (!response.ok()) return Result<bool>::failure(response.error());
        if (response.value().command_id
            != host::kWorkerControlledStopResponse) {
            return service_error<bool>(
                WindowsServiceError::worker_startup_failed,
                L"windows_service.controlled_stop",
                L"The worker returned an unexpected stop response");
        }
        auto reply = host::decode_worker_controlled_stop_reply(
            response.value().payload);
        if (!reply.ok()) return Result<bool>::failure(reply.error());
        if (!reply.value().safely_stopped) return Result<bool>::success(false);
        worker->safe_stop_confirmed = true;
        const auto after_reply = monotonic_time_ms();
        if (after_reply >= monotonic_deadline_ms) {
            return Result<bool>::success(false);
        }
        auto exited = worker->wait_for_exit(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                monotonic_deadline_ms - after_reply, 30'000)));
        if (!exited.ok()) return Result<bool>::failure(exited.error());
        if (!exited.value()) return Result<bool>::success(false);
        auto removed = remove_runtime_metadata(profile_id);
        if (!removed.ok()) return Result<bool>::failure(removed.error());
        erase_worker(profile_id);
        return Result<bool>::success(true);
    }

    enum class CheckpointReason : std::uint8_t { shutdown, suspend };
    void set_checkpoint_reason(const CheckpointReason reason) noexcept {
        checkpoint_reason_ = reason;
    }

    void poll_workers() noexcept {
        try {
            std::size_t index = 0;
            while (index < workers_.size()) {
                auto& worker = workers_[index];
                auto status = query_worker_status(worker.connection);
                if (status.ok()) {
                    const bool monotonic = safe_managed_live_status(
                        worker.last_status, worker.connection.binding(),
                        status.value());
                    if (!monotonic) {
                        OutputDebugStringW(
                            L"MyRamDisk rejected a stale worker heartbeat\n");
                        ++index;
                        continue;
                    }
                    const bool changed = status.value() != worker.last_status;
                    worker.last_status = status.move_value();
                    if (changed) {
                        auto persisted = persist_worker_status(
                            worker.last_status);
                        if (!persisted.ok()) {
                            OutputDebugStringW(
                                L"MyRamDisk could not persist worker identity\n");
                        }
                    }
                    ++index;
                    continue;
                }

                const auto failed_status = worker.last_status;
                WindowsProcessIdentityQuery processes;
                auto pin = processes.pin_process(
                    failed_status.binding.worker_pid);
                WorkerFailureObservation observation{
                    runtime_identity_from(failed_status),
                    true, false, false, 0, false, 0};
                bool exit_proven = false;
                if (!pin.ok()) {
                    const auto native = pin.error().native_code;
                    exit_proven = native == ERROR_INVALID_PARAMETER
                        || native == ERROR_NOT_FOUND;
                    observation.process_query_succeeded = exit_proven;
                } else {
                    observation.process_present = true;
                    observation.process_running = pin.value()->is_running();
                    observation.observed_process_creation_time =
                        pin.value()->process_creation_time();
                    exit_proven = !observation.process_running
                        || observation.observed_process_creation_time
                            != failed_status.binding.process_creation_time;
                }
                if (!exit_proven) {
                    auto reconnected = reconnect_live_worker(worker);
                    if (!reconnected.ok()) {
                        OutputDebugStringW(
                            L"MyRamDisk live worker reconnect failed; "
                            L"automatic kill is forbidden\n");
                    }
                    ++index;
                    continue;
                }
                auto* entry = find_entry(worker.profile_id);
                if (entry == nullptr || !entry->record.has_value()) {
                    OutputDebugStringW(
                        L"MyRamDisk dead worker has no durable profile\n");
                    ++index;
                    continue;
                }
                const auto binding = failed_status.binding;
                workers_.erase(
                    workers_.begin() + static_cast<std::ptrdiff_t>(index));
                auto recovered = recover_dead_profile(
                    entry->record.value(), binding, observation);
                if (!recovered.ok() || !recovered.value()) {
                    OutputDebugStringW(
                        L"MyRamDisk runtime worker recovery requires attention\n");
                }
                // A replacement, when successful, is appended at the end.
                // Continue at the same index to inspect the next old entry.
            }
            poll_pending_recoveries();
        } catch (...) {
            OutputDebugStringW(
                L"MyRamDisk worker supervision encountered an exception\n");
        }
    }

private:
    struct ProfileEntry final {
        product::Uuid id;
        std::optional<profile::ProfileRecord> record;
        std::optional<Error> error;
    };

    struct ManagedWorker final {
        ManagedWorker(
            product::Uuid id,
            WorkerProcess process_value,
            host::ServiceReattachmentResult connection_value,
            host::WorkerRuntimeStatus status_value) noexcept
            : profile_id(std::move(id)),
              launched(std::move(process_value)),
              connection(std::move(connection_value)),
              last_status(std::move(status_value)) {}
        ManagedWorker(
            product::Uuid id,
            std::unique_ptr<IWorkerProcessIdentityPin> pin_value,
            host::ServiceReattachmentResult connection_value,
            host::WorkerRuntimeStatus status_value) noexcept
            : profile_id(std::move(id)), pin(std::move(pin_value)),
              connection(std::move(connection_value)),
              last_status(std::move(status_value)) {}
        ManagedWorker(ManagedWorker&&) noexcept = default;
        ManagedWorker& operator=(ManagedWorker&&) noexcept = default;
        ManagedWorker(const ManagedWorker&) = delete;
        ManagedWorker& operator=(const ManagedWorker&) = delete;

        bool is_running() const noexcept {
            return launched.has_value()
                ? launched->is_running() : pin && pin->is_running();
        }

        Result<bool> wait_for_exit(const std::uint32_t timeout_ms) noexcept {
            if (launched.has_value()) {
                auto waited = launched->wait(timeout_ms);
                if (!waited.ok()) {
                    if (waited.error().native_code == ERROR_TIMEOUT) {
                        return Result<bool>::success(false);
                    }
                    return Result<bool>::failure(waited.error());
                }
                // The safe-stop reply proves the disk was checkpointed and
                // removed.  Any subsequent process exit is sufficient to
                // retire the runtime metadata; the exit code is diagnostic,
                // not part of the device-removal proof.
                return Result<bool>::success(true);
            }
            if (pin) return pin->wait_for_exit(timeout_ms);
            return Result<bool>::success(false);
        }

        product::Uuid profile_id;
        std::optional<WorkerProcess> launched;
        std::unique_ptr<IWorkerProcessIdentityPin> pin;
        host::ServiceReattachmentResult connection;
        host::WorkerRuntimeStatus last_status;
        bool safe_stop_confirmed{};
    };

    struct PendingRecovery final {
        host::WorkerBinding failed_binding;
        WorkerFailureObservation observation;
        std::uint32_t attempts{};
        std::uint64_t next_retry_ms{};
    };

    ProfileEntry* find_entry(const product::Uuid& id) noexcept {
        const auto found = std::find_if(
            entries_.begin(), entries_.end(),
            [&id](const ProfileEntry& value) { return value.id == id; });
        return found == entries_.end() ? nullptr : &*found;
    }

    ManagedWorker* find_worker(const product::Uuid& id) noexcept {
        const auto found = std::find_if(
            workers_.begin(), workers_.end(),
            [&id](const ManagedWorker& value) {
                return value.profile_id == id;
            });
        return found == workers_.end() ? nullptr : &*found;
    }

    Result<void> reconnect_live_worker(ManagedWorker& worker) {
        auto* entry = find_entry(worker.profile_id);
        if (entry == nullptr || !entry->record.has_value()) {
            return service_error<void>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.live_reconnect_profile",
                L"The live worker has no validated durable profile");
        }
        auto directory = host::open_runtime_lease_directory(
            program_data_, worker.profile_id);
        if (!directory.ok()) {
            return Result<void>::failure(directory.error());
        }
        auto visible = host::inspect_runtime_lease_binding(
            *directory.value());
        if (!visible.ok()) return Result<void>::failure(visible.error());
        auto baseline = worker.last_status;
        const auto managed_binding = worker.connection.binding();
        const auto& binding = visible.value();
        if (!safe_live_reconnect_binding(
                worker.last_status, managed_binding, binding)
            || binding.profile_revision != entry->record->revision
            || binding.session_generation
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return service_error<void>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.live_reconnect_binding",
                L"The live worker lease changed outside the exact reconnect boundary");
        }
        // A previous reconnect may have durably advanced the lease and
        // authenticated connection before its status reply was lost. The
        // visible exact current-or-next binding is then the only safe baseline
        // for another one-step rotation; runtime/device truth still comes from
        // the last fully validated status below.
        baseline.binding = binding;

        // Release the failed authenticated transport first. The Host's
        // per-connection owner then permits exactly one fresh manager while
        // the pinned process and device continue serving block I/O.
        worker.connection.disconnect();
        auto transport = NamedPipeWorkerTransport::connect(
            worker.profile_id, 30'000, kPreshutdownTimeoutMs);
        if (!transport.ok()) return Result<void>::failure(transport.error());
        auto connection = host::reattach_and_rotate_runtime_lease(
            *directory.value(), transport.move_value(), binding,
            entry->record->revision);
        if (!connection.ok()) {
            return Result<void>::failure(connection.error());
        }
        // Adopt the authenticated successor before any fallible status query.
        // A lost reply therefore leaves the live disk managed by this Service
        // and the next poll can retry from the exact adopted binding.
        worker.connection = connection.move_value();
        auto status = query_worker_status(worker.connection);
        if (!status.ok()) return Result<void>::failure(status.error());
        if (!exact_live_reconnect_successor(baseline, status.value())) {
            return service_error<void>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.live_reconnect_status",
                L"The reconnected live worker changed device identity or runtime truth");
        }
        worker.last_status = status.move_value();
        clear_runtime_unresolved(worker.profile_id);
        return persist_worker_status(worker.last_status);
    }

    Result<bool> recover_dead_profile(
        const profile::ProfileRecord& record,
        const host::WorkerBinding& failed_binding,
        WorkerFailureObservation observation) {
        schedule_pending_recovery(failed_binding, observation);
        auto recovered = recover_dead_profile_once(
            record, failed_binding, std::move(observation));
        if (recovered.ok() && recovered.value()) {
            clear_pending_recovery(failed_binding.profile_id);
            clear_runtime_unresolved(failed_binding.profile_id);
        } else {
            defer_pending_recovery(failed_binding.profile_id);
        }
        return recovered;
    }

    Result<bool> recover_dead_profile_once(
        const profile::ProfileRecord& record,
        const host::WorkerBinding& failed_binding,
        WorkerFailureObservation observation) {
        auto persisted = load_persisted_worker_status(
            program_data_, failed_binding);
        if (!persisted.ok()) {
            return Result<bool>::failure(persisted.error());
        }
        const auto expected = runtime_identity_from(persisted.value());
        observation.expected = expected;
        auto recovery_operation = derive_worker_recovery_operation_id(expected);
        if (!recovery_operation.ok()) {
            return Result<bool>::failure(recovery_operation.error());
        }
        WorkerRestartRecoveryRequest request{
            expected,
            device_identity_from(persisted.value()),
            recovery_operation.move_value(),
            persisted.value().committed_generation,
            persisted.value().dirty_epoch};
        DurableWorkerRestartHistoryStore history(program_data_);
        SystemServiceClock clock;
        WorkerRestartLimiter limiter(clock, history);
        ProductRecoveryActions actions(
            record, persisted.value(),
            [this](
                const WorkerRestartRecoveryRequest& recovery,
                const std::uint64_t generation) {
                return ensure_recovery_worker(recovery, generation);
            });
        WorkerFailureRecoveryCoordinator coordinator(limiter, actions);
        auto recovered = coordinator.recover(request, observation);
        if (!recovered.ok()) {
            return Result<bool>::failure(recovered.error());
        }
        if (recovered.value().disposition
            != WorkerRestartRecoveryDisposition::restarted) {
            return service_error<bool>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.recover_dead_worker",
                L"The proven-dead worker could not be reconciled safely; "
                L"the profile requires attention");
        }
        return Result<bool>::success(true);
    }

    void schedule_pending_recovery(
        const host::WorkerBinding& binding,
        const WorkerFailureObservation& observation) {
        mark_runtime_unresolved(binding.profile_id);
        const auto found = std::find_if(
            pending_recoveries_.begin(), pending_recoveries_.end(),
            [&](const PendingRecovery& pending) {
                return pending.failed_binding.profile_id
                    == binding.profile_id;
            });
        if (found == pending_recoveries_.end()) {
            pending_recoveries_.push_back({
                binding, observation, 0, monotonic_time_ms()});
        }
    }

    void defer_pending_recovery(const product::Uuid& profile_id) noexcept {
        const auto found = std::find_if(
            pending_recoveries_.begin(), pending_recoveries_.end(),
            [&](const PendingRecovery& pending) {
                return pending.failed_binding.profile_id == profile_id;
            });
        if (found == pending_recoveries_.end()) return;
        if (found->attempts < 4) ++found->attempts;
        constexpr std::array<std::uint64_t, 3> delays{
            5'000, 15'000, 60'000};
        found->next_retry_ms = found->attempts >= 4
            ? (std::numeric_limits<std::uint64_t>::max)()
            : monotonic_time_ms() + delays[found->attempts - 1];
    }

    void clear_pending_recovery(const product::Uuid& profile_id) noexcept {
        pending_recoveries_.erase(std::remove_if(
            pending_recoveries_.begin(), pending_recoveries_.end(),
            [&](const PendingRecovery& pending) {
                return pending.failed_binding.profile_id == profile_id;
            }), pending_recoveries_.end());
    }

    void poll_pending_recoveries() {
        std::size_t index = 0;
        while (index < pending_recoveries_.size()) {
            const auto pending = pending_recoveries_[index];
            if (pending.next_retry_ms
                    == (std::numeric_limits<std::uint64_t>::max)()
                || monotonic_time_ms() < pending.next_retry_ms) {
                ++index;
                continue;
            }
            auto* entry = find_entry(pending.failed_binding.profile_id);
            if (entry == nullptr || !entry->record.has_value()) {
                defer_pending_recovery(pending.failed_binding.profile_id);
                ++index;
                continue;
            }
            static_cast<void>(recover_dead_profile(
                entry->record.value(), pending.failed_binding,
                pending.observation));
            const auto still_pending = std::find_if(
                pending_recoveries_.begin(), pending_recoveries_.end(),
                [&](const PendingRecovery& value) {
                    return value.failed_binding.profile_id
                        == pending.failed_binding.profile_id;
                });
            if (still_pending == pending_recoveries_.end()) continue;
            index = static_cast<std::size_t>(std::distance(
                pending_recoveries_.begin(), still_pending)) + 1;
        }
    }

    Result<WorkerRestartExecutionProof> ensure_recovery_worker(
        const WorkerRestartRecoveryRequest& recovery,
        const std::uint64_t generation) {
        if (auto* existing = find_worker(recovery.expected.profile_id)) {
            const auto& status = existing->last_status;
            if (existing->is_running()
                && status.binding.profile_id == recovery.expected.profile_id
                && status.binding.image_id == recovery.expected.image_id
                && status.binding.session_generation
                    == recovery.expected.session_generation + 1
                && status.binding.profile_revision
                    == recovery.expected.profile_revision
                && status.binding.process_creation_time
                    != recovery.expected.process_creation_time
                && status.committed_generation == generation) {
                return Result<WorkerRestartExecutionProof>::success({
                    recovery.expected.profile_id,
                    recovery.expected.image_id,
                    recovery.recovery_operation_id,
                    generation,
                    runtime_identity_from(status)});
            }
            return service_error<WorkerRestartExecutionProof>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.ensure_recovery_existing",
                L"An existing worker disagrees with the recovery operation");
        }
        if (recovery.expected.session_generation
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return service_error<WorkerRestartExecutionProof>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.ensure_recovery_generation",
                L"The worker session generation cannot advance safely");
        }
        WorkerLaunchRequest launch{
            worker_path_, recovery.expected.profile_id,
            recovery.expected.image_id,
            recovery.expected.session_generation + 1,
            recovery.expected.profile_revision
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
            , false, false, false
#endif
        };
        auto worker = launch_worker(launch);
        if (!worker.ok()) {
            return Result<WorkerRestartExecutionProof>::failure(
                worker.error());
        }
        auto attachment = worker.value().attach_startup(120'000);
        if (!attachment.ok()) {
            return Result<WorkerRestartExecutionProof>::failure(
                attachment.error());
        }
        auto adopted = attachment.move_value();
        const auto identity = runtime_identity_from(adopted.status);
        if (identity.profile_id != recovery.expected.profile_id
            || identity.image_id != recovery.expected.image_id
            || identity.session_generation
                != recovery.expected.session_generation + 1
            || identity.profile_revision != recovery.expected.profile_revision
            || identity.process_creation_time
                == recovery.expected.process_creation_time
            || adopted.status.committed_generation != generation) {
            return service_error<WorkerRestartExecutionProof>(
                WindowsServiceError::worker_identity_failed,
                L"windows_service.ensure_recovery_proof",
                L"The authenticated replacement worker returned stale identity");
        }
        workers_.emplace_back(
            recovery.expected.profile_id, std::move(worker.value()),
            std::move(adopted.connection), std::move(adopted.status));
        auto persisted = persist_worker_status(workers_.back().last_status);
        if (!persisted.ok()) {
            return Result<WorkerRestartExecutionProof>::failure(
                persisted.error());
        }
        return Result<WorkerRestartExecutionProof>::success({
            recovery.expected.profile_id,
            recovery.expected.image_id,
            recovery.recovery_operation_id,
            generation,
            runtime_identity_from(workers_.back().last_status)});
    }

    void erase_worker(const product::Uuid& id) {
        workers_.erase(std::remove_if(
            workers_.begin(), workers_.end(),
            [&id](const ManagedWorker& value) {
                return value.profile_id == id;
            }), workers_.end());
        clear_runtime_unresolved(id);
    }

    void mark_runtime_unresolved(const product::Uuid& id) {
        if (std::find(
                unresolved_runtime_profiles_.begin(),
                unresolved_runtime_profiles_.end(), id)
            == unresolved_runtime_profiles_.end()) {
            unresolved_runtime_profiles_.push_back(id);
        }
    }

    void clear_runtime_unresolved(const product::Uuid& id) noexcept {
        unresolved_runtime_profiles_.erase(std::remove(
            unresolved_runtime_profiles_.begin(),
            unresolved_runtime_profiles_.end(), id),
            unresolved_runtime_profiles_.end());
    }

    Result<void> persist_worker_status(
        const host::WorkerRuntimeStatus& status) {
        auto encoded = host::encode_worker_runtime_status(status);
        if (!encoded.ok()) return Result<void>::failure(encoded.error());
        auto directory = profile::ProfileDirectoryCapability::open_existing(
            program_data_, status.binding.profile_id);
        if (!directory.ok()) return Result<void>::failure(directory.error());
        return profile::publish_atomically_with_previous(
            directory.value(), host::kRuntimeIdentityFileName,
            host::kRuntimeIdentityPreviousFileName, encoded.value());
    }

    Result<void> remove_runtime_metadata(const product::Uuid& profile_id) {
        auto directory = profile::ProfileDirectoryCapability::open_existing(
            program_data_, profile_id);
        if (!directory.ok()) return Result<void>::failure(directory.error());
        constexpr std::array<std::wstring_view, 3> leaves{
            host::kRuntimeLeaseFileName,
            host::kRuntimeIdentityFileName,
            host::kRuntimeIdentityPreviousFileName};
        for (const auto leaf : leaves) {
            auto removed = directory.value().remove_file(leaf);
            if (!removed.ok()
                && removed.error().code != static_cast<std::uint32_t>(
                    profile::ProfileStoreError::path_not_found)) {
                return removed;
            }
        }
        auto flushed = directory.value().flush_directory();
        if (!flushed.ok()) return flushed;
        DurableWorkerRestartHistoryStore restart_history(program_data_);
        return restart_history.clear(profile_id);
    }

    std::wstring program_data_;
    std::wstring worker_path_;
    std::optional<Error> initialization_error_;
    NativeHandle singleton_;
    std::vector<ProfileEntry> entries_;
    std::vector<ManagedWorker> workers_;
    std::vector<product::Uuid> unresolved_runtime_profiles_;
    std::vector<PendingRecovery> pending_recoveries_;
    CheckpointReason checkpoint_reason_{CheckpointReason::shutdown};
};

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
class EngineeringServiceActions final
    : public IServiceStartupActions,
      public IServiceShutdownActions {
public:
    Result<void> initialize_logging() override {
        return Result<void>::success();
    }
    Result<void> acquire_singleton() override {
        auto singleton = create_service_singleton(true);
        if (!singleton.ok()) return Result<void>::failure(singleton.error());
        singleton_ = singleton.move_value();
        return Result<void>::success();
    }
    Result<void> verify_worker_identity() override {
        return Result<void>::success();
    }
    Result<void> verify_driver() override {
        return Result<void>::success();
    }
    Result<std::vector<StartupProfile>> discover_profiles() override {
        return Result<std::vector<StartupProfile>>::success({});
    }
    Result<void> validate_profile(const product::Uuid&) override {
        return Result<void>::success();
    }
    Result<bool> reattach_profile(const product::Uuid&) override {
        return Result<bool>::success(false);
    }
    Result<void> start_profile(const product::Uuid&) override {
        return Result<void>::success();
    }
    std::uint64_t monotonic_time_ms() const noexcept override {
        return GetTickCount64();
    }
    Result<std::vector<product::Uuid>> running_profiles() override {
        return Result<std::vector<product::Uuid>>::success({});
    }
    Result<bool> checkpoint_best_effort(
        const product::Uuid&, std::uint64_t) override {
        return Result<bool>::success(true);
    }
    Result<bool> stop_safely(
        const product::Uuid&, std::uint64_t) override {
        return Result<bool>::success(true);
    }

private:
    NativeHandle singleton_;
};
#endif

class PendingStatusPulse final {
public:
    PendingStatusPulse(
        std::function<bool()> publish,
        const std::chrono::milliseconds interval) noexcept
        : stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          publish_(std::move(publish)), interval_(interval) {
        if (!stop_event_ || !publish_ || interval_.count() <= 0
            || interval_.count() > (std::numeric_limits<DWORD>::max)()) {
            return;
        }
        try {
            worker_ = std::thread([this] {
                for (;;) {
                    const DWORD waited = WaitForSingleObject(
                        stop_event_.get(),
                        static_cast<DWORD>(interval_.count()));
                    if (waited == WAIT_OBJECT_0) return;
                    if (waited != WAIT_TIMEOUT || !publish_()) {
                        publication_failed_.store(
                            true, std::memory_order_release);
                        return;
                    }
                }
            });
        } catch (...) {
            stop_event_.reset();
        }
    }

    ~PendingStatusPulse() noexcept { stop(); }
    PendingStatusPulse(const PendingStatusPulse&) = delete;
    PendingStatusPulse& operator=(const PendingStatusPulse&) = delete;

    bool active() const noexcept {
        return stop_event_ && worker_.joinable();
    }

    bool publication_failed() const noexcept {
        return publication_failed_.load(std::memory_order_acquire);
    }

    void stop() noexcept {
        if (worker_.joinable()) {
            static_cast<void>(SetEvent(stop_event_.get()));
            worker_.join();
        }
    }

private:
    NativeHandle stop_event_;
    std::function<bool()> publish_;
    std::chrono::milliseconds interval_{};
    std::thread worker_;
    std::atomic_bool publication_failed_{};
};

class StopPendingCoordinator final {
public:
    ~StopPendingCoordinator() noexcept { static_cast<void>(finish()); }
    StopPendingCoordinator() = default;
    StopPendingCoordinator(const StopPendingCoordinator&) = delete;
    StopPendingCoordinator& operator=(const StopPendingCoordinator&) = delete;

    bool try_claim() noexcept {
        bool expected = false;
        return claimed_.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    void release_claim() noexcept {
        claimed_.store(false, std::memory_order_release);
    }

    bool begin(
        std::function<bool()> publish,
        const std::chrono::milliseconds interval) noexcept {
        std::scoped_lock lock(mutex_);
        if (pulse_) return true;
        try {
            auto pulse = std::make_unique<PendingStatusPulse>(
                std::move(publish), interval);
            if (!pulse->active()) return false;
            pulse_ = std::move(pulse);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool finish() noexcept {
        std::unique_ptr<PendingStatusPulse> pulse;
        {
            std::scoped_lock lock(mutex_);
            pulse = std::move(pulse_);
        }
        if (!pulse) return false;
        pulse->stop();
        return pulse->publication_failed();
    }

private:
    std::atomic_bool claimed_{};
    std::mutex mutex_;
    std::unique_ptr<PendingStatusPulse> pulse_;
};

class ScmRuntime final {
public:
    ScmRuntime(std::wstring service_name, const bool engineering)
        : service_name_(std::move(service_name)),
          engineering_(engineering),
          control_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

    void run() noexcept {
        try {
            run_impl();
        } catch (...) {
            static_cast<void>(stop_pending_.finish());
            if (handler_ != nullptr) {
                static_cast<void>(publish(
                    SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0));
            }
        }
    }

private:
    void run_impl() {
        handler_ = RegisterServiceCtrlHandlerExW(
            service_name_.c_str(), &ScmRuntime::control, this);
        if (handler_ == nullptr) return;
        if (!control_event_) {
            publish(SERVICE_STOPPED, GetLastError(), 0);
            return;
        }
        if (!publish(SERVICE_START_PENDING, NO_ERROR, 120'000)) return;
        PendingStatusPulse startup_pulse(
            [this] {
                return publish(
                    SERVICE_START_PENDING, NO_ERROR,
                    kPreshutdownTimeoutMs);
            },
            std::chrono::seconds(2));
        if (!startup_pulse.active()) {
            static_cast<void>(publish(
                SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0));
            return;
        }

        std::unique_ptr<ProductServiceActions> product_actions;
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
        std::unique_ptr<EngineeringServiceActions> engineering_actions;
#endif
        IServiceStartupActions* startup = nullptr;
        IServiceShutdownActions* shutdown = nullptr;
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
        if (engineering_) {
            engineering_actions = std::make_unique<EngineeringServiceActions>();
            startup = engineering_actions.get();
            shutdown = engineering_actions.get();
        } else {
            product_actions = std::make_unique<ProductServiceActions>();
            startup = product_actions.get();
            shutdown = product_actions.get();
        }
#else
        product_actions = std::make_unique<ProductServiceActions>();
        startup = product_actions.get();
        shutdown = product_actions.get();
#endif
        auto started = run_service_startup(*startup);
        startup_pulse.stop();
        if (!started.ok() || !started.value().globally_ready
            || startup_pulse.publication_failed()) {
            publish(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
            return;
        }
        for (const auto& failure : started.value().profile_failures) {
            const auto text = failure.profile_id.to_string();
            const std::wstring wide(text.begin(), text.end());
            OutputDebugStringW((L"MyRamDisk profile startup isolated: "
                + wide + L"\n").c_str());
        }
        if (!publish(SERVICE_RUNNING, NO_ERROR, 0)) return;

        for (;;) {
            const DWORD waited = WaitForSingleObject(
                control_event_.get(), product_actions ? 2'000U : INFINITE);
            if (waited == WAIT_TIMEOUT) {
                product_actions->poll_workers();
                continue;
            }
            if (waited != WAIT_OBJECT_0) {
                publish(SERVICE_STOPPED, GetLastError(), 0);
                return;
            }
            DWORD controls = 0;
            {
                std::scoped_lock control_lock(control_mutex_);
                controls = pending_controls_.exchange(
                    0, std::memory_order_acq_rel);
            }
            if ((controls & kPreshutdownRequested) != 0) {
                if (product_actions) {
                    product_actions->set_checkpoint_reason(
                        ProductServiceActions::CheckpointReason::shutdown);
                }
                const auto now = shutdown->monotonic_time_ms();
                static_cast<void>(run_best_effort_checkpoints(
                    *shutdown, true, now + kPreshutdownTimeoutMs));
            }
            if ((controls & kSuspendRequested) != 0) {
                if (product_actions) {
                    product_actions->set_checkpoint_reason(
                        ProductServiceActions::CheckpointReason::suspend);
                }
                const auto now = shutdown->monotonic_time_ms();
                static_cast<void>(run_best_effort_checkpoints(
                    *shutdown, true, now + kSuspendTimeoutMs));
            }
            if ((controls & (kStopRequested | kShutdownRequested)) == 0) {
                continue;
            }
            const auto now = shutdown->monotonic_time_ms();
            auto stopped = run_controlled_stop(
                *shutdown, now + kPreshutdownTimeoutMs);
            const bool pulse_publication_failed = stop_pending_.finish();
            if (!stopped.ok()
                || pulse_publication_failed
                || stopped.value().resulting_state
                    != ServiceSteadyState::stopped) {
                // A failed fail-closed stop leaves every unsafe disk managed
                // and explicitly returns the SCM contract to Running so the
                // completed control request does not wedge future controls.
                const bool running = publish(SERVICE_RUNNING, NO_ERROR, 0);
                stop_pending_.release_claim();
                if (!running) return;
                continue;
            }
            publish(SERVICE_STOPPED, NO_ERROR, 0);
            return;
        }
    }

    static DWORD WINAPI control(
        const DWORD control_code,
        const DWORD event_type,
        void*,
        void* context) noexcept {
        return static_cast<ScmRuntime*>(context)->handle_control(
            control_code, event_type);
    }

    DWORD handle_control(
        const DWORD control_code,
        const DWORD event_type) noexcept {
        std::scoped_lock control_lock(control_mutex_);
        DWORD requested = 0;
        if (control_code == SERVICE_CONTROL_PRESHUTDOWN) {
            requested = kPreshutdownRequested;
        } else if (control_code == SERVICE_CONTROL_POWEREVENT
                   && event_type == PBT_APMSUSPEND) {
            requested = kSuspendRequested;
        } else if (control_code == SERVICE_CONTROL_STOP) {
            requested = kStopRequested;
        } else if (control_code == SERVICE_CONTROL_SHUTDOWN) {
            requested = kShutdownRequested;
        } else if (control_code == SERVICE_CONTROL_INTERROGATE) {
            publish_current();
            return NO_ERROR;
        } else {
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
        const bool stopping =
            (requested & (kStopRequested | kShutdownRequested)) != 0;
        if (stopping && !stop_pending_.try_claim()) return NO_ERROR;
        pending_controls_.fetch_or(requested, std::memory_order_release);
        if (stopping) {
            if (!publish(
                    SERVICE_STOP_PENDING, NO_ERROR,
                    kPreshutdownTimeoutMs)) {
                pending_controls_.fetch_and(
                    ~(kStopRequested | kShutdownRequested),
                    std::memory_order_acq_rel);
                static_cast<void>(publish(SERVICE_RUNNING, NO_ERROR, 0));
                stop_pending_.release_claim();
                return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
            }
            if (!stop_pending_.begin(
                    [this] {
                        return publish(
                            SERVICE_STOP_PENDING, NO_ERROR,
                            kPreshutdownTimeoutMs);
                    },
                    std::chrono::seconds(2))) {
                pending_controls_.fetch_and(
                    ~(kStopRequested | kShutdownRequested),
                    std::memory_order_acq_rel);
                static_cast<void>(publish(SERVICE_RUNNING, NO_ERROR, 0));
                stop_pending_.release_claim();
                return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
            }
        }
        if (SetEvent(control_event_.get())) return NO_ERROR;
        const DWORD error = GetLastError();
        pending_controls_.fetch_and(~requested, std::memory_order_acq_rel);
        if (stopping) {
            static_cast<void>(stop_pending_.finish());
            pending_controls_.fetch_and(
                ~(kStopRequested | kShutdownRequested),
                std::memory_order_acq_rel);
            static_cast<void>(publish(SERVICE_RUNNING, NO_ERROR, 0));
            stop_pending_.release_claim();
        }
        return error;
    }

    bool publish(
        const DWORD state,
        const DWORD exit_code,
        const DWORD wait_hint) noexcept {
        if (handler_ == nullptr) return false;
        std::scoped_lock lock(status_mutex_);
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = state;
        status.dwWin32ExitCode = exit_code;
        status.dwWaitHint = wait_hint;
        if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
            status.dwCheckPoint = ++status_checkpoint_;
        }
        if (state == SERVICE_RUNNING) {
            status.dwControlsAccepted = SERVICE_ACCEPT_STOP
                | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN
                | SERVICE_ACCEPT_POWEREVENT;
        }
        current_status_ = status;
        return SetServiceStatus(handler_, &status) != FALSE;
    }

    void publish_current() noexcept {
        if (handler_ == nullptr) return;
        std::scoped_lock lock(status_mutex_);
        static_cast<void>(SetServiceStatus(handler_, &current_status_));
    }

    static constexpr DWORD kStopRequested = 0x01U;
    static constexpr DWORD kShutdownRequested = 0x02U;
    static constexpr DWORD kPreshutdownRequested = 0x04U;
    static constexpr DWORD kSuspendRequested = 0x08U;

    std::wstring service_name_;
    bool engineering_{};
    SERVICE_STATUS_HANDLE handler_{};
    NativeHandle control_event_;
    std::atomic<DWORD> pending_controls_{};
    std::mutex control_mutex_;
    std::mutex status_mutex_;
    SERVICE_STATUS current_status_{};
    DWORD status_checkpoint_{};
    StopPendingCoordinator stop_pending_;
};

ScmRuntime* active_runtime = nullptr;

void WINAPI scm_service_main(DWORD, wchar_t**) {
    if (active_runtime != nullptr) active_runtime->run();
}

int run_dispatcher(
    const std::wstring& service_name,
    const bool engineering) {
    ScmRuntime runtime(service_name, engineering);
    active_runtime = &runtime;
    SERVICE_TABLE_ENTRYW table[]{{
        const_cast<wchar_t*>(service_name.c_str()), scm_service_main},
        {nullptr, nullptr}};
    const BOOL dispatched = StartServiceCtrlDispatcherW(table);
    active_runtime = nullptr;
    return dispatched ? 0
        : static_cast<int>(WindowsServiceError::dispatcher_failed);
}

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
int launch_engineering_worker(int argc, wchar_t** argv) {
    const auto host_path = option_value(argc, argv, L"--host");
    const auto profile_text = option_value(argc, argv, L"--profile");
    const auto image_text = option_value(argc, argv, L"--image");
    if (!host_path || !profile_text || !image_text) return 2;
    auto profile = parse_wide_uuid(*profile_text);
    auto image = parse_wide_uuid(*image_text);
    if (!profile.ok() || !image.ok()) return 2;
    WorkerLaunchRequest request{
        std::wstring(*host_path),
        profile.move_value(),
        image.move_value(),
        1,
        1,
        true,
        false,
        has_option(argc, argv, L"--allow-unprotected-test-path")};
    auto worker = launch_worker(request);
    if (!worker.ok()) {
        std::wcerr << worker.error().operation << L": "
                   << worker.error().message << L" (native="
                   << worker.error().native_code << L")" << std::endl;
        return 3;
    }
    WindowsProcessIdentityQuery process_query;
    auto process_pin = process_query.pin_process(
        worker.value().binding().worker_pid);
    if (!process_pin.ok()
        || process_pin.value()->process_creation_time()
            != worker.value().binding().process_creation_time) {
        return 4;
    }
    auto exit_code = worker.value().wait(10'000);
    if (!exit_code.ok() || exit_code.value() != 0) return 5;
    const auto profile_id = worker.value().binding().profile_id.to_string();
    std::wcout
        << L"{\"started\":true,\"exit_code\":" << exit_code.value()
        << L",\"command_line\":\"MyRamDiskHost.exe --profile "
        << std::wstring(profile_id.begin(), profile_id.end())
        << L" --engineering-once\",\"inherited_handle_count\":3,"
        << L"\"process_pin_verified\":true,"
        << L"\"worker_token_mode\":\"inherited_full_localsystem\"}"
        << std::endl;
    return 0;
}

int authenticate_engineering_worker(int argc, wchar_t** argv) {
    const auto host_path = option_value(argc, argv, L"--host");
    const auto profile_text = option_value(argc, argv, L"--profile");
    const auto image_text = option_value(argc, argv, L"--image");
    if (!host_path || !profile_text || !image_text) return 2;
    auto profile = parse_wide_uuid(*profile_text);
    auto image = parse_wide_uuid(*image_text);
    if (!profile.ok() || !image.ok()) return 2;
    WorkerLaunchRequest request{
        std::wstring(*host_path),
        profile.move_value(),
        image.move_value(),
        1,
        1,
        false,
        true,
        has_option(argc, argv, L"--allow-unprotected-test-path")};
    auto worker = launch_worker(request);
    if (!worker.ok()) return 3;
    auto attachment = worker.value().attach_startup(10'000);
    if (!attachment.ok()) {
        auto worker_exit = worker.value().wait(1'000);
        if (worker_exit.ok()) {
            std::wcerr << L"engineering worker exited with "
                       << worker_exit.value() << std::endl;
        } else {
            std::wcerr << L"engineering worker remained alive after pipe "
                          L"authentication failure" << std::endl;
        }
        return 4;
    }
    const bool startup_status_returned =
        attachment.value().status.binding == worker.value().binding();
    attachment.value().connection.disconnect();
    auto exit_code = worker.value().wait(10'000);
    if (!exit_code.ok() || exit_code.value() != 0) return 5;
    std::wcout
        << L"{\"authenticated\":true,\"exit_code\":0,"
        << L"\"inherited_handle_count\":3,"
        << L"\"startup_status_returned\":"
        << (startup_status_returned ? L"true" : L"false") << L"}"
        << std::endl;
    return 0;
}
#endif

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
int diagnose_product_startup() {
    struct CaptureGuard final {
        CaptureGuard() noexcept { capture_engineering_worker_stdio = true; }
        ~CaptureGuard() noexcept { capture_engineering_worker_stdio = false; }
    } capture_guard;
    ProductServiceActions actions;
    auto started = run_service_startup(actions);
    if (!started.ok()) {
        const auto& error = started.error();
        std::wcerr << L"fatal\t" << error.operation << L"\t"
                   << error.native_code << L"\t" << error.message
                   << std::endl;
        return 3;
    }

    std::wcout << L"global\t"
               << (started.value().globally_ready ? L"ready" : L"failed")
               << std::endl;
    for (const auto& failure : started.value().profile_failures) {
        const auto profile = failure.profile_id.to_string();
        std::wcout << L"profile\t"
                   << std::wstring(profile.begin(), profile.end()) << L"\t"
                   << static_cast<unsigned>(failure.stage) << L"\t"
                   << failure.error.operation << L"\t"
                   << failure.error.native_code << L"\t"
                   << failure.error.message << std::endl;
    }

    if (!started.value().attached_profiles.empty()
        || !started.value().started_profiles.empty()) {
        auto profiles = started.value().attached_profiles;
        for (const auto& profile : started.value().started_profiles) {
            if (std::find(profiles.begin(), profiles.end(), profile)
                == profiles.end()) {
                profiles.push_back(profile);
            }
        }
        const auto deadline = actions.monotonic_time_ms()
            + kPreshutdownTimeoutMs;
        bool all_stopped = true;
        for (const auto& profile : profiles) {
            const auto text = profile.to_string();
            auto stopped = actions.stop_safely(profile, deadline);
            if (!stopped.ok()) {
                std::wcerr << L"cleanup-error\t"
                           << std::wstring(text.begin(), text.end()) << L"\t"
                           << stopped.error().operation << L"\t"
                           << stopped.error().native_code << L"\t"
                           << stopped.error().message << std::endl;
                all_stopped = false;
            } else {
                std::wcout << L"cleanup\t"
                           << std::wstring(text.begin(), text.end()) << L"\t"
                           << (stopped.value() ? L"stopped" : L"unsafe")
                           << std::endl;
                all_stopped = all_stopped && stopped.value();
            }
        }
        if (!all_stopped) return 4;
    }
    return started.value().profile_failures.empty() ? 0 : 5;
}
#endif

} // namespace

#if defined(MYRD_ENABLE_TEST_SEAMS)
bool live_reconnect_status_is_safe_for_test(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerRuntimeStatus& next) noexcept {
    return exact_live_reconnect_successor(previous, next);
}

bool live_reconnect_binding_is_safe_for_test(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerBinding& managed,
    const host::WorkerBinding& visible) noexcept {
    return safe_live_reconnect_binding(previous, managed, visible);
}

bool managed_live_status_is_safe_for_test(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerBinding& managed,
    const host::WorkerRuntimeStatus& next) noexcept {
    return safe_managed_live_status(previous, managed, next);
}

Result<std::uint32_t> scm_pending_status_pulse_count_for_test() {
    std::atomic_uint32_t count{};
    PendingStatusPulse pulse(
        [&count] {
            count.fetch_add(1, std::memory_order_acq_rel);
            return true;
        },
        std::chrono::milliseconds(1));
    if (!pulse.active()) {
        return service_error<std::uint32_t>(
            WindowsServiceError::startup_failed,
            L"windows_service.test_pending_pulse",
            L"The pending-status test pulse could not start");
    }
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (count.load(std::memory_order_acquire) < 2
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    pulse.stop();
    const auto stopped_count = count.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (pulse.publication_failed()
        || stopped_count < 2
        || count.load(std::memory_order_acquire) != stopped_count) {
        return service_error<std::uint32_t>(
            WindowsServiceError::startup_failed,
            L"windows_service.test_pending_pulse",
            L"The pending-status test pulse was not independent and bounded");
    }
    return Result<std::uint32_t>::success(stopped_count);
}

Result<std::uint32_t> scm_stop_pending_covers_prework_for_test() {
    std::atomic_uint32_t count{};
    StopPendingCoordinator coordinator;
    if (!coordinator.begin(
            [&count] {
                count.fetch_add(1, std::memory_order_acq_rel);
                return true;
            },
            std::chrono::milliseconds(1))) {
        return service_error<std::uint32_t>(
            WindowsServiceError::startup_failed,
            L"windows_service.test_stop_pending_prework",
            L"The stop-pending coordinator could not start");
    }

    const auto prework_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (count.load(std::memory_order_acquire) < 2
           && std::chrono::steady_clock::now() < prework_deadline) {
        std::this_thread::yield();
    }
    const bool publication_failed = coordinator.finish();
    const auto finished_count = count.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (publication_failed
        || finished_count < 2
        || count.load(std::memory_order_acquire) != finished_count) {
        return service_error<std::uint32_t>(
            WindowsServiceError::startup_failed,
            L"windows_service.test_stop_pending_prework",
            L"Stop-pending progress did not cover prework and finish cleanly");
    }
    return Result<std::uint32_t>::success(finished_count);
}

Result<std::uint32_t> scm_stop_request_claim_lifecycle_for_test() {
    std::atomic_uint32_t pulse_count{};
    std::atomic_uint32_t pending_requests{};
    StopPendingCoordinator coordinator;

    const auto begin_claim = [&]() {
        if (!coordinator.try_claim()) return false;
        pending_requests.fetch_add(1, std::memory_order_acq_rel);
        if (coordinator.begin(
                [&pulse_count] {
                    pulse_count.fetch_add(1, std::memory_order_acq_rel);
                    return true;
                },
                std::chrono::milliseconds(1))) {
            return true;
        }
        coordinator.release_claim();
        return false;
    };
    const auto await_pulses = [&pulse_count](const std::uint32_t target) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(1);
        while (pulse_count.load(std::memory_order_acquire) < target
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return pulse_count.load(std::memory_order_acquire) >= target;
    };
    const auto fail = [] {
        return service_error<std::uint32_t>(
            WindowsServiceError::startup_failed,
            L"windows_service.test_stop_claim",
            L"Stop request ownership was not exclusive and reusable");
    };

    if (!begin_claim() || coordinator.try_claim()
        || pending_requests.load(std::memory_order_acquire) != 1
        || !await_pulses(2)) {
        return fail();
    }
    if (coordinator.finish()) return fail();
    const auto first_finished = pulse_count.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (pulse_count.load(std::memory_order_acquire) != first_finished) {
        return fail();
    }

    coordinator.release_claim();
    if (!begin_claim() || pending_requests.load(std::memory_order_acquire) != 2
        || !await_pulses(first_finished + 2)) {
        return fail();
    }
    if (coordinator.finish()) return fail();
    const auto second_finished = pulse_count.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    coordinator.release_claim();
    if (pulse_count.load(std::memory_order_acquire) != second_finished) {
        return fail();
    }
    return Result<std::uint32_t>::success(second_finished);
}
#endif

int run_service_program(const int argc, wchar_t** argv) noexcept {
    try {
        if (argc == 1) {
            return run_dispatcher(std::wstring(kServiceName), false);
        }
        const std::wstring_view command(argv[1]);
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
        if (command == L"--engineering-contract" && argc == 2) {
            return print_contract();
        }
        if (command == L"--engineering-startup-model" && argc == 2) {
            return print_startup_model();
        }
        if (command == L"--engineering-startup-failure-model" && argc == 2) {
            return print_startup_failure_model();
        }
        if (command == L"--engineering-shutdown-model" && argc == 2) {
            return print_shutdown_model();
        }
        if (command == L"--engineering-launch-worker") {
            return launch_engineering_worker(argc, argv);
        }
        if (command == L"--engineering-authenticate-worker") {
            return authenticate_engineering_worker(argc, argv);
        }
        if (command == L"--engineering-product-startup-once" && argc == 2) {
            return diagnose_product_startup();
        }
        const auto service_name = option_value(
            argc, argv, L"--service-name");
        if (command == L"--engineering-install-service" && service_name) {
            return install_engineering_service(*service_name);
        }
        if (command == L"--engineering-query-service" && service_name) {
            return print_engineering_service(*service_name);
        }
        if (command == L"--engineering-delete-service" && service_name) {
            return delete_engineering_service(*service_name);
        }
        if (command == L"--engineering-service" && service_name
            && valid_engineering_service_name(*service_name)) {
            return run_dispatcher(std::wstring(*service_name), true);
        }
        if (command == L"--engineering-console" && argc == 3
            && std::wstring_view(argv[2]) == L"--once") {
            auto singleton = create_service_singleton(false);
            return singleton.ok() ? 0
                : static_cast<int>(WindowsServiceError::singleton_exists);
        }
#else
        static_cast<void>(command);
#endif
        return static_cast<int>(WindowsServiceError::invalid_argument);
    } catch (...) {
        return static_cast<int>(WindowsServiceError::startup_failed);
    }
}

} // namespace myrd::management::windows
