#include "writer_lock.h"

#include "src/persistence/image_paths.h"

#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace myrd::persistence {
namespace {

std::mutex g_writer_registry_mutex;
std::set<std::wstring> g_writer_registry;
using AfterWriterStateSnapshotHook = void (*)(void* context);
thread_local AfterWriterStateSnapshotHook g_after_writer_snapshot_hook{};
thread_local void* g_after_writer_snapshot_context{};

[[nodiscard]] Error writer_error(
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

template <class T>
[[nodiscard]] Result<T> writer_failure(
    const ImagePathErrorCode code,
    const wchar_t* operation,
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return Result<T>::failure(writer_error(
        code, operation, message, native_code));
}

[[nodiscard]] std::wstring writer_registry_key(
    const WindowsFileIdentity& identity) {
    constexpr std::wstring_view hex = L"0123456789ABCDEF";
    std::wstring key;
    key.reserve(16 + 1 + identity.file_id.size() * 2);
    for (int shift = 60; shift >= 0; shift -= 4) {
        key.push_back(hex[static_cast<std::size_t>(
            (identity.volume_serial_number >> shift) & 0xFULL)]);
    }
    key.push_back(L':');
    for (const auto value : identity.file_id) {
        const auto byte = std::to_integer<unsigned int>(value);
        key.push_back(hex[(byte >> 4) & 0xF]);
        key.push_back(hex[byte & 0xF]);
    }
    return key;
}

[[nodiscard]] bool claim_process_writer(const std::wstring& key) {
    std::scoped_lock lock(g_writer_registry_mutex);
    return g_writer_registry.insert(key).second;
}

void release_process_writer(const std::wstring& key) noexcept {
    std::scoped_lock lock(g_writer_registry_mutex);
    g_writer_registry.erase(key);
}

[[nodiscard]] bool process_writer_is_claimed(
    const std::wstring& key) noexcept {
    std::scoped_lock lock(g_writer_registry_mutex);
    return g_writer_registry.find(key) != g_writer_registry.end();
}

[[nodiscard]] bool equal_path_ignore_case(
    const std::wstring& left,
    const std::wstring& right) noexcept {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::uint32_t release_writer_resources_locked(
    detail::WriterAuthorityState& state) noexcept {
    std::uint32_t unlock_error = 0;
    if (state.close_event) {
        static_cast<void>(SetEvent(state.close_event.get()));
    }
    if (state.byte_lock_held) {
        if (!UnlockFileEx(
                state.manifest_handle.get(),
                0,
                1,
                0,
                &state.lock_overlapped)) {
            unlock_error = GetLastError();
        }
        state.byte_lock_held = false;
    }
    if (state.registry_held) {
        release_process_writer(state.registry_key);
        state.registry_held = false;
    }
    state.active = false;
    state.manifest_handle.reset();
    state.root_handle.reset();
    return unlock_error;
}

[[nodiscard]] Result<void> release_writer_state_locked(
    detail::WriterAuthorityState& state) {
    const auto unlock_error = release_writer_resources_locked(state);
    if (unlock_error != 0) {
        return writer_failure<void>(
            ImagePathErrorCode::IoError,
            L"writer_lock.close",
            L"UnlockFileEx failed while releasing the bundle writer",
            unlock_error);
    }
    return Result<void>::success();
}

} // namespace

namespace detail {

Result<void> verify_writer_state_locked(
    const WriterAuthorityState& state,
    const bool allow_close_requested) {
    const bool close_requested = state.close_event &&
        WaitForSingleObject(state.close_event.get(), 0) != WAIT_TIMEOUT;
    if (!state.active || !state.registry_held || !state.byte_lock_held ||
        state.quarantined_native_renames != 0 ||
        !state.root_handle || !state.manifest_handle || !state.close_event ||
        (!allow_close_requested && close_requested) ||
        !process_writer_is_claimed(state.registry_key)) {
        return writer_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"writer_lock.verify",
            L"bundle writer ownership is no longer active");
    }

    auto root_identity = query_windows_file_identity(state.root_handle.get());
    auto manifest_identity = query_windows_file_identity(
        state.manifest_handle.get());
    if (!root_identity.ok() || !manifest_identity.ok()) {
        const auto native_code = !root_identity.ok()
            ? root_identity.error().native_code
            : manifest_identity.error().native_code;
        return writer_failure<void>(
            ImagePathErrorCode::IoError,
            L"writer_lock.verify",
            L"writer identity recheck failed",
            native_code);
    }
    if (!same_file_object(root_identity.value(), state.root_identity)) {
        return writer_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"writer_lock.verify",
            L"bundle root identity changed during writer ownership");
    }
    if (manifest_identity.value() != state.manifest_identity) {
        return writer_failure<void>(
            ImagePathErrorCode::IdentityMismatch,
            L"writer_lock.verify",
            L"manifest identity changed during writer ownership");
    }
    auto contained = is_direct_bundle_child(
        state.root_handle.get(), state.manifest_handle.get());
    if (!contained.ok()) {
        return Result<void>::failure(std::move(contained.error()));
    }
    if (!contained.value()) {
        return writer_failure<void>(
            ImagePathErrorCode::PathEscape,
            L"writer_lock.verify",
            L"manifest is no longer contained by the bundle root");
    }
    auto root_path = normalized_final_path(state.root_handle.get());
    auto manifest_path = normalized_final_path(state.manifest_handle.get());
    if (!root_path.ok() || !manifest_path.ok()) {
        return writer_failure<void>(
            ImagePathErrorCode::IoError,
            L"writer_lock.verify",
            L"writer final-path recheck failed",
            !root_path.ok()
                ? root_path.error().native_code
                : manifest_path.error().native_code);
    }
    if (!equal_path_ignore_case(root_path.value(), state.root_final_path) ||
        !equal_path_ignore_case(manifest_path.value(), state.manifest_final_path)) {
        return writer_failure<void>(
            ImagePathErrorCode::PathEscape,
            L"writer_lock.verify",
            L"writer final path changed during ownership");
    }
    return Result<void>::success();
}

void complete_native_rename_locked(
    WriterAuthorityState& state,
    const bool was_quarantined) noexcept {
    if (state.active_native_renames > 0) {
        --state.active_native_renames;
    }
    if (was_quarantined && state.quarantined_native_renames > 0) {
        --state.quarantined_native_renames;
    }
    state.native_rename_condition.notify_all();
    if (state.close_detached && state.active_native_renames == 0 &&
        state.quarantined_native_renames == 0) {
        static_cast<void>(release_writer_resources_locked(state));
    }
}

} // namespace detail

BundleWriterLock::BundleWriterLock(BundleWriterLock&& other) noexcept
    : state_(other.state_.exchange(nullptr, std::memory_order_acq_rel)) {}

BundleWriterLock& BundleWriterLock::operator=(
    BundleWriterLock&& other) noexcept {
    if (this != &other) {
        try {
            static_cast<void>(close());
        } catch (...) {
        }
        state_.store(
            other.state_.exchange(nullptr, std::memory_order_acq_rel),
            std::memory_order_release);
    }
    return *this;
}

BundleWriterLock::~BundleWriterLock() noexcept {
    try {
        static_cast<void>(close());
    } catch (...) {
    }
}

Result<void> BundleWriterLock::verify() const {
    auto state = snapshot_state();
    if (state == nullptr) {
        return writer_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"writer_lock.verify",
            L"bundle writer lock is closed");
    }
    if (g_after_writer_snapshot_hook != nullptr) {
        g_after_writer_snapshot_hook(g_after_writer_snapshot_context);
    }
    std::scoped_lock lock(state->mutex);
    return detail::verify_writer_state_locked(*state);
}

Result<void> BundleWriterLock::close() {
    auto state = state_.exchange(nullptr, std::memory_order_acq_rel);
    if (state == nullptr) {
        return Result<void>::success();
    }
    std::unique_lock lock(state->mutex);
    state->close_detached = true;
    if (state->close_event) {
        static_cast<void>(SetEvent(state->close_event.get()));
    }
    state->native_rename_condition.wait(lock, [&state] {
        return state->active_native_renames == 0 ||
            state->quarantined_native_renames != 0;
    });
    if (state->quarantined_native_renames != 0) {
        // A native rename still owns pointers into quarantined operation
        // storage. Retain the byte lock, registry claim and authority handles
        // so no successor writer can overlap that indeterminate operation.
        return writer_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"writer_lock.close",
            L"writer ownership is retained by an indeterminate native rename");
    }
    return release_writer_state_locked(*state);
}

WindowsFileIdentity BundleWriterLock::manifest_identity() const {
    auto state = snapshot_state();
    if (state == nullptr) {
        return {};
    }
    std::scoped_lock lock(state->mutex);
    if (!state->active || state->quarantined_native_renames != 0 ||
        !state->close_event ||
        WaitForSingleObject(state->close_event.get(), 0) != WAIT_TIMEOUT) {
        return {};
    }
    return state->manifest_identity;
}

Result<BundleWriterLock> acquire_bundle_writer_lock(
    const std::filesystem::path& bundle_root,
    const std::string_view manifest_relative_name) {
    auto opened = open_bundle_file_for_inspection(
        bundle_root, manifest_relative_name);
    if (!opened.ok()) {
        return Result<BundleWriterLock>::failure(std::move(opened.error()));
    }

    try {
        auto state = std::make_shared<detail::WriterAuthorityState>();
        state->close_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!state->close_event) {
            return writer_failure<BundleWriterLock>(
                ImagePathErrorCode::IoError,
                L"writer_lock.acquire",
                L"creating the writer-close event failed",
                GetLastError());
        }
        auto capability = opened.move_value();
        UniqueHandle writable_manifest(ReOpenFile(
            capability.file_handle_.get(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS));
        if (!writable_manifest) {
            const auto native_code = GetLastError();
            return writer_failure<BundleWriterLock>(
                native_code == ERROR_ACCESS_DENIED
                    ? ImagePathErrorCode::AccessDenied
                    : ImagePathErrorCode::IoError,
                L"writer_lock.acquire",
                L"reopening the validated manifest handle for writer I/O failed",
                native_code);
        }
        auto writable_identity = query_windows_file_identity(
            writable_manifest.get());
        if (!writable_identity.ok() || !same_file_object(
                capability.identity_, writable_identity.value())) {
            return writer_failure<BundleWriterLock>(
                ImagePathErrorCode::IdentityMismatch,
                L"writer_lock.acquire",
                L"writable manifest handle does not refer to the validated file",
                writable_identity.ok()
                    ? 0
                    : writable_identity.error().native_code);
        }
        capability.file_handle_ = std::move(writable_manifest);
        capability.identity_ = writable_identity.move_value();
        state->root_handle = std::move(capability.root_handle_);
        state->manifest_handle = std::move(capability.file_handle_);
        state->root_identity = capability.root_identity_;
        state->manifest_identity = capability.identity_;
        state->root_final_path = std::move(capability.root_final_path_);
        // Never reopen through the caller's potentially reparse-bearing path.
        // The first handle-opened, normalized final root is the authority for
        // the entire writer lifetime.
        state->root_path = std::filesystem::path(state->root_final_path);
        state->manifest_final_path = std::move(capability.final_path_);
        state->manifest_relative_name = std::move(capability.relative_name_);
        state->registry_key = writer_registry_key(state->manifest_identity);

        if (!claim_process_writer(state->registry_key)) {
            return writer_failure<BundleWriterLock>(
                ImagePathErrorCode::ImageInUse,
                L"writer_lock.acquire",
                L"another writer in this process owns the manifest");
        }
        state->registry_held = true;

        state->lock_overlapped.Offset = 0xFFFFFFFEUL;
        state->lock_overlapped.OffsetHigh = 0x7FFFFFFFUL;
        if (!LockFileEx(
                state->manifest_handle.get(),
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0,
                1,
                0,
                &state->lock_overlapped)) {
            const auto native_code = GetLastError();
            release_process_writer(state->registry_key);
            state->registry_held = false;
            if (native_code == ERROR_LOCK_VIOLATION ||
                native_code == ERROR_SHARING_VIOLATION) {
                return writer_failure<BundleWriterLock>(
                    ImagePathErrorCode::ImageInUse,
                    L"writer_lock.acquire",
                    L"another process owns the manifest writer lock",
                    native_code);
            }
            return writer_failure<BundleWriterLock>(
                ImagePathErrorCode::IoError,
                L"writer_lock.acquire",
                L"LockFileEx failed for the manifest writer lock",
                native_code);
        }
        state->byte_lock_held = true;
        state->active = true;
        return Result<BundleWriterLock>::success(
            BundleWriterLock(std::move(state)));
    } catch (const std::bad_alloc&) {
        return writer_failure<BundleWriterLock>(
            ImagePathErrorCode::IoError,
            L"writer_lock.acquire",
            L"allocating writer ownership state failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<BundleWriterLock> create_bundle_manifest_and_acquire_writer(
    const std::filesystem::path& bundle_root,
    const std::string_view manifest_relative_name) {
    auto claimed = detail_create_manifest_for_writer_acquire(
        bundle_root, manifest_relative_name);
    if (!claimed.ok()) {
        auto error = claimed.error();
        if (error.native_code == ERROR_FILE_EXISTS ||
            error.native_code == ERROR_ALREADY_EXISTS) {
            error.code = static_cast<std::uint32_t>(
                ImagePathErrorCode::ImageInUse);
            error.operation = L"writer_lock.create";
            error.message = L"the manifest name is already claimed";
        }
        return Result<BundleWriterLock>::failure(std::move(error));
    }
    const auto claimed_identity = claimed.value().identity();
    auto writer = acquire_bundle_writer_lock(
        bundle_root, manifest_relative_name);
    if (!writer.ok()) {
        return writer;
    }
    if (!same_file_object(
            claimed_identity, writer.value().manifest_identity())) {
        static_cast<void>(writer.value().close());
        return writer_failure<BundleWriterLock>(
            ImagePathErrorCode::IdentityMismatch,
            L"writer_lock.create",
            L"the claimed manifest identity changed during acquisition");
    }
    return writer;
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace test {

std::pair<std::uint64_t, std::uint64_t>
native_rename_writer_counts_for_test(const BundleWriterLock& writer) {
    auto state = writer.snapshot_state();
    if (state == nullptr) {
        return {};
    }
    std::scoped_lock lock(state->mutex);
    return {
        state->active_native_renames,
        state->quarantined_native_renames,
    };
}

Result<void> verify_bundle_writer_lock_with_snapshot_hook(
    BundleWriterLock& writer,
    AfterWriterStateSnapshotHook hook,
    void* hook_context) {
    struct ScopedHook final {
        ScopedHook(
            AfterWriterStateSnapshotHook new_hook,
            void* new_context) noexcept {
            g_after_writer_snapshot_hook = new_hook;
            g_after_writer_snapshot_context = new_context;
        }

        ~ScopedHook() noexcept {
            g_after_writer_snapshot_hook = nullptr;
            g_after_writer_snapshot_context = nullptr;
        }
    } scoped_hook(hook, hook_context);
    return writer.verify();
}

} // namespace test
#endif

} // namespace myrd::persistence
