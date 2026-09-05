#include "tests/support/test_framework.h"

#include "src/control/authorization.h"
#include "src/host/host_ipc.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <AclAPI.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace {

class LocalSid final {
public:
    explicit LocalSid(std::wstring_view text) {
        PSID value = nullptr;
        REQUIRE(ConvertStringSidToSidW(
            std::wstring(text).c_str(), &value));
        value_.reset(value);
    }
    PSID get() const noexcept { return value_.get(); }

private:
    struct Deleter final {
        void operator()(void* value) const noexcept {
            if (value != nullptr) static_cast<void>(LocalFree(value));
        }
    };
    std::unique_ptr<void, Deleter> value_;
};

PSID well_known_sid(
    WELL_KNOWN_SID_TYPE type,
    std::array<std::byte, SECURITY_MAX_SID_SIZE>& storage) {
    DWORD bytes = static_cast<DWORD>(storage.size());
    REQUIRE(CreateWellKnownSid(type, nullptr, storage.data(), &bytes));
    return storage.data();
}

bool has_allow_ace(PACL acl, ACCESS_MASK mask, PSID sid) {
    ACL_SIZE_INFORMATION size{};
    REQUIRE(GetAclInformation(
        acl, &size, sizeof(size), AclSizeInformation));
    for (DWORD index = 0; index < size.AceCount; ++index) {
        void* raw = nullptr;
        REQUIRE(GetAce(acl, index, &raw));
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        if (ace->Mask == mask
            && EqualSid(
                const_cast<DWORD*>(&ace->SidStart), sid)) {
            return true;
        }
    }
    return false;
}

bool current_process_is_local_system() {
    std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
    DWORD bytes = static_cast<DWORD>(storage.size());
    REQUIRE(CreateWellKnownSid(
        WinLocalSystemSid, nullptr, storage.data(), &bytes));
    BOOL member = FALSE;
    REQUIRE(CheckTokenMembership(nullptr, storage.data(), &member));
    return member != FALSE;
}

myrd::product::Uuid uuid(std::string_view value) {
    auto parsed = myrd::product::Uuid::parse(value);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

myrd::host::WorkerBinding binding() {
    return myrd::host::WorkerBinding{
        uuid("11111111-1111-4111-8111-111111111111"),
        uuid("22222222-2222-4222-8222-222222222222"),
        4242,
        0x01dc'0000'1234'5678ULL,
        7,
        19,
        myrd::product::kProductVersionMajor,
        myrd::product::kProductVersionMinor,
        myrd::product::kProductVersionPatch};
}

myrd::host::SecretBytes secret_bytes(std::byte value) {
    myrd::host::SecretBytes bytes{};
    bytes.fill(value);
    return bytes;
}

myrd::Error fake_error(const wchar_t* operation) {
    return myrd::Error{
        myrd::ErrorDomain::win32,
        static_cast<std::uint32_t>(
            myrd::host::HostProtocolError::invalid_argument),
        operation,
        L"injected runtime lease failure",
        ERROR_WRITE_FAULT};
}

enum class LeaseFailpoint {
    none,
    create,
    write,
    flush_temp,
    read_temp,
    publish_before,
    publish_after,
    flush_directory,
};

class FakeRuntimeLeaseDirectory final
    : public myrd::host::IRuntimeLeaseDirectory {
public:
    explicit FakeRuntimeLeaseDirectory(LeaseFailpoint failpoint)
        : failpoint_(failpoint) {}

    myrd::Result<myrd::host::RuntimeLeaseTemp> create_secure_temp() override {
        if (failpoint_ == LeaseFailpoint::create) {
            return myrd::Result<myrd::host::RuntimeLeaseTemp>::failure(
                fake_error(L"fake.create"));
        }
        temp_exists = true;
        return myrd::Result<myrd::host::RuntimeLeaseTemp>::success({1});
    }
    myrd::Result<void> write_temp(
        myrd::host::RuntimeLeaseTemp,
        std::span<const std::byte> bytes) override {
        if (failpoint_ == LeaseFailpoint::write) {
            return myrd::Result<void>::failure(fake_error(L"fake.write"));
        }
        temp.assign(bytes.begin(), bytes.end());
        return myrd::Result<void>::success();
    }
    myrd::Result<void> flush_temp(
        myrd::host::RuntimeLeaseTemp) override {
        if (failpoint_ == LeaseFailpoint::flush_temp) {
            return myrd::Result<void>::failure(fake_error(L"fake.flush"));
        }
        return myrd::Result<void>::success();
    }
    myrd::Result<std::vector<std::byte>> read_temp(
        myrd::host::RuntimeLeaseTemp) override {
        if (failpoint_ == LeaseFailpoint::read_temp) {
            return myrd::Result<std::vector<std::byte>>::failure(
                fake_error(L"fake.read_temp"));
        }
        return myrd::Result<std::vector<std::byte>>::success(temp);
    }
    myrd::Result<void> publish_temp(
        myrd::host::RuntimeLeaseTemp) override {
        if (failpoint_ == LeaseFailpoint::publish_before) {
            return myrd::Result<void>::failure(
                fake_error(L"fake.publish_before"));
        }
        current = temp;
        temp.clear();
        temp_exists = false;
        if (failpoint_ == LeaseFailpoint::publish_after) {
            return myrd::Result<void>::failure(
                fake_error(L"fake.publish_after"));
        }
        return myrd::Result<void>::success();
    }
    myrd::Result<void> flush_directory() override {
        if (failpoint_ == LeaseFailpoint::flush_directory) {
            return myrd::Result<void>::failure(
                fake_error(L"fake.flush_directory"));
        }
        return myrd::Result<void>::success();
    }
    myrd::Result<std::vector<std::byte>> read_current() override {
        if (!current.has_value()) {
            return myrd::Result<std::vector<std::byte>>::failure(
                fake_error(L"fake.read_current"));
        }
        return myrd::Result<std::vector<std::byte>>::success(
            current.value());
    }
    void discard_temp(myrd::host::RuntimeLeaseTemp) noexcept override {
        temp.clear();
        temp_exists = false;
    }

    LeaseFailpoint failpoint_;
    std::optional<std::vector<std::byte>> current;
    std::vector<std::byte> temp;
    bool temp_exists{};
};

} // namespace

TEST_CASE(host_ipc_private_pipe_descriptor_is_system_and_service_only) {
    auto security = myrd::host::create_private_pipe_security_descriptor();
    REQUIRE(security.ok());
    auto* descriptor = static_cast<PSECURITY_DESCRIPTOR>(
        security.value().native_descriptor());
    REQUIRE(descriptor != nullptr);
    REQUIRE(IsValidSecurityDescriptor(descriptor));

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    REQUIRE(GetSecurityDescriptorControl(
        descriptor, &control, &revision));
    REQUIRE((control & SE_DACL_PRESENT) != 0);
    REQUIRE((control & SE_DACL_PROTECTED) != 0);

    BOOL present = FALSE;
    BOOL defaulted = TRUE;
    PACL dacl = nullptr;
    REQUIRE(GetSecurityDescriptorDacl(
        descriptor, &present, &dacl, &defaulted));
    REQUIRE(present);
    REQUIRE(dacl != nullptr);
    REQUIRE(!defaulted);

    ACL_SIZE_INFORMATION size{};
    REQUIRE(GetAclInformation(
        dacl, &size, sizeof(size), AclSizeInformation));
    REQUIRE_EQ(size.AceCount, 2UL);

    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage{};
    auto* system = well_known_sid(WinLocalSystemSid, system_storage);
    LocalSid service(myrd::ipc::kMyRamDiskServiceSid);
    REQUIRE(has_allow_ace(dacl, FILE_ALL_ACCESS, system));
    REQUIRE(has_allow_ace(
        dacl, myrd::host::kPrivatePipeClientAccessMask, service.get()));

    PSID owner = nullptr;
    BOOL owner_defaulted = TRUE;
    REQUIRE(GetSecurityDescriptorOwner(
        descriptor, &owner, &owner_defaulted));
    REQUIRE(owner != nullptr);
    REQUIRE(!owner_defaulted);
    REQUIRE(EqualSid(owner, system));
}

TEST_CASE(host_ipc_private_pipe_creation_requires_local_system_context) {
    const auto profile = binding().profile_id;
    auto instance = myrd::host::PrivatePipeInstance::create(profile);
    if (!instance.ok()) {
        REQUIRE(!current_process_is_local_system());
        REQUIRE(
            instance.error().native_code == ERROR_ACCESS_DENIED
            || instance.error().native_code == ERROR_INVALID_OWNER
            || instance.error().native_code == ERROR_PRIVILEGE_NOT_HELD);
        return;
    }
    REQUIRE(instance.ok());
    REQUIRE_EQ(
        instance.value().name(),
        L"\\\\.\\pipe\\MyRamDisk.Worker."
        L"11111111-1111-4111-8111-111111111111");
    REQUIRE(instance.value().native_handle() != nullptr);

    DWORD flags = 0;
    REQUIRE(GetNamedPipeInfo(
        static_cast<HANDLE>(instance.value().native_handle()),
        &flags, nullptr, nullptr, nullptr));
    REQUIRE((flags & PIPE_SERVER_END) != 0);

    auto duplicate = myrd::host::PrivatePipeInstance::create(profile);
    REQUIRE(!duplicate.ok());
}

TEST_CASE(host_ipc_startup_secret_uses_one_inheritable_read_handle) {
    auto service_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x7a}));
    REQUIRE(service_secret.ok());
    auto transfer = myrd::host::prepare_startup_secret_transfer(
        binding(), service_secret.value());
    REQUIRE(transfer.ok());

    const auto raw_handle = transfer.value().inherited_read_handle();
    REQUIRE(raw_handle != nullptr);
    DWORD flags = 0;
    REQUIRE(GetHandleInformation(
        static_cast<HANDLE>(raw_handle), &flags));
    REQUIRE((flags & HANDLE_FLAG_INHERIT) != 0);
    REQUIRE_EQ(GetFileType(static_cast<HANDLE>(raw_handle)), FILE_TYPE_PIPE);

    auto worker_secret = myrd::host::consume_startup_secret(
        transfer.value().release_read_handle(), binding());
    REQUIRE(worker_secret.ok());

    auto service = myrd::host::ServiceHandshake::begin(
        binding(), service_secret.move_value(), [] {
            myrd::host::HandshakeNonce value{};
            value.fill(std::byte{0x41});
            return value;
        }());
    auto worker = myrd::host::WorkerHandshake::begin(
        binding(), worker_secret.move_value());
    REQUIRE(service.ok());
    REQUIRE(worker.ok());
    myrd::host::HandshakeNonce worker_nonce{};
    worker_nonce.fill(std::byte{0x51});
    auto challenge = worker.value().accept_hello(
        service.value().hello(), worker_nonce);
    REQUIRE(challenge.ok());
    REQUIRE(service.value().accept_challenge(challenge.value()).ok());
}

TEST_CASE(host_ipc_startup_secret_rejects_cross_profile_consumption) {
    auto secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x7a}));
    REQUIRE(secret.ok());
    auto transfer = myrd::host::prepare_startup_secret_transfer(
        binding(), secret.value());
    REQUIRE(transfer.ok());

    auto wrong = binding();
    wrong.profile_id = uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    auto consumed = myrd::host::consume_startup_secret(
        transfer.value().release_read_handle(), wrong);
    REQUIRE(!consumed.ok());
}

TEST_CASE(host_ipc_generated_secret_is_256_bit_and_nonzero) {
    auto generated = myrd::host::SessionSecret::generate();
    REQUIRE(generated.ok());
    REQUIRE_EQ(generated.value().bytes().size(), 32U);
    std::byte combined{};
    for (const auto value : generated.value().bytes()) combined |= value;
    REQUIRE(combined != std::byte{});
}

TEST_CASE(host_ipc_runtime_lease_round_trips_only_dpapi_ciphertext) {
    const auto raw_secret = secret_bytes(std::byte{0x6b});
    auto secret = myrd::host::SessionSecret::from_bytes(raw_secret);
    REQUIRE(secret.ok());
    auto encoded = myrd::host::encode_runtime_lease(
        binding(), secret.value());
    REQUIRE(encoded.ok());
    REQUIRE(encoded.value().size() <= myrd::host::kMaximumRuntimeLeaseBytes);
    REQUIRE(std::search(
        encoded.value().begin(), encoded.value().end(),
        raw_secret.begin(), raw_secret.end()) == encoded.value().end());

    auto decoded = myrd::host::decode_runtime_lease(
        encoded.value(), binding());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().binding, binding());
    REQUIRE(std::equal(
        decoded.value().secret.bytes().begin(),
        decoded.value().secret.bytes().end(),
        raw_secret.begin(), raw_secret.end()));
}

TEST_CASE(host_ipc_runtime_lease_rejects_stale_or_cross_bound_identity) {
    auto secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x6b}));
    REQUIRE(secret.ok());
    auto encoded = myrd::host::encode_runtime_lease(
        binding(), secret.value());
    REQUIRE(encoded.ok());

    const auto original = binding();
    for (std::size_t mutation = 0; mutation < 6; ++mutation) {
        auto wrong = original;
        switch (mutation) {
        case 0:
            wrong.profile_id = uuid(
                "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
            break;
        case 1:
            wrong.image_id = uuid(
                "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
            break;
        case 2: ++wrong.worker_pid; break;
        case 3: ++wrong.process_creation_time; break;
        case 4: ++wrong.session_generation; break;
        case 5: ++wrong.profile_revision; break;
        default: REQUIRE(false); break;
        }
        REQUIRE(!myrd::host::decode_runtime_lease(
            encoded.value(), wrong).ok());
    }
}

TEST_CASE(host_ipc_runtime_lease_rejects_every_truncation_and_mutation) {
    auto secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x6b}));
    REQUIRE(secret.ok());
    auto encoded = myrd::host::encode_runtime_lease(
        binding(), secret.value());
    REQUIRE(encoded.ok());

    for (std::size_t size = 0; size < encoded.value().size(); ++size) {
        REQUIRE(!myrd::host::decode_runtime_lease(
            std::span<const std::byte>(encoded.value()).first(size),
            binding()).ok());
    }
    auto trailing = encoded.value();
    trailing.push_back(std::byte{0x5a});
    REQUIRE(!myrd::host::decode_runtime_lease(trailing, binding()).ok());

    for (std::size_t index = 0; index < encoded.value().size(); ++index) {
        auto mutated = encoded.value();
        mutated[index] ^= std::byte{0x01};
        REQUIRE(!myrd::host::decode_runtime_lease(
            mutated, binding()).ok());
    }
}

TEST_CASE(host_ipc_runtime_lease_rotation_is_exactly_monotonic) {
    const auto current = binding();
    auto next = myrd::host::next_reattachment_binding(
        current, current.profile_revision + 1);
    REQUIRE(next.ok());
    REQUIRE_EQ(
        next.value().session_generation,
        current.session_generation + 1);
    REQUIRE_EQ(
        next.value().profile_revision,
        current.profile_revision + 1);
    REQUIRE_EQ(next.value().profile_id, current.profile_id);
    REQUIRE_EQ(next.value().image_id, current.image_id);
    REQUIRE_EQ(next.value().worker_pid, current.worker_pid);
    REQUIRE_EQ(
        next.value().process_creation_time,
        current.process_creation_time);

    auto rollback = myrd::host::next_reattachment_binding(
        next.value(), current.profile_revision);
    REQUIRE(!rollback.ok());
    auto exhausted = current;
    exhausted.session_generation =
        (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(!myrd::host::next_reattachment_binding(
        exhausted, exhausted.profile_revision).ok());
}

TEST_CASE(host_ipc_runtime_lease_rotation_survives_crash_before_worker_commit) {
    FakeRuntimeLeaseDirectory directory(LeaseFailpoint::none);
    const auto current = binding();
    auto current_secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x41}));
    REQUIRE(current_secret.ok());
    auto window = myrd::host::ReattachmentCredentialWindow::begin(
        current, current_secret.move_value());
    REQUIRE(window.ok());

    auto next = myrd::host::next_reattachment_binding(
        current, current.profile_revision + 1);
    REQUIRE(next.ok());
    const auto next_bytes = secret_bytes(std::byte{0x51});
    auto next_secret = myrd::host::SessionSecret::from_bytes(next_bytes);
    REQUIRE(next_secret.ok());
    REQUIRE(window.value().stage(
        next.value(), next_secret.move_value()).ok());

    auto publish_secret = window.value().credential_for(next.value());
    REQUIRE(publish_secret.ok());
    REQUIRE(myrd::host::publish_runtime_lease_atomically(
        directory, next.value(), publish_secret.value()).ok());

    // Simulate service failure after publishing N+1 but before the worker
    // commits promotion: the worker still accepts the staged credential.
    auto service_after_restart = myrd::host::load_runtime_lease(
        directory, next.value());
    REQUIRE(service_after_restart.ok());
    auto worker_before_commit = window.value().credential_for(next.value());
    REQUIRE(worker_before_commit.ok());
    REQUIRE(std::equal(
        service_after_restart.value().secret.bytes().begin(),
        service_after_restart.value().secret.bytes().end(),
        worker_before_commit.value().bytes().begin()));

    REQUIRE(window.value().commit(next.value()).ok());
    REQUIRE(!window.value().credential_for(current).ok());
    REQUIRE(window.value().credential_for(next.value()).ok());
}

TEST_CASE(host_ipc_runtime_lease_file_descriptor_is_system_only) {
    auto security = myrd::host::create_runtime_lease_security_descriptor();
    REQUIRE(security.ok());
    auto* descriptor = static_cast<PSECURITY_DESCRIPTOR>(
        security.value().native_descriptor());
    REQUIRE(descriptor != nullptr);

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    REQUIRE(GetSecurityDescriptorControl(
        descriptor, &control, &revision));
    REQUIRE((control & SE_DACL_PRESENT) != 0);
    REQUIRE((control & SE_DACL_PROTECTED) != 0);

    BOOL present = FALSE;
    BOOL defaulted = TRUE;
    PACL dacl = nullptr;
    REQUIRE(GetSecurityDescriptorDacl(
        descriptor, &present, &dacl, &defaulted));
    REQUIRE(present);
    REQUIRE(dacl != nullptr);
    ACL_SIZE_INFORMATION size{};
    REQUIRE(GetAclInformation(
        dacl, &size, sizeof(size), AclSizeInformation));
    REQUIRE_EQ(size.AceCount, 1UL);
    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage{};
    auto* system = well_known_sid(WinLocalSystemSid, system_storage);
    REQUIRE(has_allow_ace(dacl, FILE_ALL_ACCESS, system));
}

TEST_CASE(host_ipc_runtime_lease_publishes_and_loads_atomically) {
    FakeRuntimeLeaseDirectory directory(LeaseFailpoint::none);
    auto secret = myrd::host::SessionSecret::from_bytes(
        secret_bytes(std::byte{0x6b}));
    REQUIRE(secret.ok());
    auto published = myrd::host::publish_runtime_lease_atomically(
        directory, binding(), secret.value());
    REQUIRE(published.ok());
    REQUIRE(directory.current.has_value());
    REQUIRE(!directory.temp_exists);

    auto loaded = myrd::host::load_runtime_lease(directory, binding());
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().binding, binding());
}

TEST_CASE(host_ipc_runtime_lease_publication_failures_are_fail_closed) {
    for (const auto failpoint : {
             LeaseFailpoint::create,
             LeaseFailpoint::write,
             LeaseFailpoint::flush_temp,
             LeaseFailpoint::read_temp}) {
        FakeRuntimeLeaseDirectory directory(failpoint);
        auto secret = myrd::host::SessionSecret::from_bytes(
            secret_bytes(std::byte{0x6b}));
        REQUIRE(secret.ok());
        auto result = myrd::host::publish_runtime_lease_atomically(
            directory, binding(), secret.value());
        REQUIRE(!result.ok());
        REQUIRE(result.error().code != static_cast<std::uint32_t>(
            myrd::host::HostProtocolError::commit_indeterminate));
        REQUIRE(!directory.current.has_value());
        REQUIRE(!directory.temp_exists);
    }

    for (const auto failpoint : {
             LeaseFailpoint::publish_before,
             LeaseFailpoint::publish_after,
             LeaseFailpoint::flush_directory}) {
        FakeRuntimeLeaseDirectory directory(failpoint);
        auto secret = myrd::host::SessionSecret::from_bytes(
            secret_bytes(std::byte{0x6b}));
        REQUIRE(secret.ok());
        auto result = myrd::host::publish_runtime_lease_atomically(
            directory, binding(), secret.value());
        REQUIRE(!result.ok());
        REQUIRE_EQ(
            result.error().code,
            static_cast<std::uint32_t>(
                myrd::host::HostProtocolError::commit_indeterminate));
        REQUIRE(!directory.temp_exists);
    }
}
