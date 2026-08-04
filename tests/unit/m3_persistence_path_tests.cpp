#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include "src/persistence/file_identity.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/writer_lock.h"
#include "src/common/unique_handle.h"
#include "test_framework.h"

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
#include <stdexcept>
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

} // namespace
