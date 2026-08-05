#include "image_paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include "src/persistence/persistence_types.h"
#include "src/persistence/native_rename_backend.h"
#include "src/persistence/native_sparse_backend.h"
#include "src/persistence/windows_file_io_bridge.h"
#include "src/persistence/writer_lock.h"
#include "src/common/scope_exit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

[[nodiscard]] Error path_error(
    const ImagePathErrorCode code,
    std::wstring operation,
    std::wstring message,
    const std::uint32_t native_code = 0) {
    return {
        ErrorDomain::storage,
        static_cast<std::uint32_t>(code),
        std::move(operation),
        std::move(message),
        native_code,
    };
}

[[nodiscard]] Result<ImageComponent> invalid_name(
    const wchar_t* message) {
    return Result<ImageComponent>::failure(path_error(
        ImagePathErrorCode::InvalidName,
        L"image_path.validate_name",
        message));
}

template <class T>
[[nodiscard]] Result<T> path_failure(
    const ImagePathErrorCode code,
    const wchar_t* operation,
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return Result<T>::failure(path_error(
        code, operation, message, native_code));
}

[[nodiscard]] Result<void> rename_commit_indeterminate(
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return Result<void>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(
            PersistenceErrorCode::CommitIndeterminate),
        L"image_path.rename_write_through",
        message,
        native_code});
}

[[nodiscard]] ImagePathErrorCode native_open_error_code(
    const std::uint32_t code) noexcept {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ImagePathErrorCode::Missing;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return ImagePathErrorCode::AccessDenied;
    default:
        return ImagePathErrorCode::IoError;
    }
}

struct NtFileRenameInformation final {
    BOOLEAN replace_if_exists;
    HANDLE root_directory;
    ULONG file_name_length;
    WCHAR file_name[1];
};

using NtSetInformationFileFunction = LONG(NTAPI*)(
    HANDLE, detail::NativeRenameIoStatus*, void*, ULONG, ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(LONG);
using BeforeNativeRenameHook = void (*)(void* context);

class WindowsNativeRenameBackend final
    : public detail::INativeRenameBackend {
public:
    WindowsNativeRenameBackend() noexcept {
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        set_information_ = reinterpret_cast<NtSetInformationFileFunction>(
            GetProcAddress(ntdll, "NtSetInformationFile"));
        status_to_dos_ = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
            GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    }

    LONG begin(
        HANDLE file,
        detail::NativeRenameIoStatus* io_status,
        void* information,
        const ULONG information_size) override {
        if (set_information_ == nullptr) {
            return static_cast<LONG>(0xC000007AUL); // STATUS_PROCEDURE_NOT_FOUND
        }
        constexpr ULONG kFileRenameInformation = 10;
        return set_information_(
            file,
            io_status,
            information,
            information_size,
            kFileRenameInformation);
    }

    DWORD wait(
        const std::span<const HANDLE> handles,
        const DWORD timeout_ms) override {
        if (handles.empty() ||
            handles.size() > static_cast<std::size_t>(MAXIMUM_WAIT_OBJECTS)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return WAIT_FAILED;
        }
        return WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()),
            handles.data(),
            FALSE,
            timeout_ms);
    }

    void cancel(HANDLE file) noexcept override {
        static_cast<void>(CancelIoEx(file, nullptr));
    }

    ULONG status_to_dos(const LONG status) noexcept override {
        return status_to_dos_ == nullptr
            ? ERROR_GEN_FAILURE
            : status_to_dos_(status);
    }

    bool flush(HANDLE file, DWORD& native_error) noexcept override {
        if (FlushFileBuffers(file)) {
            native_error = ERROR_SUCCESS;
            return true;
        }
        native_error = GetLastError();
        return false;
    }

private:
    NtSetInformationFileFunction set_information_{};
    RtlNtStatusToDosErrorFunction status_to_dos_{};
};

class WindowsNativeSparseBackend final
    : public detail::INativeSparseBackend {
public:
    bool begin(
        HANDLE file,
        FILE_SET_SPARSE_BUFFER* input,
        OVERLAPPED* overlapped,
        DWORD& native_error) noexcept override {
        DWORD ignored = 0;
        if (DeviceIoControl(
                file, FSCTL_SET_SPARSE,
                input, sizeof(*input), nullptr, 0,
                &ignored, overlapped)) {
            native_error = ERROR_SUCCESS;
            return true;
        }
        native_error = GetLastError();
        return false;
    }

    DWORD wait(
        const std::span<const HANDLE> handles,
        const DWORD timeout_ms) noexcept override {
        if (handles.empty() ||
            handles.size() > static_cast<std::size_t>(MAXIMUM_WAIT_OBJECTS)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return WAIT_FAILED;
        }
        return WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()), handles.data(),
            FALSE, timeout_ms);
    }

    void cancel(HANDLE file, OVERLAPPED* overlapped) noexcept override {
        static_cast<void>(CancelIoEx(file, overlapped));
    }

    bool complete(
        HANDLE file,
        OVERLAPPED* overlapped,
        DWORD& native_error) noexcept override {
        DWORD ignored = 0;
        if (GetOverlappedResult(file, overlapped, &ignored, FALSE)) {
            native_error = ERROR_SUCCESS;
            return true;
        }
        native_error = GetLastError();
        return false;
    }
};

struct NativeRenameOperation final {
    ~NativeRenameOperation() noexcept;

    UniqueHandle metadata_file;
    UniqueHandle root_directory;
    std::vector<std::byte> information;
    detail::NativeRenameIoStatus io_status{};
    std::shared_ptr<detail::WriterAuthorityState> writer_authority;
    bool quarantine_slot_held{};
    bool test_releasable{};
    NativeRenameOperation* quarantine_next{};
};

struct NativeSparseOperation final {
    ~NativeSparseOperation() noexcept;

    UniqueHandle file;
    UniqueHandle completion_event;
    OVERLAPPED overlapped{};
    FILE_SET_SPARSE_BUFFER input{TRUE};
    std::shared_ptr<detail::WriterAuthorityState> writer_authority;
    bool quarantine_slot_held{};
    bool test_releasable{};
    NativeSparseOperation* quarantine_next{};
};

constexpr std::size_t kMaximumNativeRenameQuarantineSlots = 8;
std::atomic<std::size_t> g_native_rename_slots{};
std::atomic<NativeRenameOperation*> g_quarantined_native_renames{};
std::mutex g_test_quarantine_mutex;
NativeRenameOperation* g_test_quarantined_native_renames{};

constexpr std::size_t kMaximumNativeSparseQuarantineSlots = 8;
std::atomic<std::size_t> g_native_sparse_slots{};
std::atomic<NativeSparseOperation*> g_quarantined_native_sparse{};
std::mutex g_test_sparse_quarantine_mutex;
NativeSparseOperation* g_test_quarantined_native_sparse{};

NativeRenameOperation::~NativeRenameOperation() noexcept {
    if (quarantine_slot_held) {
        g_native_rename_slots.fetch_sub(1, std::memory_order_acq_rel);
    }
}

[[nodiscard]] bool try_reserve_native_rename_slot(
    NativeRenameOperation& operation) noexcept {
    auto current = g_native_rename_slots.load(std::memory_order_acquire);
    while (current < kMaximumNativeRenameQuarantineSlots) {
        if (g_native_rename_slots.compare_exchange_weak(
                current,
                current + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            operation.quarantine_slot_held = true;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool try_reserve_native_sparse_slot(
    NativeSparseOperation& operation) noexcept {
    auto current = g_native_sparse_slots.load(std::memory_order_acquire);
    while (current < kMaximumNativeSparseQuarantineSlots) {
        if (g_native_sparse_slots.compare_exchange_weak(
                current, current + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            operation.quarantine_slot_held = true;
            return true;
        }
    }
    return false;
}

[[nodiscard]] detail::INativeRenameBackend& native_rename_backend() {
    static WindowsNativeRenameBackend backend;
    return backend;
}

[[nodiscard]] detail::INativeSparseBackend& native_sparse_backend() {
    static WindowsNativeSparseBackend backend;
    return backend;
}

[[nodiscard]] DWORD deadline_timeout_ms(
    const OperationContext& context) noexcept {
    constexpr DWORD kNativeRenameMaxWaitMs = 30'000;
    if (context.deadline ==
        std::chrono::steady_clock::time_point::max()) {
        return kNativeRenameMaxWaitMs;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= context.deadline) {
        return 0;
    }
    const auto remaining = context.deadline - now;
    const auto whole_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(remaining);
    auto timeout = whole_ms.count();
    if (whole_ms < remaining) {
        ++timeout;
    }
    return static_cast<DWORD>((std::min)(
        timeout,
        static_cast<decltype(timeout)>(kNativeRenameMaxWaitMs)));
}

NativeSparseOperation::~NativeSparseOperation() noexcept {
    if (quarantine_slot_held) {
        g_native_sparse_slots.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void quarantine_native_rename(
    std::unique_ptr<NativeRenameOperation> operation) noexcept {
    // If a broken native backend neither completes nor drains after cancel,
    // Windows may still own IO_STATUS_BLOCK and rename-buffer pointers. Keep
    // that state process-live instead of hanging the caller or allowing UAF.
    // Releasing ownership intentionally leaks this one indeterminate native
    // operation. Every operation reserves one of a fixed number of process
    // slots before begin(), so hostile repetition cannot create an unbounded
    // leak. The retained writer authority also prevents any successor writer
    // from overlapping this indeterminate operation.
    auto* const quarantined = operation.release();
    if (quarantined->test_releasable) {
        std::scoped_lock lock(g_test_quarantine_mutex);
        quarantined->quarantine_next =
            g_test_quarantined_native_renames;
        g_test_quarantined_native_renames = quarantined;
        return;
    }
    auto* head = g_quarantined_native_renames.load(
        std::memory_order_acquire);
    do {
        quarantined->quarantine_next = head;
    } while (!g_quarantined_native_renames.compare_exchange_weak(
        head,
        quarantined,
        std::memory_order_release,
        std::memory_order_acquire));
}

void quarantine_native_sparse(
    std::unique_ptr<NativeSparseOperation> operation) noexcept {
    auto* const quarantined = operation.release();
    if (quarantined->test_releasable) {
        std::scoped_lock lock(g_test_sparse_quarantine_mutex);
        quarantined->quarantine_next = g_test_quarantined_native_sparse;
        g_test_quarantined_native_sparse = quarantined;
        return;
    }
    auto* head = g_quarantined_native_sparse.load(
        std::memory_order_acquire);
    do {
        quarantined->quarantine_next = head;
    } while (!g_quarantined_native_sparse.compare_exchange_weak(
        head,
        quarantined,
        std::memory_order_release,
        std::memory_order_acquire));
}

thread_local BeforeNativeRenameHook g_before_native_rename_hook{};
thread_local void* g_before_native_rename_context{};
thread_local detail::INativeRenameBackend* g_native_rename_backend_override{};
thread_local detail::INativeSparseBackend* g_native_sparse_backend_override{};
thread_local bool g_write_through_rename{};

class ScopedNativeRenameHook final {
public:
    ScopedNativeRenameHook(
        BeforeNativeRenameHook hook,
        void* context) noexcept {
        g_before_native_rename_hook = hook;
        g_before_native_rename_context = context;
    }

    ~ScopedNativeRenameHook() noexcept {
        g_before_native_rename_hook = nullptr;
        g_before_native_rename_context = nullptr;
    }

    ScopedNativeRenameHook(const ScopedNativeRenameHook&) = delete;
    ScopedNativeRenameHook& operator=(const ScopedNativeRenameHook&) = delete;
};

class ScopedNativeRenameBackend final {
public:
    explicit ScopedNativeRenameBackend(
        detail::INativeRenameBackend& backend) noexcept {
        g_native_rename_backend_override = &backend;
    }

    ~ScopedNativeRenameBackend() noexcept {
        g_native_rename_backend_override = nullptr;
    }

    ScopedNativeRenameBackend(const ScopedNativeRenameBackend&) = delete;
    ScopedNativeRenameBackend& operator=(
        const ScopedNativeRenameBackend&) = delete;
};

class ScopedNativeSparseBackend final {
public:
    explicit ScopedNativeSparseBackend(
        detail::INativeSparseBackend& backend) noexcept {
        g_native_sparse_backend_override = &backend;
    }
    ~ScopedNativeSparseBackend() noexcept {
        g_native_sparse_backend_override = nullptr;
    }
    ScopedNativeSparseBackend(const ScopedNativeSparseBackend&) = delete;
    ScopedNativeSparseBackend& operator=(
        const ScopedNativeSparseBackend&) = delete;
};

class ScopedWriteThroughRename final {
public:
    ScopedWriteThroughRename() noexcept {
        g_write_through_rename = true;
    }
    ~ScopedWriteThroughRename() noexcept {
        g_write_through_rename = false;
    }
    ScopedWriteThroughRename(const ScopedWriteThroughRename&) = delete;
    ScopedWriteThroughRename& operator=(
        const ScopedWriteThroughRename&) = delete;
};

[[nodiscard]] bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool is_dos_device_name(
    const std::wstring_view name) noexcept {
    const auto dot = name.find(L'.');
    const auto base = name.substr(0, dot);
    constexpr std::array<std::wstring_view, 8> fixed = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"CONIN$", L"CONOUT$", L"CLOCK$", L"CONFIG$",
    };
    if (std::any_of(fixed.begin(), fixed.end(), [base](const auto candidate) {
            return equals_ordinal_ignore_case(base, candidate);
        })) {
        return true;
    }
    if (base.size() != 4) {
        return false;
    }
    const auto prefix = base.substr(0, 3);
    if (!equals_ordinal_ignore_case(prefix, L"COM") &&
        !equals_ordinal_ignore_case(prefix, L"LPT")) {
        return false;
    }
    const auto suffix = base[3];
    return (suffix >= L'1' && suffix <= L'9') ||
        suffix == L'\u00B9' || suffix == L'\u00B2' || suffix == L'\u00B3';
}

[[nodiscard]] bool exact_identity_match(
    const WindowsFileIdentity& left,
    const WindowsFileIdentity& right) noexcept {
    return left == right;
}

struct OpenedBundleFile final {
    UniqueHandle root_handle;
    UniqueHandle file_handle;
    WindowsFileIdentity root_identity{};
    WindowsFileIdentity file_identity{};
    std::wstring root_final_path;
    std::wstring file_final_path;
    std::string relative_name;
};

[[nodiscard]] Result<OpenedBundleFile> open_bundle_file_handles(
    const std::filesystem::path& bundle_root,
    const std::string_view relative_name,
    const std::optional<WindowsFileIdentity>& expected_identity,
    const bool mutable_access) {
    auto component = validate_image_component(relative_name);
    if (!component.ok()) {
        return Result<OpenedBundleFile>::failure(
            std::move(component.error()));
    }

    UniqueHandle root_handle(CreateFileW(
        bundle_root.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!root_handle) {
        const auto native_code = GetLastError();
        return path_failure<OpenedBundleFile>(
            native_open_error_code(native_code),
            L"image_path.open_root",
            L"opening the bundle directory failed",
            native_code);
    }

    auto root_identity = query_windows_file_identity(root_handle.get());
    if (!root_identity.ok()) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::IoError,
            L"image_path.open_root",
            L"querying the bundle directory identity failed",
            root_identity.error().native_code);
    }
    if ((root_identity.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        root_identity.value().reparse_tag != 0) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::ReparsePoint,
            L"image_path.open_root",
            L"the bundle directory itself is a reparse point");
    }
    if ((root_identity.value().attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::PathEscape,
            L"image_path.open_root",
            L"the bundle root is not a directory");
    }

    auto root_final_path = normalized_final_path(root_handle.get());
    if (!root_final_path.ok()) {
        return Result<OpenedBundleFile>::failure(
            std::move(root_final_path.error()));
    }

    try {
        std::wstring target_path = root_final_path.value();
        if (!target_path.empty() &&
            target_path.back() != L'\\' && target_path.back() != L'/') {
            target_path.push_back(L'\\');
        }
        target_path.append(component.value().wide_name);

        const DWORD access = mutable_access
            ? (GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES)
            : (GENERIC_READ | FILE_READ_ATTRIBUTES);
        const DWORD sharing = mutable_access
            ? FILE_SHARE_READ
            : (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        UniqueHandle file_handle(CreateFileW(
            target_path.c_str(),
            access,
            sharing,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
            nullptr));
        if (!file_handle) {
            const auto native_code = GetLastError();
            return path_failure<OpenedBundleFile>(
                native_open_error_code(native_code),
                L"image_path.open_target",
                L"opening the bundle file failed",
                native_code);
        }

        auto file_identity = query_windows_file_identity(file_handle.get());
        if (!file_identity.ok()) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::IoError,
                L"image_path.open_target",
                L"querying the bundle file identity failed",
                file_identity.error().native_code);
        }
        if ((file_identity.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            file_identity.value().reparse_tag != 0) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::ReparsePoint,
                L"image_path.open_target",
                L"bundle files cannot be reparse points");
        }
        if ((file_identity.value().attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::PathEscape,
                L"image_path.open_target",
                L"bundle file target is a directory");
        }
        if (file_identity.value().hard_link_count != 1) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::HardLink,
                L"image_path.open_target",
                L"bundle files must have exactly one hard link");
        }

        auto contained = is_direct_bundle_child(
            root_handle.get(), file_handle.get());
        if (!contained.ok()) {
            return Result<OpenedBundleFile>::failure(
                std::move(contained.error()));
        }
        if (!contained.value()) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::PathEscape,
                L"image_path.open_target",
                L"opened target is not a direct child of the bundle root");
        }

        if (expected_identity.has_value() &&
            !exact_identity_match(*expected_identity, file_identity.value())) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::IdentityMismatch,
                L"image_path.open_target",
                L"opened bundle file identity differs from the expected identity");
        }

        auto file_final_path = normalized_final_path(file_handle.get());
        if (!file_final_path.ok()) {
            return Result<OpenedBundleFile>::failure(
                std::move(file_final_path.error()));
        }
        return Result<OpenedBundleFile>::success({
            std::move(root_handle),
            std::move(file_handle),
            root_identity.move_value(),
            file_identity.move_value(),
            root_final_path.move_value(),
            file_final_path.move_value(),
            component.value().utf8_name,
        });
    } catch (const std::bad_alloc&) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::IoError,
            L"image_path.open_target",
            L"allocating bundle path state failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

[[nodiscard]] Result<void> verify_validated_capability_locked(
    const detail::WriterAuthorityState& writer_state,
    const std::shared_ptr<detail::WriterAuthorityState>& authority,
    const bool deleted,
    const bool poisoned,
    const bool require_cleanup_authority,
    const bool cleanup_authorized,
    HANDLE capability_root,
    HANDLE capability_file,
    const WindowsFileIdentity& capability_root_identity,
    const WindowsFileIdentity& capability_identity,
    const wchar_t* operation,
    const bool allow_writer_close_requested = false) {
    if (poisoned) {
        return path_failure<void>(
            ImagePathErrorCode::IdentityMismatch,
            operation,
            L"validated file capability is fail-closed");
    }
    if (deleted ||
        (require_cleanup_authority && !cleanup_authorized)) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            operation,
            L"operation requires a live writer and file capability");
    }
    if (authority == nullptr || authority.get() != &writer_state) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            operation,
            L"file capability belongs to a different writer lifetime");
    }

    auto writer_valid = detail::verify_writer_state_locked(
        writer_state, allow_writer_close_requested);
    if (!writer_valid.ok()) {
        return writer_valid;
    }

    auto root_identity = query_windows_file_identity(capability_root);
    auto current_identity = query_windows_file_identity(capability_file);
    if (!root_identity.ok() || !current_identity.ok()) {
        const auto native_code = !root_identity.ok()
            ? root_identity.error().native_code
            : current_identity.error().native_code;
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            operation,
            L"validated capability identity recheck failed",
            native_code);
    }
    if (!same_file_object(root_identity.value(), capability_root_identity) ||
        !same_file_object(root_identity.value(), writer_state.root_identity)) {
        return path_failure<void>(
            ImagePathErrorCode::PathEscape,
            operation,
            L"bundle root identity changed during validated operation");
    }
    if ((current_identity.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        current_identity.value().reparse_tag != 0) {
        return path_failure<void>(
            ImagePathErrorCode::ReparsePoint,
            operation,
            L"bundle file became a reparse point during validated operation");
    }
    if (current_identity.value().hard_link_count != 1) {
        return path_failure<void>(
            ImagePathErrorCode::HardLink,
            operation,
            L"bundle file gained another hard link during validated operation");
    }
    if (!exact_identity_match(current_identity.value(), capability_identity)) {
        return path_failure<void>(
            ImagePathErrorCode::IdentityMismatch,
            operation,
            L"bundle file identity changed during validated operation");
    }
    auto contained = is_direct_bundle_child(
        capability_root, capability_file);
    if (!contained.ok()) {
        return Result<void>::failure(std::move(contained.error()));
    }
    if (!contained.value()) {
        return path_failure<void>(
            ImagePathErrorCode::PathEscape,
            operation,
            L"bundle file left the bundle root during validated operation");
    }
    return Result<void>::success();
}

[[nodiscard]] Result<UniqueHandle> duplicate_capability_handle(
    HANDLE source,
    const wchar_t* operation) {
    HANDLE duplicated = INVALID_HANDLE_VALUE;
    if (source == nullptr || source == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(
            GetCurrentProcess(),
            source,
            GetCurrentProcess(),
            &duplicated,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS)) {
        const auto native_code = source == nullptr ||
                source == INVALID_HANDLE_VALUE
            ? static_cast<DWORD>(ERROR_INVALID_HANDLE)
            : GetLastError();
        return path_failure<UniqueHandle>(
            native_open_error_code(native_code),
            operation,
            L"duplicating validated capability handle failed",
            native_code);
    }
    return Result<UniqueHandle>::success(UniqueHandle(duplicated));
}

[[nodiscard]] Result<std::unique_ptr<NativeRenameOperation>>
prepare_native_rename_operation(
    HANDLE validated_file,
    HANDLE validated_root,
    const WindowsFileIdentity& expected_identity,
    const std::wstring_view new_name) {
    // ReOpenFile cannot request DELETE on this project's deliberately
    // FILE_SHARE_READ-only validated handle. Relaxing that share contract
    // would allow pathname replacement. DuplicateHandle is therefore the
    // narrow same-file-object bridge, and STATUS_PENDING is handled below.
    auto metadata_file = duplicate_capability_handle(
        validated_file, L"image_path.rename");
    if (!metadata_file.ok()) {
        return Result<std::unique_ptr<NativeRenameOperation>>::failure(
            std::move(metadata_file.error()));
    }
    auto metadata_identity = query_windows_file_identity(
        metadata_file.value().get());
    if (!metadata_identity.ok() ||
        !exact_identity_match(metadata_identity.value(), expected_identity)) {
        return path_failure<std::unique_ptr<NativeRenameOperation>>(
            ImagePathErrorCode::IdentityMismatch,
            L"image_path.rename",
            L"synchronous metadata handle does not match the validated file",
            metadata_identity.ok()
                ? 0
                : metadata_identity.error().native_code);
    }
    auto root_directory = duplicate_capability_handle(
        validated_root, L"image_path.rename");
    if (!root_directory.ok()) {
        return Result<std::unique_ptr<NativeRenameOperation>>::failure(
            std::move(root_directory.error()));
    }

    const auto name_bytes = new_name.size() * sizeof(wchar_t);
    const auto total_bytes =
        offsetof(NtFileRenameInformation, file_name) + name_bytes;
    if (name_bytes > (std::numeric_limits<ULONG>::max)() ||
        total_bytes > (std::numeric_limits<ULONG>::max)()) {
        return path_failure<std::unique_ptr<NativeRenameOperation>>(
            ImagePathErrorCode::IoError,
            L"image_path.rename",
            L"rename information exceeds the NT buffer limit",
            ERROR_ARITHMETIC_OVERFLOW);
    }

    try {
        auto operation = std::make_unique<NativeRenameOperation>();
        operation->metadata_file = metadata_file.move_value();
        operation->root_directory = root_directory.move_value();
        operation->information.resize(total_bytes);
        auto* const info = reinterpret_cast<NtFileRenameInformation*>(
            operation->information.data());
        info->replace_if_exists = FALSE;
        info->root_directory = operation->root_directory.get();
        info->file_name_length = static_cast<ULONG>(name_bytes);
        std::memcpy(info->file_name, new_name.data(), name_bytes);
        return Result<std::unique_ptr<NativeRenameOperation>>::success(
            std::move(operation));
    } catch (const std::bad_alloc&) {
        return path_failure<std::unique_ptr<NativeRenameOperation>>(
            ImagePathErrorCode::IoError,
            L"image_path.rename",
            L"allocating NT rename operation state failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

} // namespace

Result<ImageComponent> validate_image_component(
    const std::string_view utf8_name) {
    if (utf8_name.empty() || utf8_name.size() > kMaxRelativeNameBytes) {
        return invalid_name(L"image filename must contain 1..255 UTF-8 bytes");
    }
    if (utf8_name.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return invalid_name(L"image filename is too long for UTF-8 validation");
    }

    const auto byte_count = static_cast<int>(utf8_name.size());
    const auto wide_count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8_name.data(),
        byte_count,
        nullptr,
        0);
    if (wide_count <= 0) {
        return invalid_name(L"image filename is not valid UTF-8");
    }

    try {
        std::wstring wide_name(static_cast<std::size_t>(wide_count), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                utf8_name.data(),
                byte_count,
                wide_name.data(),
                wide_count) != wide_count) {
            return invalid_name(L"image filename UTF-8 conversion failed");
        }

        if (wide_name == L"." || wide_name == L".." ||
            wide_name.back() == L'.' || wide_name.back() == L' ') {
            return invalid_name(L"image filename uses a reserved component form");
        }
        // Reject '~' so caller-controlled names cannot resolve through an
        // NTFS-generated 8.3 alias before the atomic publication rename.
        constexpr std::wstring_view forbidden = L"<>:\"/\\|?*~";
        for (const auto character : wide_name) {
            if (character < L' ' || forbidden.find(character) != std::wstring_view::npos) {
                return invalid_name(L"image filename contains a forbidden character");
            }
        }
        if (is_dos_device_name(wide_name)) {
            return invalid_name(L"image filename is a reserved DOS device name");
        }

        return Result<ImageComponent>::success({
            std::string(utf8_name),
            std::move(wide_name),
        });
    } catch (const std::bad_alloc&) {
        return Result<ImageComponent>::failure(path_error(
            ImagePathErrorCode::IoError,
            L"image_path.validate_name",
            L"allocating the validated image filename failed",
            ERROR_NOT_ENOUGH_MEMORY));
    }
}

ValidatedBundleFile::ValidatedBundleFile(
    UniqueHandle root_handle,
    UniqueHandle file_handle,
    WindowsFileIdentity root_identity,
    WindowsFileIdentity identity,
    std::wstring root_final_path,
    std::wstring final_path,
    std::string relative_name,
    const bool mutable_access,
    const bool cleanup_authorized,
    const bool manifest_identity_bound) noexcept
    : root_handle_(std::move(root_handle)),
      file_handle_(std::move(file_handle)),
      root_identity_(std::move(root_identity)),
      identity_(std::move(identity)),
      root_final_path_(std::move(root_final_path)),
      final_path_(std::move(final_path)),
      relative_name_(std::move(relative_name)),
      mutable_access_(mutable_access),
      cleanup_authorized_(cleanup_authorized),
      manifest_identity_bound_(manifest_identity_bound) {}

bool ValidatedBundleFile::writer_authorized() const noexcept {
    auto authority = writer_authority_.lock();
    if (authority == nullptr) {
        return false;
    }
    std::scoped_lock lock(authority->mutex);
    return authority->active;
}

struct ValidatedPersistenceFile::Impl final {
    Impl(
        ValidatedBundleFile validated_capability,
        std::unique_ptr<IPersistenceFile> exact_file)
        : capability(std::move(validated_capability)),
          file(std::move(exact_file)) {}

    mutable std::mutex mutex;
    ValidatedBundleFile capability;
    std::unique_ptr<IPersistenceFile> file;
    bool closed{};
    bool released{};
    bool poisoned{};
};

ValidatedPersistenceFile::ValidatedPersistenceFile(
    ValidatedBundleFile capability,
    std::unique_ptr<IPersistenceFile> file)
    : impl_(std::make_unique<Impl>(
          std::move(capability), std::move(file))) {}

ValidatedPersistenceFile::~ValidatedPersistenceFile() noexcept = default;

Result<std::shared_ptr<detail::WriterAuthorityState>>
ValidatedPersistenceFile::writer_authority_for_mutation(
    const wchar_t* operation) const {
    if (impl_ == nullptr || impl_->released || impl_->file == nullptr) {
        return path_failure<std::shared_ptr<detail::WriterAuthorityState>>(
            ImagePathErrorCode::IoError,
            operation,
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    if (impl_->poisoned) {
        return path_failure<std::shared_ptr<detail::WriterAuthorityState>>(
            ImagePathErrorCode::IdentityMismatch,
            operation,
            L"validated persistence capability is fail-closed");
    }
    if (!impl_->capability.mutable_access_) {
        return path_failure<std::shared_ptr<detail::WriterAuthorityState>>(
            ImagePathErrorCode::AccessDenied,
            operation,
            L"inspection capability cannot mutate a bundle file",
            ERROR_ACCESS_DENIED);
    }
    if (impl_->closed) {
        return path_failure<std::shared_ptr<detail::WriterAuthorityState>>(
            ImagePathErrorCode::IoError,
            operation,
            L"validated persistence file is closed",
            ERROR_INVALID_HANDLE);
    }
    auto authority = impl_->capability.writer_authority_.lock();
    if (authority == nullptr) {
        return path_failure<std::shared_ptr<detail::WriterAuthorityState>>(
            ImagePathErrorCode::WriterLockInvalid,
            operation,
            L"validated mutation outlived its writer lock");
    }
    return Result<std::shared_ptr<detail::WriterAuthorityState>>::success(
        std::move(authority));
}

Result<void> ValidatedPersistenceFile::refresh_identity_after_mutation(
    const std::shared_ptr<detail::WriterAuthorityState>& authority) {
    auto refreshed = query_windows_file_identity(
        impl_->capability.file_handle_.get());
    if (!refreshed.ok()) {
        impl_->poisoned = true;
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.refresh",
            L"refreshing validated file identity after mutation failed",
            refreshed.error().native_code);
    }
    if (!same_file_object(
            impl_->capability.identity_, refreshed.value())) {
        impl_->poisoned = true;
        return path_failure<void>(
            ImagePathErrorCode::IdentityMismatch,
            L"image_path.validated_io.refresh",
            L"validated I/O no longer refers to the original file object");
    }
    if ((refreshed.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        refreshed.value().reparse_tag != 0) {
        impl_->poisoned = true;
        return path_failure<void>(
            ImagePathErrorCode::ReparsePoint,
            L"image_path.validated_io.refresh",
            L"validated file became a reparse point during mutation");
    }
    if (refreshed.value().hard_link_count != 1) {
        impl_->poisoned = true;
        return path_failure<void>(
            ImagePathErrorCode::HardLink,
            L"image_path.validated_io.refresh",
            L"validated file gained another hard link during mutation");
    }
    auto contained = is_direct_bundle_child(
        impl_->capability.root_handle_.get(),
        impl_->capability.file_handle_.get());
    if (!contained.ok() || !contained.value()) {
        impl_->poisoned = true;
        if (!contained.ok()) {
            return Result<void>::failure(std::move(contained.error()));
        }
        return path_failure<void>(
            ImagePathErrorCode::PathEscape,
            L"image_path.validated_io.refresh",
            L"validated file left the bundle during mutation");
    }
    impl_->capability.identity_ = refreshed.move_value();
    if (impl_->capability.manifest_identity_bound_) {
        authority->manifest_identity = impl_->capability.identity_;
    }
    return Result<void>::success();
}

Result<void> ValidatedPersistenceFile::read_exact(
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    return read_exact(OperationContext{}, offset, destination);
}

Result<void> ValidatedPersistenceFile::write_exact(
    const std::uint64_t offset,
    const std::span<const std::byte> source) {
    return write_exact(OperationContext{}, offset, source);
}

Result<void> ValidatedPersistenceFile::flush() {
    return flush(OperationContext{});
}

Result<std::uint64_t> ValidatedPersistenceFile::length() const {
    return length(OperationContext{});
}

Result<void> ValidatedPersistenceFile::truncate(
    const std::uint64_t new_length) {
    return truncate(OperationContext{}, new_length);
}

Result<void> ValidatedPersistenceFile::read_exact(
    const OperationContext& context,
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    if (impl_ == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.read",
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->closed || impl_->released || impl_->file == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.read",
            L"validated persistence file is closed",
            ERROR_INVALID_HANDLE);
    }
    return impl_->file->read_exact(context, offset, destination);
}

Result<void> ValidatedPersistenceFile::write_exact(
    const OperationContext& context,
    const std::uint64_t offset,
    const std::span<const std::byte> source) {
    if (impl_ == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.write",
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    std::scoped_lock operation_lock(impl_->mutex);
    auto authority = writer_authority_for_mutation(
        L"image_path.validated_io.write");
    if (!authority.ok()) {
        return Result<void>::failure(std::move(authority.error()));
    }
    std::scoped_lock writer_lock(authority.value()->mutex);
    auto capability_valid = verify_validated_capability_locked(
        *authority.value(),
        impl_->capability.writer_authority_.lock(),
        impl_->capability.deleted_,
        impl_->poisoned,
        false,
        impl_->capability.cleanup_authorized_,
        impl_->capability.root_handle_.get(),
        impl_->capability.file_handle_.get(),
        impl_->capability.root_identity_,
        impl_->capability.identity_,
        L"image_path.validated_io.write");
    if (!capability_valid.ok()) {
        return capability_valid;
    }
    auto result = impl_->file->write_exact(context, offset, source);
    auto refreshed = refresh_identity_after_mutation(authority.value());
    if (!refreshed.ok()) {
        return refreshed;
    }
    return result;
}

Result<void> ValidatedPersistenceFile::flush(
    const OperationContext& context) {
    if (impl_ == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.flush",
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    std::scoped_lock operation_lock(impl_->mutex);
    auto authority = writer_authority_for_mutation(
        L"image_path.validated_io.flush");
    if (!authority.ok()) {
        return Result<void>::failure(std::move(authority.error()));
    }
    std::scoped_lock writer_lock(authority.value()->mutex);
    auto capability_valid = verify_validated_capability_locked(
        *authority.value(),
        impl_->capability.writer_authority_.lock(),
        impl_->capability.deleted_,
        impl_->poisoned,
        false,
        impl_->capability.cleanup_authorized_,
        impl_->capability.root_handle_.get(),
        impl_->capability.file_handle_.get(),
        impl_->capability.root_identity_,
        impl_->capability.identity_,
        L"image_path.validated_io.flush");
    if (!capability_valid.ok()) {
        return capability_valid;
    }
    return impl_->file->flush(context);
}

Result<std::uint64_t> ValidatedPersistenceFile::length(
    const OperationContext& context) const {
    if (impl_ == nullptr) {
        return path_failure<std::uint64_t>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.length",
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->closed || impl_->released || impl_->file == nullptr) {
        return path_failure<std::uint64_t>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.length",
            L"validated persistence file is closed",
            ERROR_INVALID_HANDLE);
    }
    return impl_->file->length(context);
}

Result<void> ValidatedPersistenceFile::truncate(
    const OperationContext& context,
    const std::uint64_t new_length) {
    if (impl_ == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.truncate",
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    std::scoped_lock operation_lock(impl_->mutex);
    auto authority = writer_authority_for_mutation(
        L"image_path.validated_io.truncate");
    if (!authority.ok()) {
        return Result<void>::failure(std::move(authority.error()));
    }
    std::scoped_lock writer_lock(authority.value()->mutex);
    auto capability_valid = verify_validated_capability_locked(
        *authority.value(),
        impl_->capability.writer_authority_.lock(),
        impl_->capability.deleted_,
        impl_->poisoned,
        false,
        impl_->capability.cleanup_authorized_,
        impl_->capability.root_handle_.get(),
        impl_->capability.file_handle_.get(),
        impl_->capability.root_identity_,
        impl_->capability.identity_,
        L"image_path.validated_io.truncate");
    if (!capability_valid.ok()) {
        return capability_valid;
    }
    auto result = impl_->file->truncate(context, new_length);
    auto refreshed = refresh_identity_after_mutation(authority.value());
    if (!refreshed.ok()) {
        return refreshed;
    }
    return result;
}

Result<void> ValidatedPersistenceFile::close() {
    if (impl_ == nullptr) {
        return Result<void>::success();
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->closed) {
        return Result<void>::success();
    }
    if (impl_->released || impl_->file == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.close",
            L"validated persistence file was already released",
            ERROR_INVALID_HANDLE);
    }
    auto result = impl_->file->close();
    if (result.ok()) {
        impl_->closed = true;
    }
    return result;
}

WindowsFileIdentity ValidatedPersistenceFile::observed_identity() const {
    if (impl_ == nullptr) {
        return {};
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->released) {
        return {};
    }
    return impl_->capability.identity_;
}

std::wstring ValidatedPersistenceFile::observed_final_path() const {
    if (impl_ == nullptr) {
        return {};
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->released) {
        return {};
    }
    return impl_->capability.final_path_;
}

Result<WindowsFileIdentity>
ValidatedPersistenceFile::verify_mutation_capability() {
    if (impl_ == nullptr) {
        return path_failure<WindowsFileIdentity>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.verify_mutation",
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    std::scoped_lock operation_lock(impl_->mutex);
    auto authority = writer_authority_for_mutation(
        L"image_path.validated_io.verify_mutation");
    if (!authority.ok()) {
        return Result<WindowsFileIdentity>::failure(authority.error());
    }
    std::scoped_lock writer_lock(authority.value()->mutex);
    auto capability_valid = verify_validated_capability_locked(
        *authority.value(),
        impl_->capability.writer_authority_.lock(),
        impl_->capability.deleted_, impl_->poisoned, false,
        impl_->capability.cleanup_authorized_,
        impl_->capability.root_handle_.get(),
        impl_->capability.file_handle_.get(),
        impl_->capability.root_identity_,
        impl_->capability.identity_,
        L"image_path.validated_io.verify_mutation");
    if (!capability_valid.ok()) {
        return Result<WindowsFileIdentity>::failure(
            capability_valid.error());
    }
    auto refreshed = refresh_identity_after_mutation(authority.value());
    if (!refreshed.ok()) {
        return Result<WindowsFileIdentity>::failure(refreshed.error());
    }
    return Result<WindowsFileIdentity>::success(
        impl_->capability.identity_);
}

Result<ValidatedBundleFile>
ValidatedPersistenceFile::release_validated_bundle_file() {
    if (impl_ == nullptr) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.release",
            L"validated persistence file is unavailable",
            ERROR_INVALID_HANDLE);
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->closed || impl_->released || impl_->poisoned) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::AccessDenied,
            L"image_path.validated_io.release",
            L"validated capability can be released exactly once after close",
            ERROR_ACCESS_DENIED);
    }
    impl_->released = true;
    return Result<ValidatedBundleFile>::success(
        std::move(impl_->capability));
}

Result<std::wstring> normalized_final_path(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return path_failure<std::wstring>(
            ImagePathErrorCode::IoError,
            L"image_path.final_path",
            L"normalized final path requires a valid handle",
            ERROR_INVALID_HANDLE);
    }
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, kFlags);
    if (required == 0) {
        const auto native_code = GetLastError();
        return path_failure<std::wstring>(
            ImagePathErrorCode::IoError,
            L"image_path.final_path",
            L"querying normalized final path length failed",
            native_code);
    }
    constexpr DWORD kMaximumFinalPathChars = 32768;
    if (required > kMaximumFinalPathChars) {
        return path_failure<std::wstring>(
            ImagePathErrorCode::PathEscape,
            L"image_path.final_path",
            L"normalized final path exceeds the hard limit");
    }
    try {
        std::wstring path(static_cast<std::size_t>(required), L'\0');
        const auto written = GetFinalPathNameByHandleW(
            handle, path.data(), required, kFlags);
        if (written == 0 || written >= required) {
            const auto native_code = GetLastError();
            return path_failure<std::wstring>(
                ImagePathErrorCode::IoError,
                L"image_path.final_path",
                L"querying normalized final path failed",
                native_code);
        }
        path.resize(written);
        return Result<std::wstring>::success(std::move(path));
    } catch (const std::bad_alloc&) {
        return path_failure<std::wstring>(
            ImagePathErrorCode::IoError,
            L"image_path.final_path",
            L"allocating normalized final path failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<bool> is_direct_bundle_child(HANDLE bundle_root, HANDLE target) {
    auto root_path = normalized_final_path(bundle_root);
    if (!root_path.ok()) {
        return Result<bool>::failure(std::move(root_path.error()));
    }
    auto target_path = normalized_final_path(target);
    if (!target_path.ok()) {
        return Result<bool>::failure(std::move(target_path.error()));
    }
    try {
        const auto parent = std::filesystem::path(target_path.value())
            .parent_path();
        UniqueHandle parent_handle(CreateFileW(
            parent.c_str(),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!parent_handle) {
            const auto native_code = GetLastError();
            return path_failure<bool>(
                native_open_error_code(native_code),
                L"image_path.containment",
                L"opening the target parent for identity comparison failed",
                native_code);
        }
        auto root_identity = query_windows_file_identity(bundle_root);
        auto parent_identity = query_windows_file_identity(parent_handle.get());
        if (!root_identity.ok() || !parent_identity.ok()) {
            const auto native_code = !root_identity.ok()
                ? root_identity.error().native_code
                : parent_identity.error().native_code;
            return path_failure<bool>(
                ImagePathErrorCode::IoError,
                L"image_path.containment",
                L"querying parent identity for containment failed",
                native_code);
        }
        return Result<bool>::success(same_file_object(
            root_identity.value(), parent_identity.value()));
    } catch (const std::bad_alloc&) {
        return path_failure<bool>(
            ImagePathErrorCode::IoError,
            L"image_path.containment",
            L"allocating containment path state failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<ValidatedBundleFile> open_bundle_file_for_inspection(
    const std::filesystem::path& bundle_root,
    const std::string_view relative_name,
    const std::optional<WindowsFileIdentity> expected_identity) {
    auto opened = open_bundle_file_handles(
        bundle_root, relative_name, expected_identity, false);
    if (!opened.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(opened.error()));
    }
    auto value = opened.move_value();
    return Result<ValidatedBundleFile>::success(ValidatedBundleFile(
        std::move(value.root_handle),
        std::move(value.file_handle),
        std::move(value.root_identity),
        std::move(value.file_identity),
        std::move(value.root_final_path),
        std::move(value.file_final_path),
        std::move(value.relative_name),
        false,
        false));
}

Result<ValidatedBundleFile> open_bundle_file_for_mutation(
    BundleWriterLock& writer_lock,
    const std::string_view relative_name,
    const std::optional<WindowsFileIdentity> expected_identity) {
    auto writer_state = writer_lock.snapshot_state();
    if (writer_state == nullptr) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.open_mutable",
            L"mutable bundle file requires a live writer lock");
    }
    std::scoped_lock lock(writer_state->mutex);
    auto writer_valid = detail::verify_writer_state_locked(*writer_state);
    if (!writer_valid.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(writer_valid.error()));
    }
    auto opened = open_bundle_file_handles(
        writer_state->root_path,
        relative_name,
        expected_identity,
        true);
    if (!opened.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(opened.error()));
    }
    auto value = opened.move_value();
    ValidatedBundleFile capability(
        std::move(value.root_handle),
        std::move(value.file_handle),
        std::move(value.root_identity),
        std::move(value.file_identity),
        std::move(value.root_final_path),
        std::move(value.file_final_path),
        std::move(value.relative_name),
        true,
        true);
    capability.writer_authority_ = writer_state;
    return Result<ValidatedBundleFile>::success(std::move(capability));
}

Result<ValidatedBundleFile> create_bundle_file_for_mutation(
    BundleWriterLock& writer_lock,
    const std::string_view relative_name) {
    auto component = validate_image_component(relative_name);
    if (!component.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(component.error()));
    }
    auto writer_state = writer_lock.snapshot_state();
    if (writer_state == nullptr) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.create_mutable",
            L"creating a bundle file requires a live writer lock");
    }
    std::scoped_lock lock(writer_state->mutex);
    auto writer_valid = detail::verify_writer_state_locked(*writer_state);
    if (!writer_valid.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(writer_valid.error()));
    }
    try {
        std::wstring target_path = writer_state->root_final_path;
        if (!target_path.empty() &&
            target_path.back() != L'\\' && target_path.back() != L'/') {
            target_path.push_back(L'\\');
        }
        target_path.append(component.value().wide_name);
        UniqueHandle file_handle(CreateFileW(
            target_path.c_str(),
            GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
            nullptr));
        if (!file_handle) {
            const auto native_code = GetLastError();
            return path_failure<ValidatedBundleFile>(
                native_open_error_code(native_code),
                L"image_path.create_mutable",
                L"creating a unique bundle file failed",
                native_code);
        }
        auto identity = query_windows_file_identity(file_handle.get());
        if (!identity.ok()) {
            return path_failure<ValidatedBundleFile>(
                ImagePathErrorCode::IoError,
                L"image_path.create_mutable",
                L"querying the created bundle file identity failed",
                identity.error().native_code);
        }
        if ((identity.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (identity.value().attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            identity.value().reparse_tag != 0 ||
            identity.value().hard_link_count != 1) {
            return path_failure<ValidatedBundleFile>(
                ImagePathErrorCode::IdentityMismatch,
                L"image_path.create_mutable",
                L"created bundle file identity is unsafe");
        }
        auto contained = is_direct_bundle_child(
            writer_state->root_handle.get(), file_handle.get());
        if (!contained.ok()) {
            return Result<ValidatedBundleFile>::failure(
                std::move(contained.error()));
        }
        if (!contained.value()) {
            return path_failure<ValidatedBundleFile>(
                ImagePathErrorCode::PathEscape,
                L"image_path.create_mutable",
                L"created bundle file is not a direct child");
        }
        auto root_handle = duplicate_capability_handle(
            writer_state->root_handle.get(), L"image_path.create_mutable");
        if (!root_handle.ok()) {
            return Result<ValidatedBundleFile>::failure(
                std::move(root_handle.error()));
        }
        auto final_path = normalized_final_path(file_handle.get());
        if (!final_path.ok()) {
            return Result<ValidatedBundleFile>::failure(
                std::move(final_path.error()));
        }
        ValidatedBundleFile capability(
            root_handle.move_value(),
            std::move(file_handle),
            writer_state->root_identity,
            identity.move_value(),
            writer_state->root_final_path,
            final_path.move_value(),
            component.value().utf8_name,
            true,
            true);
        capability.writer_authority_ = writer_state;
        return Result<ValidatedBundleFile>::success(std::move(capability));
    } catch (const std::bad_alloc&) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::IoError,
            L"image_path.create_mutable",
            L"allocating created bundle capability failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<void> mark_validated_bundle_file_sparse(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file) {
    auto live = check_operation_context(context);
    if (!live.ok()) return live;
    auto writer_state = writer_lock.snapshot_state();
    auto authority = file.writer_authority_.lock();
    if (writer_state == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.mark_sparse",
            L"marking sparse requires a live writer lock");
    }
    std::unique_lock lock(writer_state->mutex);
    if (!file.mutable_access_) {
        return path_failure<void>(
            ImagePathErrorCode::AccessDenied,
            L"image_path.mark_sparse",
            L"read-only bundle capability cannot be marked sparse",
            ERROR_ACCESS_DENIED);
    }
    auto valid = verify_validated_capability_locked(
        *writer_state, authority, file.deleted_, file.poisoned_,
        false, file.cleanup_authorized_, file.root_handle_.get(),
        file.file_handle_.get(), file.root_identity_, file.identity_,
        L"image_path.mark_sparse");
    if (!valid.ok()) return valid;

    try {
        auto operation = std::make_unique<NativeSparseOperation>();
        auto duplicated = duplicate_capability_handle(
            file.file_handle_.get(), L"image_path.mark_sparse");
        if (!duplicated.ok()) {
            return Result<void>::failure(duplicated.error());
        }
        operation->file = duplicated.move_value();
        operation->completion_event.reset(
            CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!operation->completion_event) {
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.mark_sparse",
                L"creating sparse completion event failed",
                GetLastError());
        }
        operation->overlapped.hEvent = operation->completion_event.get();
        operation->test_releasable =
            g_native_sparse_backend_override != nullptr;
        UniqueHandle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_event) {
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.mark_sparse",
                L"creating sparse cancellation event failed",
                GetLastError());
        }
        if (!try_reserve_native_sparse_slot(*operation)) {
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.mark_sparse",
                L"native sparse quarantine capacity is exhausted",
                ERROR_NOT_ENOUGH_QUOTA);
        }
        operation->writer_authority = writer_state;
        ++writer_state->active_native_renames;
        bool native_operation_registered = true;
        ScopeExit finish_native_operation([&]() noexcept {
            if (!native_operation_registered) return;
            if (lock.owns_lock()) {
                detail::complete_native_rename_locked(
                    *writer_state, false);
                return;
            }
            std::scoped_lock state_lock(writer_state->mutex);
            detail::complete_native_rename_locked(
                *writer_state, false);
        });
        const auto quarantine_operation = [&]() noexcept {
            ++writer_state->quarantined_native_renames;
            writer_state->native_rename_condition.notify_all();
            native_operation_registered = false;
            quarantine_native_sparse(std::move(operation));
        };

        auto& backend = g_native_sparse_backend_override != nullptr
            ? *g_native_sparse_backend_override
            : native_sparse_backend();
        DWORD native_error = ERROR_SUCCESS;
        const auto synchronous = backend.begin(
            operation->file.get(), &operation->input,
            &operation->overlapped, native_error);
        if (!synchronous && native_error != ERROR_IO_PENDING) {
            return path_failure<void>(
                native_open_error_code(native_error),
                L"image_path.mark_sparse",
                L"FSCTL_SET_SPARSE failed",
                native_error);
        }
        if (!synchronous) {
            std::stop_callback callback(
                context.stop_token,
                [event = stop_event.get()]() noexcept {
                    static_cast<void>(SetEvent(event));
                });
            const std::array<HANDLE, 3> wait_handles{
                operation->completion_event.get(),
                stop_event.get(),
                writer_state->close_event.get()};
            DWORD waited = WAIT_FAILED;
            DWORD wait_error = ERROR_SUCCESS;
            bool completion_observed = false;
            bool completion_succeeded = false;
            {
                ScopeExit relock_writer([&lock]() noexcept {
                    if (!lock.owns_lock()) lock.lock();
                });
                lock.unlock();
                waited = backend.wait(
                    wait_handles, deadline_timeout_ms(context));
                if (waited == WAIT_FAILED) wait_error = GetLastError();
                if (waited == WAIT_OBJECT_0) {
                    completion_succeeded = backend.complete(
                        operation->file.get(), &operation->overlapped,
                        native_error);
                    completion_observed = completion_succeeded ||
                        native_error != ERROR_IO_INCOMPLETE;
                } else {
                    backend.cancel(
                        operation->file.get(), &operation->overlapped);
                    constexpr DWORD kCancelDrainTimeoutMs = 2000;
                    const std::array<HANDLE, 1> drain_handle{
                        operation->completion_event.get()};
                    const auto drained = backend.wait(
                        drain_handle, kCancelDrainTimeoutMs);
                    if (drained == WAIT_OBJECT_0) {
                        completion_succeeded = backend.complete(
                            operation->file.get(), &operation->overlapped,
                            native_error);
                        completion_observed = completion_succeeded ||
                            native_error != ERROR_IO_INCOMPLETE;
                    } else if (drained == WAIT_FAILED && wait_error == 0) {
                        wait_error = GetLastError();
                    }
                }
            }
            if (!completion_observed) {
                quarantine_operation();
                file.poisoned_ = true;
                return path_failure<void>(
                    ImagePathErrorCode::IoError,
                    L"image_path.mark_sparse",
                    L"native sparse completion is indeterminate",
                    ERROR_IO_INCOMPLETE);
            }
            if (!completion_succeeded) {
                if (native_error != ERROR_OPERATION_ABORTED) {
                    return path_failure<void>(
                        native_open_error_code(native_error),
                        L"image_path.mark_sparse",
                        L"pending FSCTL_SET_SPARSE failed",
                        native_error);
                }
                if (waited == WAIT_OBJECT_0 + 2) {
                    return path_failure<void>(
                        ImagePathErrorCode::WriterLockInvalid,
                        L"image_path.mark_sparse",
                        L"writer closed while sparse mark was pending");
                }
                auto interrupted = check_operation_context(context);
                if (!interrupted.ok()) return interrupted;
                return path_failure<void>(
                    ImagePathErrorCode::IoError,
                    L"image_path.mark_sparse",
                    L"native sparse mark was cancelled",
                    wait_error != 0 ? wait_error : ERROR_OPERATION_ABORTED);
            }
            valid = verify_validated_capability_locked(
                *writer_state, authority, file.deleted_, file.poisoned_,
                false, file.cleanup_authorized_, file.root_handle_.get(),
                file.file_handle_.get(), file.root_identity_, file.identity_,
                L"image_path.mark_sparse");
            if (!valid.ok()) return valid;
        }
    } catch (const std::bad_alloc&) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.mark_sparse",
            L"allocating sparse native operation failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
    auto refreshed = query_windows_file_identity(file.file_handle_.get());
    if (!refreshed.ok()) {
        file.poisoned_ = true;
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.mark_sparse",
            L"refreshing sparse file identity failed",
            refreshed.error().native_code);
    }
    file.identity_ = refreshed.move_value();
    live = check_operation_context(context);
    if (!live.ok()) return live;
    return Result<void>::success();
}

Result<ValidatedBundleFile> open_locked_manifest_for_mutation(
    BundleWriterLock& writer_lock) {
    auto writer_state = writer_lock.snapshot_state();
    if (writer_state == nullptr) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.open_manifest_mutable",
            L"manifest mutation requires a live writer lock");
    }
    std::scoped_lock lock(writer_state->mutex);
    auto writer_valid = detail::verify_writer_state_locked(
        *writer_state);
    if (!writer_valid.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(writer_valid.error()));
    }

    auto root_handle = duplicate_capability_handle(
        writer_state->root_handle.get(),
        L"image_path.open_manifest_mutable");
    if (!root_handle.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(root_handle.error()));
    }
    auto manifest_handle = duplicate_capability_handle(
        writer_state->manifest_handle.get(),
        L"image_path.open_manifest_mutable");
    if (!manifest_handle.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(manifest_handle.error()));
    }

    try {
        ValidatedBundleFile capability(
            root_handle.move_value(),
            manifest_handle.move_value(),
            writer_state->root_identity,
            writer_state->manifest_identity,
            writer_state->root_final_path,
            writer_state->manifest_final_path,
            writer_state->manifest_relative_name,
            true,
            false,
            true);
        capability.writer_authority_ = writer_state;
        return Result<ValidatedBundleFile>::success(std::move(capability));
    } catch (const std::bad_alloc&) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::IoError,
            L"image_path.open_manifest_mutable",
            L"allocating manifest capability failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<std::unique_ptr<ValidatedPersistenceFile>>
open_validated_persistence_file(ValidatedBundleFile&& capability) {
    if (!capability.file_handle_) {
        return path_failure<std::unique_ptr<ValidatedPersistenceFile>>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.open",
            L"validated file capability is closed",
            ERROR_INVALID_HANDLE);
    }
    auto file = detail::duplicate_windows_persistence_file(
        capability.file_handle_.get());
    if (!file.ok()) {
        return Result<std::unique_ptr<ValidatedPersistenceFile>>::failure(
            std::move(file.error()));
    }
    try {
        return Result<std::unique_ptr<ValidatedPersistenceFile>>::success(
            std::unique_ptr<ValidatedPersistenceFile>(
                new ValidatedPersistenceFile(
                    std::move(capability), file.move_value())));
    } catch (const std::bad_alloc&) {
        return path_failure<std::unique_ptr<ValidatedPersistenceFile>>(
            ImagePathErrorCode::IoError,
            L"image_path.validated_io.open",
            L"allocating validated persistence adapter failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<void> truncate_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::uint64_t new_length) {
    if (new_length > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.truncate",
            L"truncate length exceeds the signed Windows file limit",
            ERROR_ARITHMETIC_OVERFLOW);
    }
    auto writer_state = writer_lock.snapshot_state();
    if (writer_state == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.truncate",
            L"truncate requires a live writer lock");
    }
    std::scoped_lock lock(writer_state->mutex);
    auto allowed = verify_validated_capability_locked(
        *writer_state,
        file.writer_authority_.lock(),
        file.deleted_,
        file.poisoned_,
        true,
        file.cleanup_authorized_,
        file.root_handle_.get(),
        file.file_handle_.get(),
        file.root_identity_,
        file.identity_,
        L"image_path.truncate");
    if (!allowed.ok()) {
        return allowed;
    }
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(new_length);
    if (!SetFileInformationByHandle(
            file.file_handle_.get(), FileEndOfFileInfo, &info, sizeof(info))) {
        const auto native_code = GetLastError();
        return path_failure<void>(
            native_open_error_code(native_code),
            L"image_path.truncate",
            L"truncating the validated bundle file failed",
            native_code);
    }
    auto refreshed = query_windows_file_identity(file.file_handle_.get());
    if (!refreshed.ok()) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.truncate",
            L"refreshing identity after truncate failed",
            refreshed.error().native_code);
    }
    file.identity_ = refreshed.move_value();
    return Result<void>::success();
}

Result<void> rename_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::string_view new_relative_name) {
    OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    return rename_validated_bundle_file(
        context, writer_lock, file, new_relative_name);
}

Result<void> rename_validated_bundle_file(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::string_view new_relative_name) {
    auto context_valid = check_operation_context(context);
    if (!context_valid.ok()) {
        return context_valid;
    }
    auto component = validate_image_component(new_relative_name);
    if (!component.ok()) {
        return Result<void>::failure(std::move(component.error()));
    }
    auto writer_state = writer_lock.snapshot_state();
    if (writer_state == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.rename",
            L"rename requires a live writer lock");
    }
    std::unique_lock lock(writer_state->mutex);
    bool native_commit_observed = false;
    auto allowed = verify_validated_capability_locked(
        *writer_state,
        file.writer_authority_.lock(),
        file.deleted_,
        file.poisoned_,
        true,
        file.cleanup_authorized_,
        file.root_handle_.get(),
        file.file_handle_.get(),
        file.root_identity_,
        file.identity_,
        L"image_path.rename");
    if (!allowed.ok()) {
        return allowed;
    }

    try {
        std::wstring committed_final_path;
        std::string committed_relative_name;
        if (g_write_through_rename) {
            // Preallocate every value needed to update the capability before
            // entering the namespace commit. The handle-relative target is a
            // validated direct child of this already verified root.
            committed_final_path = file.root_final_path_;
            if (!committed_final_path.empty() &&
                committed_final_path.back() != L'\\' &&
                committed_final_path.back() != L'/') {
                committed_final_path.push_back(L'\\');
            }
            committed_final_path.append(component.value().wide_name);
            committed_relative_name = component.value().utf8_name;
        }
        // Duplicate the already-validated file object without reopening a
        // pathname. Its overlapped nature can produce STATUS_PENDING, so the
        // bounded path below owns every native buffer through drain/quarantine.
        auto prepared = prepare_native_rename_operation(
            file.file_handle_.get(),
            file.root_handle_.get(),
            file.identity_,
            component.value().wide_name);
        if (!prepared.ok()) {
            return Result<void>::failure(std::move(prepared.error()));
        }
        auto operation = prepared.move_value();
        operation->test_releasable =
            g_native_rename_backend_override != nullptr;
        UniqueHandle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_event) {
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.rename",
                L"creating the rename cancellation event failed",
                GetLastError());
        }

        context_valid = check_operation_context(context);
        if (!context_valid.ok()) {
            return context_valid;
        }
        if (!try_reserve_native_rename_slot(*operation)) {
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.rename",
                L"native rename quarantine capacity is exhausted",
                ERROR_NOT_ENOUGH_QUOTA);
        }
        operation->writer_authority = writer_state;
        ++writer_state->active_native_renames;
        bool native_rename_registered = true;
        ScopeExit finish_native_rename([&]() noexcept {
            if (!native_rename_registered) {
                return;
            }
            if (lock.owns_lock()) {
                detail::complete_native_rename_locked(
                    *writer_state, false);
                return;
            }
            std::scoped_lock state_lock(writer_state->mutex);
            detail::complete_native_rename_locked(
                *writer_state, false);
        });
        const auto quarantine_operation = [&]() noexcept {
            ++writer_state->quarantined_native_renames;
            writer_state->native_rename_condition.notify_all();
            native_rename_registered = false;
            quarantine_native_rename(std::move(operation));
        };
        if (g_before_native_rename_hook != nullptr) {
            g_before_native_rename_hook(g_before_native_rename_context);
        }

        auto& backend = g_native_rename_backend_override != nullptr
            ? *g_native_rename_backend_override
            : native_rename_backend();
        operation->io_status.status = detail::kNativeStatusPending;
        LONG status{};
        try {
            status = backend.begin(
                operation->metadata_file.get(),
                &operation->io_status,
                operation->information.data(),
                static_cast<ULONG>(operation->information.size()));
        } catch (...) {
            // A backend that throws after submitting work cannot prove that
            // Windows released the caller-owned IO_STATUS_BLOCK/buffer.
            // Retain both the operation and writer authority fail-closed.
            backend.cancel(operation->metadata_file.get());
            quarantine_operation();
            file.poisoned_ = true;
            if (g_write_through_rename) {
                return rename_commit_indeterminate(
                    L"native rename submission threw after commit may have occurred",
                    ERROR_UNHANDLED_EXCEPTION);
            }
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.rename",
                L"native rename backend threw during submission",
                ERROR_UNHANDLED_EXCEPTION);
        }
        if (status == detail::kNativeStatusPending) {
            std::stop_callback callback(
                context.stop_token,
                [event = stop_event.get()]() noexcept {
                    static_cast<void>(SetEvent(event));
                });
            const std::array<HANDLE, 3> wait_handles = {
                operation->metadata_file.get(),
                stop_event.get(),
                writer_state->close_event.get(),
            };

            DWORD waited{};
            DWORD wait_failure_code{};
            bool completion_observed{};
            {
                ScopeExit relock_writer([&lock]() noexcept {
                    if (!lock.owns_lock()) {
                        lock.lock();
                    }
                });
                lock.unlock();
                try {
                    waited = backend.wait(
                        wait_handles, deadline_timeout_ms(context));
                } catch (...) {
                    waited = WAIT_FAILED;
                    wait_failure_code = ERROR_UNHANDLED_EXCEPTION;
                }
                if (waited == WAIT_FAILED && wait_failure_code == 0) {
                    wait_failure_code = GetLastError();
                }
                if (waited == WAIT_OBJECT_0 &&
                    operation->io_status.status !=
                        detail::kNativeStatusPending) {
                    status = operation->io_status.status;
                    completion_observed = true;
                } else {
                    backend.cancel(operation->metadata_file.get());
                    constexpr DWORD kCancelDrainTimeoutMs = 2000;
                    const std::array<HANDLE, 1> drain_handle = {
                        operation->metadata_file.get(),
                    };
                    DWORD drained{};
                    try {
                        drained = backend.wait(
                            drain_handle, kCancelDrainTimeoutMs);
                    } catch (...) {
                        drained = WAIT_FAILED;
                        wait_failure_code = ERROR_UNHANDLED_EXCEPTION;
                    }
                    if (drained == WAIT_FAILED && wait_failure_code == 0) {
                        wait_failure_code = GetLastError();
                    }
                    completion_observed = drained == WAIT_OBJECT_0 &&
                        operation->io_status.status !=
                            detail::kNativeStatusPending;
                    if (completion_observed) {
                        // Cancellation is only a request. The native operation
                        // may have committed before CancelIoEx won. Classify
                        // the durable native completion, not the wake event.
                        status = operation->io_status.status;
                    }
                }
            }

            if (!completion_observed) {
                quarantine_operation();
                if (g_write_through_rename) {
                    file.poisoned_ = true;
                    return rename_commit_indeterminate(
                        L"native rename completion could not be determined",
                        ERROR_IO_INCOMPLETE);
                }
            }

            const auto native_completion_code = completion_observed
                ? backend.status_to_dos(status)
                : ERROR_IO_INCOMPLETE;
            if (completion_observed && status >= 0) {
                // A successful native status means the namespace mutation is
                // already externally visible. From this point a write-through
                // publisher may only report success or CommitIndeterminate.
                native_commit_observed = true;
            }
            if (completion_observed &&
                native_completion_code != ERROR_OPERATION_ABORTED) {
                // Success and non-cancellation native failures are handled by
                // the common post-wait path below.
            } else {
                file.poisoned_ = true;
                if (waited == WAIT_OBJECT_0 + 2) {
                    return path_failure<void>(
                        ImagePathErrorCode::WriterLockInvalid,
                        L"image_path.rename",
                        L"writer closed while native rename was pending");
                }
                if (waited == WAIT_OBJECT_0 + 1) {
                    auto cancelled = check_operation_context(context);
                    if (!cancelled.ok()) {
                        return cancelled;
                    }
                    return Result<void>::failure({
                        ErrorDomain::storage,
                        static_cast<std::uint32_t>(
                            OperationErrorCode::cancelled),
                        L"image_path.rename",
                        L"native rename was cancelled",
                    });
                }
                if (waited == WAIT_TIMEOUT) {
                    return Result<void>::failure({
                        ErrorDomain::storage,
                        static_cast<std::uint32_t>(
                            OperationErrorCode::timed_out),
                        L"image_path.rename",
                        L"native rename deadline expired",
                    });
                }
                return path_failure<void>(
                    ImagePathErrorCode::IoError,
                    L"image_path.rename",
                    L"waiting for pending native rename failed",
                    wait_failure_code != 0
                        ? wait_failure_code
                        : waited == WAIT_FAILED
                            ? ERROR_GEN_FAILURE
                        : waited == WAIT_OBJECT_0
                            ? ERROR_IO_INCOMPLETE
                            : ERROR_GEN_FAILURE);
            }
            auto still_allowed = verify_validated_capability_locked(
                *writer_state,
                file.writer_authority_.lock(),
                file.deleted_,
                file.poisoned_,
                true,
                file.cleanup_authorized_,
                file.root_handle_.get(),
                file.file_handle_.get(),
                file.root_identity_,
                file.identity_,
                L"image_path.rename",
                true);
            if (!still_allowed.ok()) {
                file.poisoned_ = true;
                if (g_write_through_rename && native_commit_observed) {
                    return rename_commit_indeterminate(
                        L"rename committed but post-commit authority recheck failed",
                        still_allowed.error().native_code);
                }
                return still_allowed;
            }
        }

        if (status < 0) {
            auto still_allowed = verify_validated_capability_locked(
                *writer_state,
                file.writer_authority_.lock(),
                file.deleted_,
                file.poisoned_,
                true,
                file.cleanup_authorized_,
                file.root_handle_.get(),
                file.file_handle_.get(),
                file.root_identity_,
                file.identity_,
                L"image_path.rename",
                true);
            if (!still_allowed.ok()) {
                file.poisoned_ = true;
                return still_allowed;
            }
            const auto native_code = backend.status_to_dos(status);
            return path_failure<void>(
                native_open_error_code(native_code),
                L"image_path.rename",
                L"NT handle-relative rename failed",
                native_code);
        }

        native_commit_observed = true;
        if (g_write_through_rename) {
            DWORD flush_error = ERROR_SUCCESS;
            if (!backend.flush(file.file_handle_.get(), flush_error)) {
                file.poisoned_ = true;
                return rename_commit_indeterminate(
                    L"rename committed but its write-through flush failed",
                    flush_error);
            }
            file.final_path_.swap(committed_final_path);
            file.relative_name_.swap(committed_relative_name);
            return Result<void>::success();
        }

        auto post_allowed = verify_validated_capability_locked(
            *writer_state,
            file.writer_authority_.lock(),
            file.deleted_,
            file.poisoned_,
            true,
            file.cleanup_authorized_,
            file.root_handle_.get(),
            file.file_handle_.get(),
            file.root_identity_,
            file.identity_,
            L"image_path.rename",
            true);
        if (!post_allowed.ok()) {
            file.poisoned_ = true;
            if (g_write_through_rename) {
                return rename_commit_indeterminate(
                    L"rename committed but post-commit authority changed",
                    post_allowed.error().native_code);
            }
            return post_allowed;
        }
        auto contained = is_direct_bundle_child(
            file.root_handle_.get(), file.file_handle_.get());
        if (!contained.ok() || !contained.value()) {
            file.poisoned_ = true;
            if (g_write_through_rename) {
                return rename_commit_indeterminate(
                    L"rename committed but containment could not be proven");
            }
            return path_failure<void>(
                ImagePathErrorCode::PathEscape,
                L"image_path.rename",
                L"renamed bundle file is no longer contained");
        }
        auto final_path = normalized_final_path(file.file_handle_.get());
        auto identity = query_windows_file_identity(file.file_handle_.get());
        if (!final_path.ok() || !identity.ok()) {
            file.poisoned_ = true;
            const auto native_code = !final_path.ok()
                ? final_path.error().native_code
                : identity.error().native_code;
            if (g_write_through_rename) {
                return rename_commit_indeterminate(
                    L"rename committed but identity refresh failed",
                    native_code);
            }
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.rename",
                L"refreshing identity after rename failed",
                native_code);
        }
        if (!same_file_object(file.identity_, identity.value())) {
            file.poisoned_ = true;
            if (g_write_through_rename) {
                return rename_commit_indeterminate(
                    L"rename committed but file identity changed");
            }
            return path_failure<void>(
                ImagePathErrorCode::IdentityMismatch,
                L"image_path.rename",
                L"rename changed the underlying file object");
        }
        file.final_path_ = final_path.move_value();
        file.identity_ = identity.move_value();
        file.relative_name_ = component.value().utf8_name;
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        if (g_write_through_rename && native_commit_observed) {
            file.poisoned_ = true;
            return rename_commit_indeterminate(
                L"rename committed but post-commit allocation failed",
                ERROR_NOT_ENOUGH_MEMORY);
        }
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.rename",
            L"allocating rename information failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<void> write_through_rename_validated_bundle_file(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::string_view new_relative_name) {
    if (g_write_through_rename) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.rename_write_through",
            L"write-through rename cannot be nested",
            ERROR_INVALID_STATE);
    }
    const ScopedWriteThroughRename write_through;
    return rename_validated_bundle_file(
        context, writer_lock, file, new_relative_name);
}

Result<void> delete_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file) {
    auto writer_state = writer_lock.snapshot_state();
    if (writer_state == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.delete",
            L"delete requires a live writer lock");
    }
    std::scoped_lock lock(writer_state->mutex);
    auto allowed = verify_validated_capability_locked(
        *writer_state,
        file.writer_authority_.lock(),
        file.deleted_,
        file.poisoned_,
        true,
        file.cleanup_authorized_,
        file.root_handle_.get(),
        file.file_handle_.get(),
        file.root_identity_,
        file.identity_,
        L"image_path.delete");
    if (!allowed.ok()) {
        return allowed;
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(
            file.file_handle_.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition))) {
        const auto native_code = GetLastError();
        return path_failure<void>(
            native_open_error_code(native_code),
            L"image_path.delete",
            L"deleting the validated bundle file failed",
            native_code);
    }
    file.deleted_ = true;
    file.file_handle_.reset();
    return Result<void>::success();
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace test {

Result<void> rename_validated_bundle_file_with_hook(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::string_view new_relative_name,
    BeforeNativeRenameHook hook,
    void* hook_context) {
    if (hook == nullptr || g_before_native_rename_hook != nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.rename_test_seam",
            L"rename test seam requires one non-nested hook",
            ERROR_INVALID_PARAMETER);
    }
    const ScopedNativeRenameHook scoped_hook(hook, hook_context);
    return rename_validated_bundle_file(
        writer_lock, file, new_relative_name);
}

Result<void> rename_validated_bundle_file_with_backend(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::string_view new_relative_name,
    detail::INativeRenameBackend& backend) {
    if (g_native_rename_backend_override != nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.rename_test_seam",
            L"rename backend test seam cannot be nested",
            ERROR_INVALID_PARAMETER);
    }
    const ScopedNativeRenameBackend scoped_backend(backend);
    return rename_validated_bundle_file(
        context, writer_lock, file, new_relative_name);
}

Result<void> write_through_rename_validated_bundle_file_with_backend(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::string_view new_relative_name,
    detail::INativeRenameBackend& backend) {
    if (g_native_rename_backend_override != nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.rename_test_seam",
            L"rename backend test seam cannot be nested",
            ERROR_INVALID_PARAMETER);
    }
    const ScopedNativeRenameBackend scoped_backend(backend);
    return write_through_rename_validated_bundle_file(
        context, writer_lock, file, new_relative_name);
}

Result<void> mark_validated_bundle_file_sparse_with_backend(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    detail::INativeSparseBackend& backend) {
    if (g_native_sparse_backend_override != nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.sparse_test_seam",
            L"sparse backend test seam cannot be nested",
            ERROR_INVALID_PARAMETER);
    }
    const ScopedNativeSparseBackend scoped_backend(backend);
    return mark_validated_bundle_file_sparse(
        context, writer_lock, file);
}

std::size_t native_rename_slots_in_use() noexcept {
    return g_native_rename_slots.load(std::memory_order_acquire);
}

std::size_t maximum_native_rename_slots() noexcept {
    return kMaximumNativeRenameQuarantineSlots;
}

bool quarantine_empty_native_rename_for_test() {
    auto operation = std::make_unique<NativeRenameOperation>();
    if (!try_reserve_native_rename_slot(*operation)) {
        return false;
    }
    operation->test_releasable = true;
    quarantine_native_rename(std::move(operation));
    return true;
}

bool quarantine_empty_native_rename_with_production_provenance_for_test() {
    auto operation = std::make_unique<NativeRenameOperation>();
    if (!try_reserve_native_rename_slot(*operation)) {
        return false;
    }
    quarantine_native_rename(std::move(operation));
    return true;
}

bool quarantine_empty_native_rename_for_writer_for_test(
    BundleWriterLock& writer) {
    auto state = writer.snapshot_state();
    if (state == nullptr) {
        return false;
    }
    auto operation = std::make_unique<NativeRenameOperation>();
    if (!try_reserve_native_rename_slot(*operation)) {
        return false;
    }
    std::scoped_lock lock(state->mutex);
    operation->writer_authority = state;
    operation->test_releasable = true;
    ++state->active_native_renames;
    ++state->quarantined_native_renames;
    quarantine_native_rename(std::move(operation));
    return true;
}

std::size_t release_quarantined_native_renames_for_test() {
    // Test fakes call this only after they have proved their simulated native
    // operation can no longer access the retained buffers. Production never
    // invokes this seam: an actually indeterminate operation remains retained
    // for process lifetime.
    NativeRenameOperation* operation{};
    {
        std::scoped_lock lock(g_test_quarantine_mutex);
        operation = g_test_quarantined_native_renames;
        g_test_quarantined_native_renames = nullptr;
    }
    std::size_t released{};
    while (operation != nullptr) {
        auto* const next = operation->quarantine_next;
        if (operation->writer_authority != nullptr) {
            auto& state = *operation->writer_authority;
            std::scoped_lock lock(state.mutex);
            detail::complete_native_rename_locked(state, true);
        }
        delete operation;
        operation = next;
        ++released;
    }
    return released;
}

std::size_t release_quarantined_native_sparse_for_test() {
    NativeSparseOperation* operation{};
    {
        std::scoped_lock lock(g_test_sparse_quarantine_mutex);
        operation = g_test_quarantined_native_sparse;
        g_test_quarantined_native_sparse = nullptr;
    }
    std::size_t released{};
    while (operation != nullptr) {
        auto* const next = operation->quarantine_next;
        if (operation->writer_authority != nullptr) {
            auto& state = *operation->writer_authority;
            std::scoped_lock lock(state.mutex);
            detail::complete_native_rename_locked(state, true);
        }
        delete operation;
        operation = next;
        ++released;
    }
    return released;
}

} // namespace test
#endif

} // namespace myrd::persistence
