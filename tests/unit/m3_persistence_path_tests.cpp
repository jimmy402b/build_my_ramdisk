#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include "src/persistence/file_identity.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/writer_lock.h"
#include "src/common/unique_handle.h"
#include "test_framework.h"
#include "validated_rename_test_seam.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using myrd::persistence::ImagePathErrorCode;
using myrd::persistence::WindowsFileIdentity;
using myrd::persistence::acquire_bundle_writer_lock;
using myrd::persistence::delete_validated_bundle_file;
using myrd::persistence::is_image_path_error;
using myrd::persistence::is_direct_bundle_child;
using myrd::persistence::open_bundle_file_for_inspection;
using myrd::persistence::open_bundle_file_for_mutation;
using myrd::persistence::query_windows_file_identity;
using myrd::persistence::rename_validated_bundle_file;
using myrd::persistence::truncate_validated_bundle_file;
using myrd::persistence::validate_image_component;

static_assert(std::is_same_v<
    decltype(std::declval<const myrd::persistence::BundleWriterLock&>()
        .manifest_identity()),
    WindowsFileIdentity>);

class TemporarySandbox final {
public:
    TemporarySandbox() {
        static std::atomic<std::uint64_t> sequence{};
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-task12-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(id));
        bundle_ = root_ / L"bundle";
        external_ = root_ / L"external";
        std::filesystem::create_directories(bundle_);
        std::filesystem::create_directories(external_);
    }

    ~TemporarySandbox() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TemporarySandbox(const TemporarySandbox&) = delete;
    TemporarySandbox& operator=(const TemporarySandbox&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]] const std::filesystem::path& bundle() const noexcept {
        return bundle_;
    }

    [[nodiscard]] const std::filesystem::path& external() const noexcept {
        return external_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path bundle_;
    std::filesystem::path external_;
};

class WriterLockChild final {
public:
    WriterLockChild() = default;

    ~WriterLockChild() noexcept {
        if (process_) {
            static_cast<void>(TerminateProcess(process_.get(), 127));
            static_cast<void>(WaitForSingleObject(process_.get(), 5'000));
        }
    }

    WriterLockChild(const WriterLockChild&) = delete;
    WriterLockChild& operator=(const WriterLockChild&) = delete;

    void start(
        const std::filesystem::path& bundle_root,
        const std::wstring& ready_event,
        const std::wstring& release_event) {
        std::array<wchar_t, 32768> module_path{};
        const auto module_chars = GetModuleFileNameW(
            nullptr,
            module_path.data(),
            static_cast<DWORD>(module_path.size()));
        if (module_chars == 0 || module_chars >= module_path.size()) {
            throw std::runtime_error("failed to locate test executable");
        }
        const auto helper = std::filesystem::path(
            std::wstring(module_path.data(), module_chars)).parent_path() /
            L"myrd_m3_writer_lock_helper.exe";

        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) {
            throw std::runtime_error("failed to create child cleanup job");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                job_.get(),
                JobObjectExtendedLimitInformation,
                &limits,
                sizeof(limits))) {
            throw std::runtime_error("failed to configure child cleanup job");
        }

        std::wstring command = L"\"" + helper.wstring() + L"\" \"" +
            bundle_root.wstring() + L"\" \"" + ready_event + L"\" \"" +
            release_event + L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process_info{};
        if (!CreateProcessW(
                helper.c_str(),
                mutable_command.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED,
                nullptr,
                nullptr,
                &startup,
                &process_info)) {
            throw std::runtime_error("failed to launch writer helper");
        }
        process_.reset(process_info.hProcess);
        thread_.reset(process_info.hThread);
        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            throw std::runtime_error("failed to assign writer helper job");
        }
        if (ResumeThread(thread_.get()) == static_cast<DWORD>(-1)) {
            throw std::runtime_error("failed to resume writer helper");
        }
    }

    [[nodiscard]] DWORD wait(const DWORD timeout_ms) const noexcept {
        return WaitForSingleObject(process_.get(), timeout_ms);
    }

    [[nodiscard]] DWORD exit_code() const {
        DWORD code = STILL_ACTIVE;
        if (!GetExitCodeProcess(process_.get(), &code)) {
            throw std::runtime_error("failed to query writer helper exit code");
        }
        return code;
    }

    void terminate(const DWORD code) {
        if (!TerminateProcess(process_.get(), code)) {
            throw std::runtime_error("failed to terminate writer helper");
        }
    }

    void disarm() noexcept {
        process_.reset();
        thread_.reset();
        job_.reset();
    }

private:
    myrd::UniqueHandle job_;
    myrd::UniqueHandle process_;
    myrd::UniqueHandle thread_;
};

[[nodiscard]] std::wstring unique_event_name(const wchar_t* suffix) {
    static std::atomic<std::uint64_t> sequence{};
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
    return L"Local\\myrd-task12-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(id) + L"-" + suffix;
}

void write_text(
    const std::filesystem::path& path,
    const std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("failed to create test file");
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) {
        throw std::runtime_error("failed to write test file");
    }
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    auto handle = myrd::UniqueHandle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle) {
        throw std::runtime_error("failed to open test file");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size) ||
        size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        throw std::runtime_error("invalid test file size");
    }
    std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!content.empty() &&
        (!ReadFile(
             handle.get(),
             content.data(),
             static_cast<DWORD>(content.size()),
             &read,
             nullptr) ||
         read != static_cast<DWORD>(content.size()))) {
        throw std::runtime_error("failed to read test file");
    }
    return content;
}

[[nodiscard]] myrd::UniqueHandle open_test_path(
    const std::filesystem::path& path,
    const bool directory = false) {
    return myrd::UniqueHandle(CreateFileW(
        path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL) |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
}

void enable_case_sensitive_directory(const std::filesystem::path& path) {
    auto handle = myrd::UniqueHandle(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!handle) {
        throw std::runtime_error("failed to open case-sensitive directory");
    }
    FILE_CASE_SENSITIVE_INFO info{};
    info.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    if (!SetFileInformationByHandle(
            handle.get(), FileCaseSensitiveInfo, &info, sizeof(info))) {
        throw std::runtime_error("failed to enable case-sensitive directory");
    }
}

[[nodiscard]] WindowsFileIdentity identity_of(
    const std::filesystem::path& path,
    const bool directory = false) {
    auto handle = open_test_path(path, directory);
    if (!handle) {
        throw std::runtime_error("failed to open test identity handle");
    }
    auto identity = query_windows_file_identity(handle.get());
    if (!identity.ok()) {
        throw std::runtime_error("failed to query test identity");
    }
    return identity.move_value();
}

[[nodiscard]] DWORD probe_handle_relative_rename(
    const std::filesystem::path& root,
    const std::filesystem::path& source,
    const std::wstring_view destination_name,
    const DWORD root_access) {
    auto root_handle = myrd::UniqueHandle(CreateFileW(
        root.c_str(),
        root_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!root_handle) {
        return GetLastError();
    }
    auto source_handle = myrd::UniqueHandle(CreateFileW(
        source.c_str(),
        DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!source_handle) {
        return GetLastError();
    }

    const auto name_bytes = destination_name.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* const info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = root_handle.get();
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(info->FileName, destination_name.data(), name_bytes);
    if (!SetFileInformationByHandle(
            source_handle.get(),
            FileRenameInfo,
            info,
            static_cast<DWORD>(storage.size()))) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

struct NtIoStatusBlock final {
    union {
        LONG status;
        void* pointer;
    };
    ULONG_PTR information;
};

struct NtFileRenameInformation final {
    BOOLEAN replace_if_exists;
    HANDLE root_directory;
    ULONG file_name_length;
    WCHAR file_name[1];
};

[[nodiscard]] LONG probe_nt_handle_relative_rename_handles(
    HANDLE root_handle,
    HANDLE source_handle,
    const std::wstring_view destination_name) {
    using NtSetInformationFileFunction = LONG(NTAPI*)(
        HANDLE, NtIoStatusBlock*, void*, ULONG, ULONG);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto function = reinterpret_cast<NtSetInformationFileFunction>(
        GetProcAddress(ntdll, "NtSetInformationFile"));
    if (function == nullptr) {
        return static_cast<LONG>(0xC0000139UL);
    }

    const auto name_bytes = destination_name.size() * sizeof(wchar_t);
    std::vector<std::byte> storage(
        offsetof(NtFileRenameInformation, file_name) + name_bytes);
    auto* const info = reinterpret_cast<NtFileRenameInformation*>(
        storage.data());
    info->replace_if_exists = FALSE;
    info->root_directory = root_handle;
    info->file_name_length = static_cast<ULONG>(name_bytes);
    std::memcpy(info->file_name, destination_name.data(), name_bytes);
    NtIoStatusBlock io_status{};
    constexpr ULONG kFileRenameInformation = 10;
    return function(
        source_handle,
        &io_status,
        info,
        static_cast<ULONG>(storage.size()),
        kFileRenameInformation);
}

[[nodiscard]] LONG probe_nt_handle_relative_rename(
    const std::filesystem::path& root,
    const std::filesystem::path& source,
    const std::wstring_view destination_name,
    const DWORD root_access) {
    auto root_handle = myrd::UniqueHandle(CreateFileW(
        root.c_str(),
        root_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!root_handle) {
        return static_cast<LONG>(0xC0000001UL);
    }
    auto source_handle = myrd::UniqueHandle(CreateFileW(
        source.c_str(),
        DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!source_handle) {
        return static_cast<LONG>(0xC0000001UL);
    }

    return probe_nt_handle_relative_rename_handles(
        root_handle.get(), source_handle.get(), destination_name);
}

struct MountPointReparseBuffer final {
    DWORD reparse_tag;
    WORD reparse_data_length;
    WORD reserved;
    WORD substitute_name_offset;
    WORD substitute_name_length;
    WORD print_name_offset;
    WORD print_name_length;
    WCHAR path_buffer[32768];
};

void create_directory_junction(
    const std::filesystem::path& link,
    const std::filesystem::path& target) {
    std::filesystem::create_directory(link);
    auto handle = myrd::UniqueHandle(CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!handle) {
        throw std::runtime_error("failed to open junction path");
    }

    const auto absolute_target = std::filesystem::absolute(target).wstring();
    const auto substitute_name = std::wstring(L"\\??\\") + absolute_target;
    MountPointReparseBuffer buffer{};
    buffer.reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
    const auto substitute_bytes = static_cast<WORD>(
        substitute_name.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<WORD>(
        absolute_target.size() * sizeof(wchar_t));
    buffer.substitute_name_offset = 0;
    buffer.substitute_name_length = substitute_bytes;
    buffer.print_name_offset = static_cast<WORD>(
        substitute_bytes + sizeof(wchar_t));
    buffer.print_name_length = print_bytes;
    std::memcpy(
        buffer.path_buffer,
        substitute_name.data(),
        substitute_bytes);
    std::memcpy(
        reinterpret_cast<std::byte*>(buffer.path_buffer) +
            buffer.print_name_offset,
        absolute_target.data(),
        print_bytes);
    buffer.reparse_data_length = static_cast<WORD>(
        8 + substitute_bytes + sizeof(wchar_t) +
        print_bytes + sizeof(wchar_t));

    DWORD returned = 0;
    const auto input_bytes = static_cast<DWORD>(
        8 + buffer.reparse_data_length);
    if (!DeviceIoControl(
            handle.get(),
            FSCTL_SET_REPARSE_POINT,
            &buffer,
            input_bytes,
            nullptr,
            0,
            &returned,
            nullptr)) {
        throw std::runtime_error("failed to create directory junction");
    }
}

struct RenameRouteSwap final {
    std::filesystem::path route;
    std::filesystem::path replacement_target;
    bool ran{};
};

void swap_rename_route(void* opaque) {
    auto& swap = *static_cast<RenameRouteSwap*>(opaque);
    if (!RemoveDirectoryW(swap.route.c_str())) {
        throw std::runtime_error("failed to remove rename test route");
    }
    create_directory_junction(swap.route, swap.replacement_target);
    swap.ran = true;
}

struct CloseWriterAtStateSnapshot final {
    myrd::persistence::BundleWriterLock* writer{};
    bool hook_ran{};
    bool close_ok{};
};

void close_writer_at_state_snapshot(void* opaque) {
    auto& context = *static_cast<CloseWriterAtStateSnapshot*>(opaque);
    context.hook_ran = true;
    context.close_ok = context.writer->close().ok();
}

class ScriptedPendingRenameBackend final
    : public myrd::persistence::detail::INativeRenameBackend {
public:
    enum class Mode {
        Success,
        Error,
        Deadline,
        CancelCompletionWins,
        WriterClose,
        WriterCloseCompletionWins,
        NonDrainingWriterClose,
        ThrowDuringWait,
    };

    ScriptedPendingRenameBackend(
        const Mode mode,
        myrd::persistence::BundleWriterLock& writer,
        std::stop_source* stop_source = nullptr)
        : mode_(mode), writer_(writer), stop_source_(stop_source) {}

    LONG begin(
        HANDLE file,
        myrd::persistence::detail::NativeRenameIoStatus* io_status,
        void* information,
        const ULONG information_size) override {
        ++begin_calls;
        io_status_ = io_status;
        if (mode_ == Mode::Success ||
            mode_ == Mode::CancelCompletionWins ||
            mode_ == Mode::WriterCloseCompletionWins) {
            using NtSetInformationFileFunction = LONG(NTAPI*)(
                HANDLE,
                myrd::persistence::detail::NativeRenameIoStatus*,
                void*,
                ULONG,
                ULONG);
            const auto function = reinterpret_cast<NtSetInformationFileFunction>(
                GetProcAddress(
                    GetModuleHandleW(L"ntdll.dll"),
                    "NtSetInformationFile"));
            if (function == nullptr) {
                throw std::runtime_error("NtSetInformationFile unavailable");
            }
            constexpr ULONG kFileRenameInformation = 10;
            completion_status_ = function(
                file,
                io_status,
                information,
                information_size,
                kFileRenameInformation);
            if (completion_status_ ==
                myrd::persistence::detail::kNativeStatusPending) {
                const auto waited = WaitForSingleObject(file, 2000);
                if (waited != WAIT_OBJECT_0) {
                    throw std::runtime_error(
                        "real async rename did not complete in pending seam");
                }
                completion_status_ = io_status->status;
            }
            if (completion_status_ != 0) {
                throw std::runtime_error("real rename failed in pending seam");
            }
        } else if (mode_ == Mode::Error) {
            completion_status_ = static_cast<LONG>(0xC0000022UL);
        } else if (mode_ == Mode::NonDrainingWriterClose) {
            completion_status_ =
                myrd::persistence::detail::kNativeStatusPending;
        } else {
            completion_status_ = static_cast<LONG>(0xC0000120UL);
        }
        return myrd::persistence::detail::kNativeStatusPending;
    }

    DWORD wait(
        const std::span<const HANDLE> handles,
        const DWORD) override {
        ++wait_calls;
        if (mode_ == Mode::ThrowDuringWait) {
            throw std::runtime_error("scripted pending wait failure");
        }
        if (handles.size() == 1) {
            if (mode_ == Mode::NonDrainingWriterClose) {
                return WAIT_TIMEOUT;
            }
            io_status_->status = completion_status_;
            return WAIT_OBJECT_0;
        }

        probe_writer_mutex(
            mode_ == Mode::WriterClose ||
            mode_ == Mode::WriterCloseCompletionWins ||
            mode_ == Mode::NonDrainingWriterClose);
        if (mode_ == Mode::Deadline) {
            Sleep(10);
            return WAIT_TIMEOUT;
        }
        if (mode_ == Mode::WriterClose ||
            mode_ == Mode::WriterCloseCompletionWins ||
            mode_ == Mode::NonDrainingWriterClose) {
            return WAIT_OBJECT_0 + 2;
        }
        if (mode_ == Mode::CancelCompletionWins) {
            if (stop_source_ == nullptr) {
                throw std::runtime_error(
                    "cancel-completion race requires a stop source");
            }
            stop_source_->request_stop();
            return WAIT_OBJECT_0 + 1;
        }
        io_status_->status = completion_status_;
        return WAIT_OBJECT_0;
    }

    void cancel(HANDLE) noexcept override {
        ++cancel_calls;
    }

    ULONG status_to_dos(const LONG status) noexcept override {
        if (status == static_cast<LONG>(0xC0000022UL)) {
            return ERROR_ACCESS_DENIED;
        }
        if (status == static_cast<LONG>(0xC0000120UL)) {
            return ERROR_OPERATION_ABORTED;
        }
        return status == 0 ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
    }

    std::size_t wait_calls{};
    std::size_t cancel_calls{};
    std::size_t begin_calls{};
    bool writer_mutex_available{};
    bool writer_close_completed{};

    [[nodiscard]] bool finish_writer_probe() {
        if (!probe_thread_.joinable()) {
            return false;
        }
        probe_thread_.join();
        writer_close_completed = close_writer_ &&
            probe_operation_ok_->load(std::memory_order_acquire);
        return probe_completed_->load(std::memory_order_acquire);
    }

private:
    Mode mode_;
    myrd::persistence::BundleWriterLock& writer_;
    std::stop_source* stop_source_{};
    myrd::persistence::detail::NativeRenameIoStatus* io_status_{};
    LONG completion_status_{};
    bool close_writer_{};
    std::shared_ptr<std::atomic<bool>> probe_completed_;
    std::shared_ptr<std::atomic<bool>> probe_operation_ok_;
    std::thread probe_thread_;

    void probe_writer_mutex(const bool close_writer) {
        close_writer_ = close_writer;
        probe_completed_ = std::make_shared<std::atomic<bool>>(false);
        probe_operation_ok_ = std::make_shared<std::atomic<bool>>(false);
        auto completed = probe_completed_;
        auto operation_ok = probe_operation_ok_;
        auto* const writer = &writer_;
        probe_thread_ = std::thread([=] {
            const auto result = close_writer
                ? writer->close()
                : writer->verify();
            operation_ok->store(result.ok(), std::memory_order_release);
            completed->store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);
        while (!completed->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            Sleep(1);
        }
        writer_mutex_available = completed->load(std::memory_order_acquire);
    }
};

TEST_CASE(task12_nt_relative_rename_works_when_win32_wrapper_rejects_root) {
    TemporarySandbox sandbox;
    const auto read_only_source = sandbox.bundle() / L"read-only.rdj";
    const auto add_file_source = sandbox.bundle() / L"add-file.rdj";
    const auto generic_write_source = sandbox.bundle() / L"generic-write.rdj";
    const auto nt_source = sandbox.bundle() / L"nt-relative.rdj";
    const auto nt_read_only_source = sandbox.bundle() / L"nt-read-only.rdj";
    write_text(read_only_source, "read-only");
    write_text(add_file_source, "add-file");
    write_text(generic_write_source, "generic-write");
    write_text(nt_source, "nt-relative");
    write_text(nt_read_only_source, "nt-read-only");

    const auto read_only_error = probe_handle_relative_rename(
        sandbox.bundle(),
        read_only_source,
        L"read-only-renamed.rdj",
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    REQUIRE(read_only_error != ERROR_SUCCESS);
    REQUIRE(std::filesystem::exists(read_only_source));
    REQUIRE(!std::filesystem::exists(
        sandbox.bundle() / L"read-only-renamed.rdj"));

    const auto add_file_error = probe_handle_relative_rename(
        sandbox.bundle(),
        add_file_source,
        L"add-file-renamed.rdj",
        FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES);
    const auto generic_write_error = probe_handle_relative_rename(
        sandbox.bundle(),
        generic_write_source,
        L"generic-write-renamed.rdj",
        GENERIC_READ | GENERIC_WRITE | DELETE);
    const auto nt_status = probe_nt_handle_relative_rename(
        sandbox.bundle(),
        nt_source,
        L"nt-relative-renamed.rdj",
        FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES);
    const auto nt_read_only_status = probe_nt_handle_relative_rename(
        sandbox.bundle(),
        nt_read_only_source,
        L"nt-read-only-renamed.rdj",
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    REQUIRE_EQ(add_file_error, static_cast<DWORD>(ERROR_INVALID_PARAMETER));
    REQUIRE_EQ(generic_write_error, static_cast<DWORD>(ERROR_INVALID_PARAMETER));
    REQUIRE_EQ(nt_status, static_cast<LONG>(0));
    REQUIRE_EQ(nt_read_only_status, static_cast<LONG>(0));
    REQUIRE(!std::filesystem::exists(nt_source));
    REQUIRE(!std::filesystem::exists(nt_read_only_source));
    REQUIRE_EQ(
        read_text(sandbox.bundle() / L"nt-read-only-renamed.rdj"),
        std::string("nt-read-only"));
    REQUIRE_EQ(
        read_text(sandbox.bundle() / L"nt-relative-renamed.rdj"),
        std::string("nt-relative"));
}

TEST_CASE(task12_writer_verify_survives_close_after_atomic_state_snapshot) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    write_text(manifest, std::string(8192, 'm'));

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    CloseWriterAtStateSnapshot context{&writer};

    const auto verified =
        myrd::persistence::test::verify_bundle_writer_lock_with_snapshot_hook(
            writer,
            close_writer_at_state_snapshot,
            &context);

    REQUIRE(context.hook_ran);
    REQUIRE(context.close_ok);
    REQUIRE(is_image_path_error(
        verified, ImagePathErrorCode::WriterLockInvalid));
    REQUIRE_EQ(writer.manifest_identity(), WindowsFileIdentity{});
    REQUIRE(writer.close().ok());
    auto replacement = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(replacement.ok());
}

TEST_CASE(task12_sync_metadata_reopen_is_blocked_by_validated_share_contract) {
    TemporarySandbox sandbox;
    const auto source = sandbox.bundle() / L"async-source.rdj";
    write_text(source, "same-object");
    auto async_handle = myrd::UniqueHandle(CreateFileW(
        source.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
        nullptr));
    REQUIRE(static_cast<bool>(async_handle));
    SetLastError(ERROR_SUCCESS);
    auto synchronous_handle = myrd::UniqueHandle(ReOpenFile(
        async_handle.get(),
        DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_RANDOM_ACCESS));
    REQUIRE(!static_cast<bool>(synchronous_handle));
    REQUIRE_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    REQUIRE_EQ(read_text(source), std::string("same-object"));
    REQUIRE(!std::filesystem::exists(
        sandbox.bundle() / L"sync-renamed.rdj"));
}

TEST_CASE(task12_pending_rename_success_releases_writer_mutex_while_waiting) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-success.rdj";
    const auto destination = sandbox.bundle() / L"renamed-success.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "pending-success-data");

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-success.rdj", identity_of(source));
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::Success, writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto renamed =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context, writer, capability, "renamed-success.rdj", backend);

    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{1});
    REQUIRE(backend.finish_writer_probe());
    REQUIRE(renamed.ok());
    REQUIRE(backend.writer_mutex_available);
    REQUIRE_EQ(backend.cancel_calls, std::size_t{0});
    REQUIRE(!std::filesystem::exists(source));
    REQUIRE_EQ(read_text(destination), std::string("pending-success-data"));
}

TEST_CASE(task12_pending_rename_error_releases_mutex_and_preserves_file) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-error.rdj";
    const auto destination = sandbox.bundle() / L"renamed-error.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "pending-error-data");
    const auto before_identity = identity_of(source);

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-error.rdj", before_identity);
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::Error, writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto denied =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context, writer, capability, "renamed-error.rdj", backend);

    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{1});
    REQUIRE(backend.finish_writer_probe());
    REQUIRE(is_image_path_error(denied, ImagePathErrorCode::AccessDenied));
    REQUIRE(backend.writer_mutex_available);
    REQUIRE_EQ(backend.cancel_calls, std::size_t{0});
    REQUIRE_EQ(read_text(source), std::string("pending-error-data"));
    REQUIRE(!std::filesystem::exists(destination));
    REQUIRE_EQ(identity_of(source), before_identity);
}

TEST_CASE(task12_pending_rename_deadline_cancels_drains_and_fails_closed) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-deadline.rdj";
    const auto destination = sandbox.bundle() / L"renamed-deadline.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "pending-deadline-data");
    const auto before_identity = identity_of(source);

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-deadline.rdj", before_identity);
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::Deadline, writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(5);

    const auto timed_out =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context, writer, capability, "renamed-deadline.rdj", backend);

    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{2});
    REQUIRE(backend.finish_writer_probe());
    REQUIRE(!timed_out.ok());
    REQUIRE_EQ(
        timed_out.error().code,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::timed_out));
    REQUIRE(backend.writer_mutex_available);
    REQUIRE_EQ(backend.cancel_calls, std::size_t{1});
    REQUIRE_EQ(read_text(source), std::string("pending-deadline-data"));
    REQUIRE(!std::filesystem::exists(destination));
    REQUIRE_EQ(identity_of(source), before_identity);
    const auto fail_closed = truncate_validated_bundle_file(
        writer, capability, before_identity.size);
    REQUIRE(is_image_path_error(
        fail_closed, ImagePathErrorCode::IdentityMismatch));
}

TEST_CASE(task12_pending_rename_cancel_that_loses_to_completion_reports_success) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-cancel-race.rdj";
    const auto destination =
        sandbox.bundle() / L"renamed-cancel-race.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "cancel-lost-to-completion");

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-cancel-race.rdj", identity_of(source));
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    std::stop_source stop;
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::CancelCompletionWins,
        writer,
        &stop);
    myrd::OperationContext context{};
    context.stop_token = stop.get_token();
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto completed =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context,
            writer,
            capability,
            "renamed-cancel-race.rdj",
            backend);
    REQUIRE(stop.stop_requested());
    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{2});
    REQUIRE_EQ(backend.cancel_calls, std::size_t{1});
    REQUIRE(completed.ok());
    REQUIRE(!std::filesystem::exists(source));
    REQUIRE_EQ(
        read_text(destination), std::string("cancel-lost-to-completion"));
    REQUIRE_EQ(capability.relative_name(),
               std::string("renamed-cancel-race.rdj"));
    REQUIRE(writer.verify().ok());
    REQUIRE(backend.finish_writer_probe());
}

TEST_CASE(task12_pending_rename_writer_close_cancels_drains_without_deadlock) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-close.rdj";
    const auto destination = sandbox.bundle() / L"renamed-close.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "pending-close-data");
    const auto before_identity = identity_of(source);

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-close.rdj", before_identity);
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::WriterClose, writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto closed =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context, writer, capability, "renamed-close.rdj", backend);

    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{2});
    REQUIRE(backend.finish_writer_probe());
    REQUIRE(is_image_path_error(
        closed, ImagePathErrorCode::WriterLockInvalid));
    // close() may acquire the mutex, but it must remain blocked on the
    // registered native operation until cancellation has drained.
    REQUIRE(!backend.writer_mutex_available);
    REQUIRE(backend.writer_close_completed);
    REQUIRE_EQ(backend.cancel_calls, std::size_t{1});
    REQUIRE_EQ(read_text(source), std::string("pending-close-data"));
    REQUIRE(!std::filesystem::exists(destination));
    REQUIRE_EQ(identity_of(source), before_identity);
}

TEST_CASE(task12_writer_close_that_loses_to_native_completion_reports_success) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-close-race.rdj";
    const auto destination =
        sandbox.bundle() / L"renamed-close-race.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "close-lost-to-completion");

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-close-race.rdj", identity_of(source));
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::WriterCloseCompletionWins,
        writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto completed =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context,
            writer,
            capability,
            "renamed-close-race.rdj",
            backend);

    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{2});
    REQUIRE_EQ(backend.cancel_calls, std::size_t{1});
    REQUIRE(backend.finish_writer_probe());
    REQUIRE(!backend.writer_mutex_available);
    REQUIRE(backend.writer_close_completed);
    REQUIRE(completed.ok());
    REQUIRE(!std::filesystem::exists(source));
    REQUIRE_EQ(
        read_text(destination), std::string("close-lost-to-completion"));
    REQUIRE_EQ(capability.relative_name(),
               std::string("renamed-close-race.rdj"));
    REQUIRE(is_image_path_error(
        writer.verify(), ImagePathErrorCode::WriterLockInvalid));
}

TEST_CASE(task12_non_draining_rename_retains_writer_ownership_after_close) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-quarantine.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "pending-quarantine-data");
    const auto before_identity = identity_of(source);

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-quarantine.rdj", before_identity);
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::NonDrainingWriterClose,
        writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto quarantined =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context,
            writer,
            capability,
            "renamed-quarantine.rdj",
            backend);

    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{2});
    REQUIRE_EQ(backend.cancel_calls, std::size_t{1});
    REQUIRE(backend.finish_writer_probe());
    REQUIRE(is_image_path_error(
        quarantined, ImagePathErrorCode::WriterLockInvalid));
    REQUIRE(!backend.writer_close_completed);
    auto replacement = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(!replacement.ok());
    REQUIRE(is_image_path_error(
        replacement, ImagePathErrorCode::ImageInUse));
    REQUIRE_EQ(read_text(source), std::string("pending-quarantine-data"));
    REQUIRE_EQ(identity_of(source), before_identity);
}

TEST_CASE(task12_throwing_pending_backend_is_quarantined_without_uaf) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"pending-throw.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "pending-throw-data");
    const auto before_identity = identity_of(source);
    const auto slots_before =
        myrd::persistence::test::native_rename_slots_in_use();

    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "pending-throw.rdj", before_identity);
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::ThrowDuringWait, writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto failed =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context,
            writer,
            capability,
            "renamed-throw.rdj",
            backend);

    REQUIRE(is_image_path_error(failed, ImagePathErrorCode::IoError));
    REQUIRE_EQ(failed.error().native_code,
               static_cast<std::uint32_t>(ERROR_UNHANDLED_EXCEPTION));
    REQUIRE_EQ(backend.begin_calls, std::size_t{1});
    REQUIRE_EQ(backend.wait_calls, std::size_t{2});
    REQUIRE_EQ(backend.cancel_calls, std::size_t{1});
    REQUIRE_EQ(
        myrd::persistence::test::native_rename_slots_in_use(),
        slots_before + 1);
    REQUIRE(is_image_path_error(
        writer.verify(), ImagePathErrorCode::WriterLockInvalid));
    auto replacement = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(is_image_path_error(
        replacement, ImagePathErrorCode::ImageInUse));
    REQUIRE_EQ(read_text(source), std::string("pending-throw-data"));
    REQUIRE_EQ(identity_of(source), before_identity);
}

TEST_CASE(task12_rejects_unsafe_or_non_utf8_components) {
    const std::vector<std::string> invalid_names = {
        "",
        ".",
        "..",
        "../outside.rdj",
        "..\\outside.rdj",
        "/absolute.rdj",
        "\\absolute.rdj",
        "C:relative.rdj",
        "C:\\absolute.rdj",
        "\\\\server\\share\\outside.rdj",
        "folder/file.rdj",
        "folder\\file.rdj",
        "stream.rdj:payload",
        "trailing-dot.",
        "trailing-space ",
        "bad<name.rdj",
        "bad>name.rdj",
        "bad\"name.rdj",
        "bad|name.rdj",
        "bad?name.rdj",
        "bad*name.rdj",
        std::string("bad\0name.rdj", 12),
        std::string("\xC0\xAF", 2),
        std::string("\xE2\x28\xA1", 3),
        std::string("\xF0\x28\x8C\xBC", 4),
    };

    for (const auto& name : invalid_names) {
        const auto result = validate_image_component(name);
        REQUIRE(is_image_path_error(
            result, ImagePathErrorCode::InvalidName));
    }

    const std::vector<std::string> device_names = {
        "CON", "con.rdj", "PrN.TxT", "AUX", "nul.bin",
        "COM1", "com9.rdj", "LPT1", "lPt9.anything",
        "COM\xC2\xB9.rdj", "LPT\xC2\xB2.log",
        "CONIN$", "conout$.log",
    };
    for (const auto& name : device_names) {
        const auto result = validate_image_component(name);
        REQUIRE(is_image_path_error(
            result, ImagePathErrorCode::InvalidName));
    }

    const auto too_long = std::string(256, 'a');
    const auto too_long_result = validate_image_component(too_long);
    REQUIRE(is_image_path_error(
        too_long_result, ImagePathErrorCode::InvalidName));
}

TEST_CASE(task12_accepts_bounded_unicode_and_case_variants_without_normalizing) {
    const std::vector<std::string> valid_names = {
        "disk.rdm",
        "DISK.BASE-000001.VHDX",
        "journal-000001.rdj",
        "ramdisk-\xE7\xA3\x81\xE7\x9B\x98.rdj",
        "caf\xC3\xA9.rdj",
        "cafe\xCC\x81.rdj",
        std::string(255, 'a'),
    };

    for (const auto& name : valid_names) {
        const auto result = validate_image_component(name);
        REQUIRE(result.ok());
        REQUIRE_EQ(result.value().utf8_name, name);
        REQUIRE(!result.value().wide_name.empty());
    }
}

TEST_CASE(task12_immutable_format_identities_have_exact_value_semantics) {
    std::array<std::byte, 16> first{};
    std::array<std::byte, 16> second{};
    first[0] = std::byte{0x11};
    second[0] = std::byte{0x22};

    const myrd::persistence::JournalFileIdentity journal_a{first};
    const myrd::persistence::JournalFileIdentity journal_b{first};
    const myrd::persistence::JournalFileIdentity journal_c{second};
    REQUIRE_EQ(journal_a, journal_b);
    REQUIRE(!(journal_a == journal_c));

    const myrd::persistence::VhdxFileIdentity vhdx_a{first, second};
    const myrd::persistence::VhdxFileIdentity vhdx_b{first, second};
    const myrd::persistence::VhdxFileIdentity vhdx_c{second, first};
    REQUIRE_EQ(vhdx_a, vhdx_b);
    REQUIRE(!(vhdx_a == vhdx_c));
}

TEST_CASE(task12_handle_containment_uses_exact_parent_not_sibling_prefix) {
    TemporarySandbox sandbox;
    const auto child = sandbox.bundle() / L"child.rdj";
    const auto sibling_root = sandbox.root() / L"bundle-evil";
    const auto sibling = sibling_root / L"child.rdj";
    std::filesystem::create_directory(sibling_root);
    write_text(child, "inside");
    write_text(sibling, "outside");

    auto root_handle = open_test_path(sandbox.bundle(), true);
    auto child_handle = open_test_path(child);
    auto sibling_handle = open_test_path(sibling);
    REQUIRE(root_handle);
    REQUIRE(child_handle);
    REQUIRE(sibling_handle);

    const auto child_result = is_direct_bundle_child(
        root_handle.get(), child_handle.get());
    REQUIRE(child_result.ok());
    REQUIRE(child_result.value());

    const auto sibling_result = is_direct_bundle_child(
        root_handle.get(), sibling_handle.get());
    REQUIRE(sibling_result.ok());
    REQUIRE(!sibling_result.value());
}

TEST_CASE(task12_handle_containment_uses_parent_identity_in_case_sensitive_tree) {
    TemporarySandbox sandbox;
    const auto case_parent = sandbox.root() / L"case-sensitive";
    std::filesystem::create_directory(case_parent);
    enable_case_sensitive_directory(case_parent);
    const auto root = case_parent / L"Bundle";
    const auto sibling = case_parent / L"bundle";
    std::filesystem::create_directory(root);
    std::filesystem::create_directory(sibling);
    const auto outside = sibling / L"outside.rdj";
    write_text(outside, "outside");

    auto root_handle = open_test_path(root, true);
    auto outside_handle = open_test_path(outside);
    REQUIRE(root_handle);
    REQUIRE(outside_handle);
    const auto contained = is_direct_bundle_child(
        root_handle.get(), outside_handle.get());
    REQUIRE(contained.ok());
    REQUIRE(!contained.value());
}

TEST_CASE(task12_unicode_case_open_keeps_the_same_native_identity) {
    TemporarySandbox sandbox;
    const auto actual = sandbox.bundle() / L"磁盘-Ä.RDJ";
    write_text(actual, "unicode");
    const auto expected = identity_of(actual);

    const auto opened = open_bundle_file_for_inspection(
        sandbox.bundle(),
        "\xE7\xA3\x81\xE7\x9B\x98-\xC3\xA4.rdj",
        expected);
    REQUIRE(opened.ok());
    REQUIRE_EQ(opened.value().identity(), expected);
}

TEST_CASE(task12_rejects_reparse_and_hardlink_targets_without_external_changes) {
    TemporarySandbox sandbox;
    const auto outside = sandbox.external() / L"outside.rdj";
    const auto hardlink = sandbox.bundle() / L"hardlink.rdj";
    const auto symlink = sandbox.bundle() / L"symlink.rdj";
    const auto junction = sandbox.bundle() / L"junction";
    write_text(outside, "external-payload");
    REQUIRE(CreateHardLinkW(hardlink.c_str(), outside.c_str(), nullptr));
    constexpr DWORD kAllowUnprivilegedSymlinkCreate = 0x2;
    REQUIRE(CreateSymbolicLinkW(
        symlink.c_str(),
        outside.c_str(),
        kAllowUnprivilegedSymlinkCreate));
    create_directory_junction(junction, sandbox.external());
    const auto before_identity = identity_of(outside);
    const auto before_directory_identity = identity_of(
        sandbox.external(), true);
    const auto before_content = read_text(outside);

    const auto hardlink_result = open_bundle_file_for_inspection(
        sandbox.bundle(), "hardlink.rdj");
    REQUIRE(is_image_path_error(
        hardlink_result, ImagePathErrorCode::HardLink));

    const auto symlink_result = open_bundle_file_for_inspection(
        sandbox.bundle(), "symlink.rdj");
    REQUIRE(is_image_path_error(
        symlink_result, ImagePathErrorCode::ReparsePoint));

    const auto junction_result = open_bundle_file_for_inspection(
        sandbox.bundle(), "junction");
    REQUIRE(is_image_path_error(
        junction_result, ImagePathErrorCode::ReparsePoint));

    REQUIRE_EQ(read_text(outside), before_content);
    REQUIRE_EQ(identity_of(outside), before_identity);
    REQUIRE_EQ(identity_of(sandbox.external(), true), before_directory_identity);
}

TEST_CASE(task12_identity_mismatch_is_distinct_from_missing_after_replacement) {
    TemporarySandbox sandbox;
    const auto path = sandbox.bundle() / L"replace.rdj";
    const auto old_path = sandbox.external() / L"old.rdj";
    write_text(path, "old");
    const auto original = identity_of(path);

    std::filesystem::rename(path, old_path);
    write_text(path, "new");
    const auto mismatch = open_bundle_file_for_inspection(
        sandbox.bundle(), "replace.rdj", original);
    REQUIRE(is_image_path_error(
        mismatch, ImagePathErrorCode::IdentityMismatch));

    std::filesystem::remove(path);
    const auto missing = open_bundle_file_for_inspection(
        sandbox.bundle(), "replace.rdj", original);
    REQUIRE(is_image_path_error(
        missing, ImagePathErrorCode::Missing));
    REQUIRE_EQ(read_text(old_path), std::string("old"));
}

TEST_CASE(task12_validated_io_stays_on_the_opened_object_after_path_replacement) {
    TemporarySandbox sandbox;
    const auto path = sandbox.bundle() / L"data.rdj";
    const auto moved = sandbox.external() / L"validated-object.rdj";
    write_text(path, "original");
    const auto original_identity = identity_of(path);

    auto capability_result = open_bundle_file_for_inspection(
        sandbox.bundle(), "data.rdj", original_identity);
    REQUIRE(capability_result.ok());
    auto io_result = myrd::persistence::open_validated_persistence_file(
        capability_result.move_value());
    REQUIRE(io_result.ok());
    auto io = io_result.move_value();

    REQUIRE(MoveFileExW(path.c_str(), moved.c_str(), MOVEFILE_WRITE_THROUGH));
    write_text(path, "replaced");
    REQUIRE(!myrd::persistence::same_file_object(
        identity_of(path), original_identity));

    std::array<std::byte, 8> bytes{};
    REQUIRE(io->read_exact(0, bytes).ok());
    REQUIRE_EQ(
        std::string(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        std::string("original"));
    REQUIRE_EQ(io->observed_identity(), original_identity);
    const auto denied = io->write_exact(0, std::span<const std::byte>(bytes));
    REQUIRE(is_image_path_error(denied, ImagePathErrorCode::AccessDenied));
    REQUIRE_EQ(read_text(path), std::string("replaced"));
    REQUIRE_EQ(read_text(moved), std::string("original"));

    REQUIRE(io->close().ok());
    const auto closed_write = io->write_exact(
        0, std::span<const std::byte>(bytes));
    REQUIRE(closed_write.has_error());
}

TEST_CASE(task12_validated_writer_io_refreshes_identity_for_cleanup) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto data = sandbox.bundle() / L"data.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(data, "0123");

    auto lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "data.rdj", identity_of(data));
    REQUIRE(capability_result.ok());
    auto io_result = myrd::persistence::open_validated_persistence_file(
        capability_result.move_value());
    REQUIRE(io_result.ok());
    auto io = io_result.move_value();

    const std::string suffix = "4567";
    const auto suffix_bytes = std::as_bytes(std::span(suffix));
    REQUIRE(io->write_exact(4, suffix_bytes).ok());
    REQUIRE(io->truncate(6).ok());
    REQUIRE_EQ(io->observed_identity().size, std::uint64_t{6});
    REQUIRE(io->close().ok());

    auto released_result = io->release_validated_bundle_file();
    REQUIRE(released_result.ok());
    auto released = released_result.move_value();
    REQUIRE_EQ(released.identity().size, std::uint64_t{6});
    REQUIRE(rename_validated_bundle_file(
        writer, released, "written.rdj").ok());
    REQUIRE_EQ(
        read_text(sandbox.bundle() / L"written.rdj"),
        std::string("012345"));
    REQUIRE(delete_validated_bundle_file(writer, released).ok());
}

TEST_CASE(task12_validated_write_rejects_hardlink_added_after_adapter_open) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto data = sandbox.bundle() / L"write-target.rdj";
    const auto external_alias = sandbox.external() / L"write-alias.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(data, "inside-must-not-change");

    auto lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "write-target.rdj", identity_of(data));
    REQUIRE(capability_result.ok());
    auto io_result = myrd::persistence::open_validated_persistence_file(
        capability_result.move_value());
    REQUIRE(io_result.ok());
    auto io = io_result.move_value();

    REQUIRE(CreateHardLinkW(external_alias.c_str(), data.c_str(), nullptr));
    const auto before_content = read_text(data);
    const auto before_identity = identity_of(external_alias);
    const std::array<std::byte, 4> replacement{
        std::byte{'b'}, std::byte{'a'}, std::byte{'d'}, std::byte{'!'}};
    const auto denied = io->write_exact(0, replacement);

    REQUIRE_EQ(read_text(data), before_content);
    REQUIRE_EQ(read_text(external_alias), before_content);
    REQUIRE(is_image_path_error(denied, ImagePathErrorCode::HardLink));
    REQUIRE_EQ(identity_of(data), before_identity);
    REQUIRE_EQ(identity_of(external_alias), before_identity);
}

TEST_CASE(task12_validated_truncate_rejects_hardlink_added_after_adapter_open) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto data = sandbox.bundle() / L"truncate-target.rdj";
    const auto external_alias = sandbox.external() / L"truncate-alias.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(data, "truncate-must-not-change");

    auto lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "truncate-target.rdj", identity_of(data));
    REQUIRE(capability_result.ok());
    auto io_result = myrd::persistence::open_validated_persistence_file(
        capability_result.move_value());
    REQUIRE(io_result.ok());
    auto io = io_result.move_value();

    REQUIRE(CreateHardLinkW(external_alias.c_str(), data.c_str(), nullptr));
    const auto before_content = read_text(data);
    const auto before_identity = identity_of(external_alias);
    const auto denied = io->truncate(4);

    REQUIRE_EQ(read_text(data), before_content);
    REQUIRE_EQ(read_text(external_alias), before_content);
    REQUIRE(is_image_path_error(denied, ImagePathErrorCode::HardLink));
    REQUIRE_EQ(identity_of(data), before_identity);
    REQUIRE_EQ(identity_of(external_alias), before_identity);
}

TEST_CASE(task12_locked_manifest_io_is_writable_only_for_live_writer) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    write_text(manifest, std::string(8192, 'm'));

    auto lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    auto manifest_capability =
        myrd::persistence::open_locked_manifest_for_mutation(writer);
    REQUIRE(manifest_capability.ok());
    auto io_result = myrd::persistence::open_validated_persistence_file(
        manifest_capability.move_value());
    REQUIRE(io_result.ok());
    auto io = io_result.move_value();

    const std::array<std::byte, 1> marker{std::byte{'x'}};
    REQUIRE(io->write_exact(0, marker).ok());
    std::array<std::byte, 1> read_back{};
    REQUIRE(io->read_exact(0, read_back).ok());
    REQUIRE_EQ(read_back, marker);
    REQUIRE(writer.verify().ok());

    REQUIRE(writer.close().ok());
    const auto denied = io->write_exact(1, marker);
    REQUIRE(is_image_path_error(
        denied, ImagePathErrorCode::WriterLockInvalid));
    REQUIRE_EQ(read_text(manifest).front(), 'x');
}

TEST_CASE(task12_writer_lock_allows_inspect_but_rejects_second_writer) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto replacement = sandbox.external() / L"replacement.rdm";
    write_text(manifest, std::string(8192, 'm'));
    write_text(replacement, std::string(8192, 'r'));

    auto first_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(first_result.ok());
    auto first = first_result.move_value();
    REQUIRE(first.verify().ok());

    const auto inspection = open_bundle_file_for_inspection(
        sandbox.bundle(), "disk.rdm", first.manifest_identity());
    REQUIRE(inspection.ok());

    const auto second = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(is_image_path_error(
        second, ImagePathErrorCode::ImageInUse));
    REQUIRE(first.verify().ok());

    REQUIRE(!MoveFileExW(
        replacement.c_str(),
        manifest.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    REQUIRE_EQ(identity_of(manifest), first.manifest_identity());
    REQUIRE_EQ(read_text(replacement), std::string(8192, 'r'));

    REQUIRE(first.close().ok());
    auto after_close = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(after_close.ok());
}

TEST_CASE(task12_writer_lock_releases_on_normal_and_abnormal_process_exit) {
    TemporarySandbox sandbox;
    write_text(sandbox.bundle() / L"disk.rdm", std::string(8192, 'm'));

    for (const bool abnormal_exit : {false, true}) {
        const auto ready_name = unique_event_name(L"ready");
        const auto release_name = unique_event_name(L"release");
        myrd::UniqueHandle ready(CreateEventW(
            nullptr, TRUE, FALSE, ready_name.c_str()));
        myrd::UniqueHandle release(CreateEventW(
            nullptr, TRUE, FALSE, release_name.c_str()));
        REQUIRE(ready);
        REQUIRE(release);

        WriterLockChild child;
        child.start(sandbox.bundle(), ready_name, release_name);
        REQUIRE_EQ(WaitForSingleObject(ready.get(), 5'000), WAIT_OBJECT_0);

        const auto while_held = acquire_bundle_writer_lock(
            sandbox.bundle(), "disk.rdm");
        REQUIRE(is_image_path_error(
            while_held, ImagePathErrorCode::ImageInUse));

        if (abnormal_exit) {
            child.terminate(99);
        } else {
            REQUIRE(SetEvent(release.get()));
        }
        REQUIRE_EQ(child.wait(5'000), WAIT_OBJECT_0);
        REQUIRE_EQ(child.exit_code(), abnormal_exit ? DWORD{99} : DWORD{0});
        child.disarm();

        auto after_exit = acquire_bundle_writer_lock(
            sandbox.bundle(), "disk.rdm");
        REQUIRE(after_exit.ok());
        REQUIRE(after_exit.value().verify().ok());
    }
}

TEST_CASE(task12_concurrent_writer_race_has_exactly_one_owner) {
    TemporarySandbox sandbox;
    write_text(sandbox.bundle() / L"disk.rdm", std::string(8192, 'm'));

    constexpr std::size_t kThreadCount = 8;
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t ready = 0;
    std::size_t attempted = 0;
    std::size_t owners = 0;
    bool unexpected_error = false;
    bool start = false;
    bool release = false;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (std::size_t index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&] {
            {
                std::unique_lock lock(mutex);
                ++ready;
                condition.notify_all();
                condition.wait(lock, [&] { return start; });
            }
            auto result = acquire_bundle_writer_lock(
                sandbox.bundle(), "disk.rdm");
            std::optional<myrd::persistence::BundleWriterLock> owner;
            if (result.ok()) {
                owner.emplace(result.move_value());
            } else if (!is_image_path_error(
                           result, ImagePathErrorCode::ImageInUse)) {
                std::scoped_lock lock(mutex);
                unexpected_error = true;
            }
            {
                std::unique_lock lock(mutex);
                ++attempted;
                if (owner.has_value()) {
                    ++owners;
                }
                condition.notify_all();
                if (owner.has_value()) {
                    condition.wait(lock, [&] { return release; });
                }
            }
        });
    }

    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return ready == kThreadCount; });
        start = true;
        condition.notify_all();
        const auto completed = condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return attempted == kThreadCount; });
        release = true;
        condition.notify_all();
        REQUIRE(completed);
        REQUIRE_EQ(owners, std::size_t{1});
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE(!unexpected_error);
}

TEST_CASE(task12_cleanup_rechecks_capability_identity_containment_and_lock) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto data = sandbox.bundle() / L"old.rdj";
    const auto replacement = sandbox.external() / L"replacement.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(data, "0123456789");
    write_text(replacement, "must-not-replace");

    auto lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    const auto expected_data = identity_of(data);
    {
        auto inspection_result = open_bundle_file_for_inspection(
            sandbox.bundle(), "old.rdj", expected_data);
        REQUIRE(inspection_result.ok());
        auto inspection = inspection_result.move_value();
        const auto denied = delete_validated_bundle_file(writer, inspection);
        REQUIRE(is_image_path_error(
            denied, ImagePathErrorCode::WriterLockInvalid));
        REQUIRE_EQ(read_text(data), std::string("0123456789"));
    }
    auto capability_result = open_bundle_file_for_mutation(
        writer, "old.rdj", expected_data);
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    REQUIRE(capability.writer_authorized());

    const auto replaced = MoveFileExW(
        replacement.c_str(),
        data.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    REQUIRE(!replaced);
    REQUIRE_EQ(read_text(data), std::string("0123456789"));
    REQUIRE_EQ(read_text(replacement), std::string("must-not-replace"));

    REQUIRE(truncate_validated_bundle_file(
        writer, capability, 4).ok());
    REQUIRE_EQ(read_text(data), std::string("0123"));
    const auto rename_result = rename_validated_bundle_file(
        writer, capability, "renamed.rdj");
    REQUIRE(rename_result.ok());
    REQUIRE(!std::filesystem::exists(data));
    const auto renamed = sandbox.bundle() / L"renamed.rdj";
    REQUIRE_EQ(read_text(renamed), std::string("0123"));
    REQUIRE(delete_validated_bundle_file(writer, capability).ok());
    REQUIRE(!std::filesystem::exists(renamed));
}

TEST_CASE(task12_old_capability_cannot_use_replaced_manifest_writer) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto old_manifest = sandbox.external() / L"old.rdm";
    const auto data = sandbox.bundle() / L"old.rdj";
    write_text(manifest, std::string(8192, 'a'));
    write_text(data, "original-data");

    auto old_lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(old_lock_result.ok());
    auto old_writer = old_lock_result.move_value();
    auto old_capability_result = open_bundle_file_for_mutation(
        old_writer, "old.rdj", identity_of(data));
    REQUIRE(old_capability_result.ok());
    auto old_capability = old_capability_result.move_value();
    REQUIRE(old_writer.close().ok());

    std::filesystem::rename(manifest, old_manifest);
    write_text(manifest, std::string(8192, 'b'));
    auto new_lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(new_lock_result.ok());
    auto new_writer = new_lock_result.move_value();

    const auto denied = truncate_validated_bundle_file(
        new_writer, old_capability, 1);
    REQUIRE(is_image_path_error(
        denied, ImagePathErrorCode::WriterLockInvalid));
    REQUIRE_EQ(read_text(data), std::string("original-data"));
    REQUIRE_EQ(read_text(old_manifest), std::string(8192, 'a'));
}

TEST_CASE(task12_writer_anchors_the_opened_root_across_intermediate_junction_swap) {
    TemporarySandbox sandbox;
    const auto first_parent = sandbox.external() / L"first";
    const auto second_parent = sandbox.external() / L"second";
    const auto first_bundle = first_parent / L"bundle";
    const auto second_bundle = second_parent / L"bundle";
    const auto route = sandbox.root() / L"route";
    std::filesystem::create_directories(first_bundle);
    std::filesystem::create_directories(second_bundle);
    write_text(first_bundle / L"disk.rdm", std::string(8192, 'a'));
    write_text(second_bundle / L"disk.rdm", std::string(8192, 'b'));
    write_text(first_bundle / L"owned.rdj", "first-data");
    write_text(second_bundle / L"owned.rdj", "second-data");
    const auto first_identity = identity_of(first_bundle / L"owned.rdj");
    const auto second_identity = identity_of(second_bundle / L"owned.rdj");
    REQUIRE(!myrd::persistence::same_file_object(
        first_identity, second_identity));

    create_directory_junction(route, first_parent);
    auto lock_result = acquire_bundle_writer_lock(
        route / L"bundle", "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    REQUIRE(RemoveDirectoryW(route.c_str()));
    create_directory_junction(route, second_parent);

    auto capability_result = open_bundle_file_for_mutation(
        writer, "owned.rdj");
    REQUIRE(capability_result.ok());
    REQUIRE_EQ(capability_result.value().identity(), first_identity);
    REQUIRE_EQ(read_text(first_bundle / L"owned.rdj"), std::string("first-data"));
    REQUIRE_EQ(read_text(second_bundle / L"owned.rdj"), std::string("second-data"));
}

TEST_CASE(task12_rename_is_anchored_after_final_check_before_native_call) {
    TemporarySandbox sandbox;
    const auto first_parent = sandbox.external() / L"rename-first";
    const auto second_parent = sandbox.external() / L"rename-second";
    const auto first_bundle = first_parent / L"bundle";
    const auto second_bundle = second_parent / L"bundle";
    const auto route = sandbox.root() / L"rename-route";
    std::filesystem::create_directories(first_bundle);
    std::filesystem::create_directories(second_bundle);
    const auto first_manifest = first_bundle / L"disk.rdm";
    const auto second_manifest = second_bundle / L"disk.rdm";
    const auto first_data = first_bundle / L"owned.rdj";
    const auto second_data = second_bundle / L"owned.rdj";
    write_text(first_manifest, std::string(8192, 'a'));
    write_text(second_manifest, std::string(8192, 'b'));
    write_text(first_data, "first-owned");
    write_text(second_data, "external-must-not-change");
    const auto external_manifest_identity = identity_of(second_manifest);
    const auto external_data_identity = identity_of(second_data);
    const auto external_manifest_content = read_text(second_manifest);
    const auto external_data_content = read_text(second_data);

    create_directory_junction(route, first_parent);
    auto lock_result = acquire_bundle_writer_lock(
        route / L"bundle", "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "owned.rdj", identity_of(first_data));
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    RenameRouteSwap swap{route, second_parent};

    const auto renamed =
        myrd::persistence::test::rename_validated_bundle_file_with_hook(
            writer,
            capability,
            "renamed.rdj",
            swap_rename_route,
            &swap);
    REQUIRE(renamed.ok());
    REQUIRE(swap.ran);
    REQUIRE(!std::filesystem::exists(first_data));
    REQUIRE_EQ(
        read_text(first_bundle / L"renamed.rdj"),
        std::string("first-owned"));
    REQUIRE_EQ(read_text(second_manifest), external_manifest_content);
    REQUIRE_EQ(read_text(second_data), external_data_content);
    REQUIRE_EQ(identity_of(second_manifest), external_manifest_identity);
    REQUIRE_EQ(identity_of(second_data), external_data_identity);
    REQUIRE(!std::filesystem::exists(second_bundle / L"renamed.rdj"));
}

TEST_CASE(task12_cleanup_rejects_a_hardlink_added_after_validation) {
    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto data = sandbox.bundle() / L"owned.rdj";
    const auto outside_link = sandbox.external() / L"outside-link.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(data, "preserve-me");

    auto lock_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(lock_result.ok());
    auto writer = lock_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "owned.rdj", identity_of(data));
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();

    REQUIRE(CreateHardLinkW(outside_link.c_str(), data.c_str(), nullptr));
    const auto before_identity = identity_of(outside_link);
    const auto before_content = read_text(outside_link);
    const auto denied = delete_validated_bundle_file(writer, capability);
    REQUIRE(is_image_path_error(denied, ImagePathErrorCode::HardLink));
    REQUIRE_EQ(read_text(data), before_content);
    REQUIRE_EQ(read_text(outside_link), before_content);
    REQUIRE_EQ(identity_of(outside_link), before_identity);
}

TEST_CASE(task12_native_rename_quarantine_is_process_bounded) {
    const auto maximum =
        myrd::persistence::test::maximum_native_rename_slots();
    auto in_use = myrd::persistence::test::native_rename_slots_in_use();
    // Earlier indeterminate-completion tests retain their bounded slots.
    REQUIRE(in_use >= std::size_t{2});
    REQUIRE(maximum > in_use);
    while (in_use < maximum) {
        REQUIRE(
            myrd::persistence::test::quarantine_empty_native_rename_for_test());
        ++in_use;
    }
    REQUIRE_EQ(
        myrd::persistence::test::native_rename_slots_in_use(), maximum);
    REQUIRE(
        !myrd::persistence::test::quarantine_empty_native_rename_for_test());

    TemporarySandbox sandbox;
    const auto manifest = sandbox.bundle() / L"disk.rdm";
    const auto source = sandbox.bundle() / L"capacity.rdj";
    write_text(manifest, std::string(8192, 'm'));
    write_text(source, "capacity-preserved");
    const auto before_identity = identity_of(source);
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.bundle(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto capability_result = open_bundle_file_for_mutation(
        writer, "capacity.rdj", before_identity);
    REQUIRE(capability_result.ok());
    auto capability = capability_result.move_value();
    ScriptedPendingRenameBackend backend(
        ScriptedPendingRenameBackend::Mode::Error, writer);
    myrd::OperationContext context{};
    context.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    const auto rejected =
        myrd::persistence::test::rename_validated_bundle_file_with_backend(
            context,
            writer,
            capability,
            "capacity-renamed.rdj",
            backend);

    REQUIRE(is_image_path_error(rejected, ImagePathErrorCode::IoError));
    REQUIRE_EQ(rejected.error().native_code,
               static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_QUOTA));
    REQUIRE_EQ(backend.begin_calls, std::size_t{0});
    REQUIRE_EQ(read_text(source), std::string("capacity-preserved"));
    REQUIRE_EQ(identity_of(source), before_identity);
    REQUIRE(writer.verify().ok());
}

} // namespace
