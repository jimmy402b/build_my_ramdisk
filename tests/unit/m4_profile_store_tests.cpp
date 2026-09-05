#include "tests/support/test_framework.h"

#include "src/profile/profile_store.h"
#include "src/profile/profile_codec.h"

#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

class TestDirectory final {
public:
    TestDirectory() {
        static std::atomic_uint32_t sequence{0};
        root_ = std::filesystem::temp_directory_path()
            / (L"myrd-m4-profile-store-" + std::to_wstring(GetCurrentProcessId())
               + L"-" + std::to_wstring(sequence.fetch_add(1)));
        REQUIRE(!std::filesystem::exists(root_));
        REQUIRE(std::filesystem::create_directories(program_data()));
    }

    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;

    std::filesystem::path program_data() const {
        return root_ / L"ProgramData";
    }

    std::filesystem::path outside() const {
        return root_ / L"outside";
    }

private:
    std::filesystem::path root_;
};

void create_directory_with_sddl(
    const std::filesystem::path& path, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, &descriptor, nullptr));
    SECURITY_ATTRIBUTES attributes{
        sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const auto error = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    REQUIRE(created || error == ERROR_ALREADY_EXISTS);
}

void create_system_admin_directory(const std::filesystem::path& path) {
    create_directory_with_sddl(
        path, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
}

void create_secure_profile_layout(
    const std::filesystem::path& program_data,
    const myrd::product::Uuid& profile_id) {
    const auto product_root = program_data / L"MyRamDisk";
    const auto profiles_root = product_root / L"profiles";
    const auto id = profile_id.to_string();
    const auto profile_root = profiles_root
        / std::wstring(id.begin(), id.end());
    create_system_admin_directory(product_root);
    create_system_admin_directory(profiles_root);
    create_system_admin_directory(profile_root);
}

std::vector<std::byte> bytes(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::byte>& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(reinterpret_cast<const char*>(value.data()),
                 static_cast<std::streamsize>(value.size()));
    REQUIRE(stream.good());
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    REQUIRE(stream.good());
    const auto length = stream.tellg();
    REQUIRE(length >= 0);
    std::vector<std::byte> value(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(value.data()), length);
    REQUIRE(stream.good());
    return value;
}

myrd::profile::ProfileRecord profile_record(
    std::uint64_t revision, std::wstring display_name) {
    return myrd::profile::ProfileRecord{
        revision,
        myrd::profile::Profile{
            uuid("00112233-4455-4677-8899-aabbccddeeff"),
            L"S-1-5-21-100-200-300-1001",
            std::move(display_name),
            1700000000000ULL,
            myrd::profile::ProfileLifecycle::provisioning,
            L"D:\\Images\\disk.bundle",
            "disk.rdm",
            uuid("10213243-5465-7687-98a9-bacbdcedfe0f"),
            8ULL * 1024ULL * 1024ULL * 1024ULL,
            512,
            4096,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            myrd::profile::MountKind::drive_letter,
            L"R:",
            true,
            myrd::profile::MemoryPolicy{
                myrd::profile::MemoryMode::automatic,
                2ULL * 1024ULL * 1024ULL * 1024ULL,
                1536ULL * 1024ULL * 1024ULL,
                1ULL * 1024ULL * 1024ULL * 1024ULL,
                256ULL * 1024ULL * 1024ULL},
            {},
            {}}};
}

std::vector<std::byte> encoded_record(
    const myrd::profile::ProfileRecord& record) {
    auto result = myrd::profile::encode_profile(record);
    REQUIRE(result.ok());
    return result.move_value();
}

class AcceptingBindingVerifier final
    : public myrd::profile::IProfileRepairBindingVerifier {
public:
    myrd::Result<void> verify_repair_binding(
        const myrd::profile::ProfileRecord& candidate) override {
        ++calls;
        observed = candidate;
        return myrd::Result<void>::success();
    }

    std::size_t calls{};
    std::optional<myrd::profile::ProfileRecord> observed;
};

class RejectingBindingVerifier final
    : public myrd::profile::IProfileRepairBindingVerifier {
public:
    myrd::Result<void> verify_repair_binding(
        const myrd::profile::ProfileRecord&) override {
        return myrd::Result<void>::failure(myrd::Error{
            myrd::ErrorDomain::protocol, 0x7fff,
            L"test.binding", L"Rejected binding"});
    }
};

class DiskFullDirectory final : public myrd::profile::IAtomicFileDirectory {
public:
    explicit DiskFullDirectory(std::vector<std::byte> current)
        : current_(std::move(current)) {}

    myrd::Result<std::vector<std::byte>> read_file(
        std::wstring_view name) override {
        if (name == L"profile.myrd") {
            return myrd::Result<std::vector<std::byte>>::success(current_);
        }
        return myrd::Result<std::vector<std::byte>>::failure(myrd::Error{
            myrd::ErrorDomain::protocol,
            static_cast<std::uint32_t>(
                myrd::profile::ProfileStoreError::path_not_found),
            L"test.read", L"File not found"});
    }
    myrd::Result<myrd::profile::AtomicTempFile> create_unique_temp(
        std::wstring_view) override {
        return myrd::Result<myrd::profile::AtomicTempFile>::success({1});
    }
    myrd::Result<void> write_temp(
        myrd::profile::AtomicTempFile,
        std::span<const std::byte>) override {
        return myrd::Result<void>::failure(myrd::Error{
            myrd::ErrorDomain::win32,
            static_cast<std::uint32_t>(
                myrd::profile::ProfileStoreError::io_failure),
            L"test.write", L"Disk full", ERROR_DISK_FULL});
    }
    myrd::Result<void> flush_temp(
        myrd::profile::AtomicTempFile) override {
        return unexpected();
    }
    myrd::Result<std::vector<std::byte>> read_temp(
        myrd::profile::AtomicTempFile) override {
        return myrd::Result<std::vector<std::byte>>::failure(
            unexpected().error());
    }
    myrd::Result<void> publish_temp(
        myrd::profile::AtomicTempFile,
        std::wstring_view,
        bool) override {
        ++publish_calls;
        return unexpected();
    }
    myrd::Result<void> remove_file(std::wstring_view) override {
        return unexpected();
    }
    myrd::Result<std::vector<std::wstring>> list_files() override {
        return myrd::Result<std::vector<std::wstring>>::failure(
            unexpected().error());
    }
    myrd::Result<void> flush_directory() override { return unexpected(); }
    void discard_temp(myrd::profile::AtomicTempFile) noexcept override {
        ++discard_calls;
    }

    std::uint32_t publish_calls{};
    std::uint32_t discard_calls{};

private:
    static myrd::Result<void> unexpected() {
        return myrd::Result<void>::failure(myrd::Error{
            myrd::ErrorDomain::internal, 0x7ffe,
            L"test.unexpected", L"Unexpected backend call"});
    }

    std::vector<std::byte> current_;
};

} // namespace

TEST_CASE(profile_store_derives_the_exact_programdata_layout) {
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    auto paths = myrd::profile::derive_profile_store_paths(
        L"C:\\ProgramData", profile_id);
    REQUIRE(paths.ok());
    REQUIRE_EQ(paths.value().product_root,
               std::wstring(L"C:\\ProgramData\\MyRamDisk"));
    REQUIRE_EQ(paths.value().profiles_root,
               std::wstring(L"C:\\ProgramData\\MyRamDisk\\profiles"));
    REQUIRE_EQ(paths.value().profile_root,
               std::wstring(
                   L"C:\\ProgramData\\MyRamDisk\\profiles\\"
                   L"00112233-4455-4677-8899-aabbccddeeff"));
    REQUIRE_EQ(paths.value().current_profile,
               paths.value().profile_root + L"\\profile.myrd");
    REQUIRE_EQ(paths.value().previous_profile,
               paths.value().profile_root + L"\\profile.prev");
    REQUIRE_EQ(paths.value().runtime_lease,
               paths.value().profile_root + L"\\runtime.lease");
}

TEST_CASE(profile_store_path_derivation_rejects_untrusted_root_shapes) {
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    for (const auto root : {
             std::wstring_view{}, std::wstring_view(L"ProgramData"),
             std::wstring_view(L"C:ProgramData"),
             std::wstring_view(L"C:\\ProgramData\\..\\Temp")}) {
        REQUIRE(!myrd::profile::derive_profile_store_paths(
            root, profile_id).ok());
    }
}

TEST_CASE(profile_store_opens_only_the_exact_protected_directory_chain) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);

    auto opened = myrd::profile::ProfileDirectoryCapability::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(opened.ok());
    REQUIRE_EQ(opened.value().paths().profile_root,
               (sandbox.program_data() / L"MyRamDisk" / L"profiles"
                / L"00112233-4455-4677-8899-aabbccddeeff").wstring());
}

TEST_CASE(profile_store_rejects_a_broad_product_root_acl) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    REQUIRE(std::filesystem::create_directories(
        sandbox.program_data() / L"MyRamDisk" / L"profiles"
        / L"00112233-4455-4677-8899-aabbccddeeff"));

    auto opened = myrd::profile::ProfileDirectoryCapability::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(!opened.ok());
    REQUIRE_EQ(opened.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::unsafe_acl));
}

TEST_CASE(profile_store_rejects_unexpected_management_ace_flags) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    const auto product_root = sandbox.program_data() / L"MyRamDisk";
    const auto profiles_root = product_root / L"profiles";
    const auto profile_root = profiles_root
        / L"00112233-4455-4677-8899-aabbccddeeff";
    create_directory_with_sddl(
        product_root,
        L"D:P(A;OICINP;FA;;;SY)(A;OICINP;FA;;;BA)");
    create_system_admin_directory(profiles_root);
    create_system_admin_directory(profile_root);

    auto opened = myrd::profile::ProfileDirectoryCapability::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(!opened.ok());
    REQUIRE_EQ(opened.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::unsafe_acl));
}

TEST_CASE(profile_store_rejects_a_reparse_point_in_the_directory_chain) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    REQUIRE(std::filesystem::create_directories(sandbox.outside()));
    REQUIRE(CreateSymbolicLinkW(
        (sandbox.program_data() / L"MyRamDisk").c_str(),
        sandbox.outside().c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));

    auto opened = myrd::profile::ProfileDirectoryCapability::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(!opened.ok());
    REQUIRE_EQ(opened.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::unsafe_reparse));
}

TEST_CASE(profile_store_real_backend_publishes_current_and_previous) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto opened = myrd::profile::ProfileDirectoryCapability::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(opened.ok());

    const auto old_value = bytes({0x10, 0x20, 0x30});
    const auto new_value = bytes({0x40, 0x50, 0x60, 0x70});
    write_bytes(opened.value().paths().current_profile, old_value);
    auto published = myrd::profile::publish_atomically_with_previous(
        opened.value(), L"profile.myrd", L"profile.prev", new_value);
    REQUIRE(published.ok());
    REQUIRE_EQ(read_bytes(opened.value().paths().current_profile), new_value);
    REQUIRE_EQ(read_bytes(opened.value().paths().previous_profile), old_value);

    for (const auto& entry : std::filesystem::directory_iterator(
             opened.value().paths().profile_root)) {
        REQUIRE(entry.path().extension() != L".tmp");
    }
}

TEST_CASE(profile_store_real_backend_supports_initial_publication) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto opened = myrd::profile::ProfileDirectoryCapability::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(opened.ok());

    const auto new_value = bytes({0xaa, 0xbb, 0xcc});
    auto published = myrd::profile::publish_atomically_with_previous(
        opened.value(), L"profile.myrd", L"profile.prev", new_value);
    REQUIRE(published.ok());
    REQUIRE_EQ(read_bytes(opened.value().paths().current_profile), new_value);
    REQUIRE(!std::filesystem::exists(opened.value().paths().previous_profile));
}

TEST_CASE(profile_store_real_backend_rejects_hard_linked_files) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto opened = myrd::profile::ProfileDirectoryCapability::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(opened.ok());

    write_bytes(opened.value().paths().current_profile, bytes({0x01}));
    const auto alias = std::filesystem::path(opened.value().paths().profile_root)
        / L"profile.alias";
    REQUIRE(CreateHardLinkW(
        alias.c_str(), opened.value().paths().current_profile.c_str(), nullptr));
    auto read = opened.value().read_file(L"profile.myrd");
    REQUIRE(!read.ok());
}

TEST_CASE(managed_atomic_directory_reuses_safe_publish_and_handle_delete) {
    TestDirectory sandbox;
    const auto product_root = sandbox.program_data() / L"MyRamDisk";
    const auto operations_root = product_root / L"operations";
    create_system_admin_directory(product_root);
    create_system_admin_directory(operations_root);
    const std::array<std::wstring_view, 2> components{
        L"MyRamDisk", L"operations"};
    auto managed = myrd::profile::open_managed_atomic_directory(
        sandbox.program_data().wstring(), components);
    REQUIRE(managed.ok());
    REQUIRE_EQ(managed.value().path, operations_root.wstring());

    const auto value = bytes({0x10, 0x20, 0x30});
    REQUIRE(myrd::profile::publish_atomically_with_previous(
        *managed.value().directory, L"record.myop", L"record.prev", value).ok());
    REQUIRE_EQ(read_bytes(operations_root / L"record.myop"), value);
    REQUIRE(managed.value().directory->remove_file(L"record.myop").ok());
    REQUIRE(managed.value().directory->flush_directory().ok());
    REQUIRE(!std::filesystem::exists(operations_root / L"record.myop"));
}

TEST_CASE(managed_atomic_directory_rejects_traversal_and_hardlink_delete) {
    TestDirectory sandbox;
    const auto product_root = sandbox.program_data() / L"MyRamDisk";
    const auto operations_root = product_root / L"operations";
    create_system_admin_directory(product_root);
    create_system_admin_directory(operations_root);
    const std::array<std::wstring_view, 2> traversal{
        L"MyRamDisk", L".."};
    REQUIRE(!myrd::profile::open_managed_atomic_directory(
        sandbox.program_data().wstring(), traversal).ok());

    const std::array<std::wstring_view, 2> components{
        L"MyRamDisk", L"operations"};
    auto managed = myrd::profile::open_managed_atomic_directory(
        sandbox.program_data().wstring(), components);
    REQUIRE(managed.ok());
    const auto record = operations_root / L"record.myop";
    const auto alias = operations_root / L"record.alias";
    write_bytes(record, bytes({0x01}));
    REQUIRE(CreateHardLinkW(alias.c_str(), record.c_str(), nullptr));
    REQUIRE(!managed.value().directory->remove_file(L"record.myop").ok());
    REQUIRE(std::filesystem::exists(record));
    REQUIRE(std::filesystem::exists(alias));
    REQUIRE(!managed.value().directory->remove_file(
        L"..\\profiles\\profile.myrd").ok());
}

TEST_CASE(profile_store_load_and_revision_cas_are_end_to_end) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());

    const auto first = profile_record(1, L"first");
    REQUIRE(store.value().write_next(0, first, true).ok());
    auto loaded = store.value().load_current();
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value(), first);

    const auto second = profile_record(2, L"second");
    REQUIRE(store.value().write_next(1, second, true).ok());
    auto stale = store.value().write_next(
        1, profile_record(3, L"stale"), true);
    REQUIRE(!stale.ok());
    REQUIRE_EQ(stale.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::revision_conflict));
    REQUIRE_EQ(store.value().load_current().value(), second);
}

TEST_CASE(profile_store_serializes_concurrent_revision_cas_writers) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    REQUIRE(store.value().write_next(
        0, profile_record(1, L"first"), true).ok());

    std::atomic_uint32_t successes{};
    auto writer = [&](std::wstring name) {
        if (store.value().write_next(
                1, profile_record(2, std::move(name)), true).ok()) {
            ++successes;
        }
    };
    std::thread first(writer, L"winner-a");
    std::thread second(writer, L"winner-b");
    first.join();
    second.join();
    REQUIRE_EQ(successes.load(), 1U);
    REQUIRE_EQ(store.value().load_current().value().revision, 2ULL);
}

TEST_CASE(profile_store_serializes_cas_across_independent_store_instances) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto first_store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    auto second_store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(first_store.ok());
    REQUIRE(second_store.ok());
    REQUIRE(first_store.value().write_next(
        0, profile_record(1, L"first"), true).ok());

    std::atomic_uint32_t ready{};
    std::atomic_bool start{};
    std::atomic_uint32_t successes{};
    auto writer = [&](myrd::profile::ProfileStore& store, std::wstring name) {
        ++ready;
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (store.write_next(
                1, profile_record(2, std::move(name)), true).ok()) {
            ++successes;
        }
    };
    std::thread first(
        writer, std::ref(first_store.value()), L"instance-a");
    std::thread second(
        writer, std::ref(second_store.value()), L"instance-b");
    while (ready.load(std::memory_order_acquire) != 2U) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    first.join();
    second.join();
    REQUIRE_EQ(successes.load(), 1U);
    REQUIRE_EQ(first_store.value().load_current().value().revision, 2ULL);
}

TEST_CASE(profile_store_load_current_never_falls_back_to_previous) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    REQUIRE(store.value().write_next(
        0, profile_record(1, L"first"), true).ok());
    REQUIRE(store.value().write_next(
        1, profile_record(2, L"second"), true).ok());

    auto corrupt = read_bytes(store.value().paths().current_profile);
    REQUIRE(corrupt.size() > myrd::profile::kProfileHeaderBytes);
    corrupt.back() ^= std::byte{0xff};
    write_bytes(store.value().paths().current_profile, corrupt);
    auto loaded = store.value().load_current();
    REQUIRE(!loaded.ok());
    REQUIRE_EQ(loaded.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::current_corrupt));
    auto previous = store.value().load_previous();
    REQUIRE(previous.ok());
    REQUIRE_EQ(previous.value().revision, 1ULL);
}

TEST_CASE(profile_store_missing_current_never_falls_back_to_previous) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    const auto paths = myrd::profile::derive_profile_store_paths(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(paths.ok());
    write_bytes(paths.value().previous_profile,
                encoded_record(profile_record(1, L"previous")));
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());

    auto current = store.value().load_current();
    REQUIRE(!current.ok());
    REQUIRE_EQ(current.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::path_not_found));
    REQUIRE_EQ(store.value().load_previous().value().revision, 1ULL);
}

TEST_CASE(profile_store_maps_oversized_current_to_corruption) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    const auto paths = myrd::profile::derive_profile_store_paths(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(paths.ok());
    write_bytes(paths.value().current_profile,
                std::vector<std::byte>(
                    myrd::profile::kProfileMaximumBytes + 1U,
                    std::byte{0x5a}));
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    auto loaded = store.value().load_current();
    REQUIRE(!loaded.ok());
    REQUIRE_EQ(loaded.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::current_corrupt));
}

TEST_CASE(profile_store_rejects_uuid_mismatch_and_invalid_updates_without_write) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    REQUIRE(store.value().write_next(
        0, profile_record(1, L"first"), true).ok());

    auto invalid = profile_record(2, L"invalid");
    invalid.profile.owner_sid = L"S-1-5-19";
    auto rejected = store.value().write_next(1, invalid, false);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::validation_failed));
    REQUIRE_EQ(store.value().load_current().value().revision, 1ULL);
    REQUIRE(!std::filesystem::exists(store.value().paths().previous_profile));

    auto wrong_id = profile_record(2, L"wrong-id");
    wrong_id.profile.profile_uuid =
        uuid("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    rejected = store.value().write_next(1, wrong_id, false);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::profile_id_mismatch));
    REQUIRE_EQ(store.value().load_current().value().revision, 1ULL);
}

TEST_CASE(profile_store_rebind_changes_only_bundle_binding_and_keeps_previous) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    const auto original = profile_record(1, L"first");
    REQUIRE(store.value().write_next(0, original, true).ok());

    auto rebound = original;
    rebound.revision = 2;
    rebound.profile.bundle_path = L"D:\\Images\\replacement.bundle";
    rebound.profile.manifest_name = "replacement.rdm";
    REQUIRE(store.value().write_rebind(1, rebound).ok());
    REQUIRE_EQ(store.value().load_current().value(), rebound);
    REQUIRE_EQ(store.value().load_previous().value(), original);
}

TEST_CASE(profile_store_rebind_rejects_stale_jump_and_unrelated_changes) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    const auto original = profile_record(1, L"first");
    REQUIRE(store.value().write_next(0, original, true).ok());

    auto proposed = original;
    proposed.revision = 2;
    proposed.profile.bundle_path = L"D:\\Images\\replacement.bundle";
    proposed.profile.manifest_name = "replacement.rdm";
    auto rejected = store.value().write_rebind(0, proposed);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::revision_conflict));

    proposed.revision = 3;
    rejected = store.value().write_rebind(1, proposed);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::validation_failed));

    proposed.revision = 2;
    proposed.profile.display_name = L"changed-too";
    rejected = store.value().write_rebind(1, proposed);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::profile::ProfileStoreError::validation_failed));
    REQUIRE_EQ(store.value().load_current().value(), original);
    REQUIRE(!std::filesystem::exists(store.value().paths().previous_profile));
}

TEST_CASE(profile_store_repairs_only_with_expected_revision_and_binding_proof) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    REQUIRE(store.value().write_next(
        0, profile_record(1, L"first"), true).ok());
    REQUIRE(store.value().write_next(
        1, profile_record(2, L"second"), true).ok());

    auto corrupt = read_bytes(store.value().paths().current_profile);
    corrupt.back() ^= std::byte{0xff};
    write_bytes(store.value().paths().current_profile, corrupt);
    RejectingBindingVerifier rejecting;
    REQUIRE(!store.value().repair_from_previous(2, rejecting).ok());
    REQUIRE(!store.value().load_current().ok());
    REQUIRE_EQ(store.value().load_previous().value().revision, 1ULL);

    AcceptingBindingVerifier accepting;
    REQUIRE(!store.value().repair_from_previous(1, accepting).ok());
    REQUIRE_EQ(accepting.calls, 0U);
    REQUIRE(store.value().repair_from_previous(2, accepting).ok());
    REQUIRE_EQ(accepting.calls, 1U);
    REQUIRE_EQ(accepting.observed->revision, 1ULL);
    auto repaired = store.value().load_current();
    REQUIRE(repaired.ok());
    REQUIRE_EQ(repaired.value().revision, 3ULL);
    REQUIRE_EQ(repaired.value().profile.display_name, std::wstring(L"first"));
    REQUIRE_EQ(store.value().load_previous().value().revision, 1ULL);
}

TEST_CASE(profile_store_repairs_semantically_invalid_current_records) {
    TestDirectory sandbox;
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    create_secure_profile_layout(sandbox.program_data(), profile_id);
    const auto paths = myrd::profile::derive_profile_store_paths(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(paths.ok());
    const auto previous = profile_record(1, L"previous");
    auto invalid_current = profile_record(2, L"invalid-current");
    invalid_current.profile.memory.hard_limit_bytes = 1;
    write_bytes(paths.value().previous_profile, encoded_record(previous));
    write_bytes(paths.value().current_profile, encoded_record(invalid_current));

    auto store = myrd::profile::ProfileStore::open_existing(
        sandbox.program_data().wstring(), profile_id);
    REQUIRE(store.ok());
    REQUIRE(!store.value().load_current().ok());
    AcceptingBindingVerifier accepting;
    REQUIRE(store.value().repair_from_previous(2, accepting).ok());
    REQUIRE_EQ(store.value().load_current().value().revision, 3ULL);
    REQUIRE_EQ(store.value().load_previous().value(), previous);
}

TEST_CASE(profile_store_disk_full_never_reaches_publication) {
    const auto profile_id = uuid("00112233-4455-4677-8899-aabbccddeeff");
    auto current = myrd::profile::encode_profile(
        profile_record(1, L"first"));
    REQUIRE(current.ok());
    auto backend = std::make_unique<DiskFullDirectory>(current.move_value());
    auto* observed = backend.get();
    auto store = myrd::profile::ProfileStore::from_directory_capability(
        std::move(backend), {}, profile_id);
    REQUIRE(store.ok());

    auto result = store.value().write_next(
        1, profile_record(2, L"second"), true);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().native_code,
               static_cast<std::uint32_t>(ERROR_DISK_FULL));
    REQUIRE_EQ(observed->publish_calls, 0U);
    REQUIRE_EQ(observed->discard_calls, 1U);
    auto loaded = store.value().load_current();
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().revision, 1ULL);
}
