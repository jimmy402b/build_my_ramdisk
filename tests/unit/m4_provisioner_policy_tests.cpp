#include "tests/support/test_framework.h"

#include "src/host/provisioner.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::host;

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

PhysicalDiskIdentity expected_disk() {
    return {
        42,
        L"\\\\.\\PhysicalDrive42",
        8ULL << 30,
        11,
        1,
        2,
        3};
}

NewDiskProvisioningExpectation expectation() {
    return {
        {uuid("00112233-4455-4677-8899-aabbccddeeff"),
         uuid("10213243-5465-4687-98a9-bacbdcedfe0f"),
         3,
         7,
         aim::DeviceId{0x00030201}},
        uuid("30000000-0000-4000-8000-000000000001"),
        expected_disk(),
        ProvisioningLifecycle::provisioning,
        ProvisioningSource::new_image,
        true,
        true,
        false,
        false,
        false};
}

NewDiskObservation exact_observation() {
    return {
        expected_disk(),
        ObservedDiskLayout::raw,
        0,
        0,
        0,
        {}};
}

void require_error(
    const Error& error,
    ProvisioningPolicyError expected) {
    REQUIRE_EQ(error.code, static_cast<std::uint32_t>(expected));
}

VerifiedNewDisk verified_new_disk() {
    const std::array observations{exact_observation()};
    auto verified = assess_new_disk_provisioning(expectation(), observations);
    REQUIRE(verified.ok());
    return verified.move_value();
}

std::wstring decode_encoded_command(std::wstring_view encoded) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (const wchar_t wide : encoded) {
        if (wide == L'=') break;
        REQUIRE(wide >= 0 && wide <= 0x7F);
        const auto position = alphabet.find(static_cast<char>(wide));
        REQUIRE(position != std::string_view::npos);
        accumulator = (accumulator << 6)
            | static_cast<unsigned>(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<unsigned char>(
                (accumulator >> bits) & 0xFFU));
        }
    }
    REQUIRE(bytes.size() % 2 == 0);
    std::wstring decoded;
    decoded.reserve(bytes.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); index += 2) {
        decoded.push_back(static_cast<wchar_t>(
            static_cast<unsigned>(bytes[index])
            | static_cast<unsigned>(bytes[index + 1]) << 8));
    }
    return decoded;
}

std::wstring encode_test_script(std::wstring_view script) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    bytes.reserve(script.size() * 2);
    for (const wchar_t code_unit : script) {
        const auto value = static_cast<unsigned>(code_unit);
        bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
        bytes.push_back(static_cast<unsigned char>((value >> 8) & 0xFFU));
    }
    std::wstring encoded;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const unsigned first = bytes[offset];
        const unsigned second = offset + 1 < bytes.size()
            ? bytes[offset + 1] : 0;
        const unsigned third = offset + 2 < bytes.size()
            ? bytes[offset + 2] : 0;
        const unsigned combined = (first << 16) | (second << 8) | third;
        encoded.push_back(static_cast<wchar_t>(
            alphabet[(combined >> 18) & 0x3FU]));
        encoded.push_back(static_cast<wchar_t>(
            alphabet[(combined >> 12) & 0x3FU]));
        encoded.push_back(offset + 1 < bytes.size()
            ? static_cast<wchar_t>(alphabet[(combined >> 6) & 0x3FU])
            : L'=');
        encoded.push_back(offset + 2 < bytes.size()
            ? static_cast<wchar_t>(alphabet[combined & 0x3FU])
            : L'=');
    }
    return encoded;
}

ProvisioningCommand benign_command(
    std::wstring_view script,
    std::chrono::milliseconds timeout = std::chrono::seconds(5),
    std::uint64_t stdout_limit = 4096,
    std::uint64_t stderr_limit = 4096) {
    return {
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
        {L"-NoLogo", L"-NoProfile", L"-NonInteractive",
         L"-ExecutionPolicy", L"Bypass", L"-EncodedCommand",
         encode_test_script(script)},
        timeout,
        stdout_limit,
        stderr_limit,
        "test.benign"};
}

std::wstring lower(std::wstring value) {
    for (auto& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return value;
}

std::optional<std::wstring> environment(const wchar_t* name) {
    std::size_t required = 0;
    if (_wgetenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    std::size_t written = 0;
    if (_wgetenv_s(&written, value.data(), required, name) != 0
        || written != required) {
        return std::nullopt;
    }
    value.resize(written - 1);
    return value;
}

std::uint64_t environment_u64(const wchar_t* name) {
    const auto wide = environment(name);
    REQUIRE(wide.has_value());
    std::string text;
    text.reserve(wide->size());
    for (const wchar_t character : *wide) {
        REQUIRE(character >= L'0' && character <= L'9');
        text.push_back(static_cast<char>(character));
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value, 10);
    REQUIRE(parsed.ec == std::errc{});
    REQUIRE(parsed.ptr == text.data() + text.size());
    return value;
}

product::Uuid environment_uuid(const wchar_t* name) {
    const auto wide = environment(name);
    REQUIRE(wide.has_value());
    std::string text;
    text.reserve(wide->size());
    for (const wchar_t character : *wide) {
        REQUIRE(character >= 0 && character <= 0x7F);
        text.push_back(static_cast<char>(character));
    }
    return uuid(text);
}

} // namespace

TEST_CASE(provisioning_policy_accepts_only_one_exact_owned_raw_empty_disk) {
    const std::array observations{exact_observation()};
    const auto assessed = assess_new_disk_provisioning(
        expectation(), observations);

    REQUIRE(assessed.ok());
    REQUIRE_EQ(assessed.value().owner(), expectation().owner);
    REQUIRE_EQ(assessed.value().physical_disk(), expected_disk());
    REQUIRE_EQ(assessed.value().operation_id(), expectation().operation_id);
}

TEST_CASE(provisioning_policy_ignores_fully_foreign_disks) {
    auto foreign = exact_observation();
    foreign.physical_disk.physical_drive_number = 99;
    foreign.physical_disk.path = L"\\\\.\\PhysicalDrive99";
    foreign.physical_disk.port_number = 19;
    foreign.physical_disk.path_id = 9;
    foreign.physical_disk.target_id = 9;
    foreign.physical_disk.lun = 9;
    const std::array observations{foreign, exact_observation()};

    const auto assessed = assess_new_disk_provisioning(
        expectation(), observations);
    REQUIRE(assessed.ok());
}

TEST_CASE(provisioning_policy_rejects_duplicate_or_reused_disk_identity) {
    const std::array duplicates{exact_observation(), exact_observation()};
    auto assessed = assess_new_disk_provisioning(
        expectation(), duplicates);
    REQUIRE(!assessed.ok());
    require_error(assessed.error(), ProvisioningPolicyError::ambiguous);

    auto reused = exact_observation();
    reused.physical_disk.port_number++;
    reused.physical_disk.length_bytes += 4096;
    const std::array reused_set{reused};
    assessed = assess_new_disk_provisioning(expectation(), reused_set);
    REQUIRE(!assessed.ok());
    require_error(
        assessed.error(), ProvisioningPolicyError::identity_conflict);
}

TEST_CASE(provisioning_policy_rejects_non_provisioning_or_non_new_sources) {
    auto expected = expectation();
    expected.lifecycle = ProvisioningLifecycle::ready;
    const std::array observations{exact_observation()};
    auto assessed = assess_new_disk_provisioning(expected, observations);
    REQUIRE(!assessed.ok());
    require_error(
        assessed.error(), ProvisioningPolicyError::wrong_lifecycle);

    constexpr std::array forbidden_sources{
        ProvisioningSource::imported,
        ProvisioningSource::existing,
        ProvisioningSource::recovered,
        ProvisioningSource::unknown};
    for (const auto source : forbidden_sources) {
        expected = expectation();
        expected.source = source;
        assessed = assess_new_disk_provisioning(expected, observations);
        REQUIRE(!assessed.ok());
        require_error(
            assessed.error(), ProvisioningPolicyError::source_not_new);
    }
}

TEST_CASE(provisioning_policy_requires_current_operation_and_device_ownership) {
    const std::array observations{exact_observation()};
    auto expected = expectation();
    expected.operation_owns_session = false;
    auto assessed = assess_new_disk_provisioning(expected, observations);
    REQUIRE(!assessed.ok());
    require_error(
        assessed.error(), ProvisioningPolicyError::ownership_unproven);

    expected = expectation();
    expected.device_created_by_operation = false;
    assessed = assess_new_disk_provisioning(expected, observations);
    REQUIRE(!assessed.ok());
    require_error(
        assessed.error(), ProvisioningPolicyError::ownership_unproven);
}

TEST_CASE(provisioning_policy_never_formats_after_any_identity_is_recorded) {
    const std::array observations{exact_observation()};
    for (unsigned field = 0; field < 3; ++field) {
        auto expected = expectation();
        if (field == 0) expected.has_recorded_disk_identity = true;
        if (field == 1) expected.has_recorded_partition_identity = true;
        if (field == 2) expected.has_recorded_volume_identity = true;
        const auto assessed = assess_new_disk_provisioning(
            expected, observations);
        REQUIRE(!assessed.ok());
        require_error(
            assessed.error(), ProvisioningPolicyError::already_provisioned);
    }
}

TEST_CASE(provisioning_policy_rejects_non_raw_or_nonempty_layout) {
    const std::array forbidden_layouts{
        ObservedDiskLayout::mbr,
        ObservedDiskLayout::gpt,
        ObservedDiskLayout::unknown};
    for (const auto layout : forbidden_layouts) {
        auto observed = exact_observation();
        observed.layout = layout;
        const std::array observations{observed};
        const auto assessed = assess_new_disk_provisioning(
            expectation(), observations);
        REQUIRE(!assessed.ok());
        require_error(assessed.error(), ProvisioningPolicyError::not_raw);
    }

    for (unsigned field = 0; field < 3; ++field) {
        auto observed = exact_observation();
        if (field == 0) observed.partition_count = 1;
        if (field == 1) observed.volume_count = 1;
        if (field == 2) observed.extent_count = 1;
        const std::array observations{observed};
        const auto assessed = assess_new_disk_provisioning(
            expectation(), observations);
        REQUIRE(!assessed.ok());
        require_error(assessed.error(), ProvisioningPolicyError::not_empty);
    }
}

TEST_CASE(provisioning_policy_rejects_every_protected_disk_class) {
    constexpr unsigned kProtectionCount = 9;
    for (unsigned field = 0; field < kProtectionCount; ++field) {
        auto observed = exact_observation();
        switch (field) {
        case 0: observed.protection.is_system = true; break;
        case 1: observed.protection.is_boot = true; break;
        case 2: observed.protection.boots_from_disk = true; break;
        case 3: observed.protection.has_pagefile = true; break;
        case 4: observed.protection.has_hibernation_file = true; break;
        case 5: observed.protection.has_crash_dump = true; break;
        case 6: observed.protection.is_clustered = true; break;
        case 7: observed.protection.is_highly_available = true; break;
        case 8: observed.protection.is_shadow_copy = true; break;
        default: REQUIRE(false);
        }
        const std::array observations{observed};
        const auto assessed = assess_new_disk_provisioning(
            expectation(), observations);
        REQUIRE(!assessed.ok());
        require_error(
            assessed.error(), ProvisioningPolicyError::protected_disk);
    }
}

TEST_CASE(provisioning_policy_requires_online_writable_disk) {
    auto observed = exact_observation();
    observed.protection.is_read_only = true;
    const std::array read_only{observed};
    auto assessed = assess_new_disk_provisioning(expectation(), read_only);
    REQUIRE(!assessed.ok());
    require_error(assessed.error(), ProvisioningPolicyError::read_only);

    observed = exact_observation();
    observed.protection.is_offline = true;
    const std::array offline{observed};
    assessed = assess_new_disk_provisioning(expectation(), offline);
    REQUIRE(!assessed.ok());
    require_error(assessed.error(), ProvisioningPolicyError::offline);
}

TEST_CASE(provisioning_policy_rejects_incomplete_expectations) {
    const std::array observations{exact_observation()};
    auto expected = expectation();
    expected.operation_id =
        uuid("00000000-0000-0000-0000-000000000000");
    auto assessed = assess_new_disk_provisioning(expected, observations);
    REQUIRE(!assessed.ok());
    require_error(
        assessed.error(), ProvisioningPolicyError::invalid_expectation);

    expected = expectation();
    expected.physical_disk.path = L"R:";
    assessed = assess_new_disk_provisioning(expected, observations);
    REQUIRE(!assessed.ok());
    require_error(
        assessed.error(), ProvisioningPolicyError::invalid_expectation);
}

TEST_CASE(provisioning_policy_reports_not_found_without_an_anchor) {
    auto foreign = exact_observation();
    foreign.physical_disk.physical_drive_number = 99;
    foreign.physical_disk.path = L"\\\\.\\PhysicalDrive99";
    foreign.physical_disk.port_number = 19;
    foreign.physical_disk.path_id = 9;
    foreign.physical_disk.target_id = 9;
    foreign.physical_disk.lun = 9;
    const std::array observations{foreign};

    const auto assessed = assess_new_disk_provisioning(
        expectation(), observations);
    REQUIRE(!assessed.ok());
    require_error(assessed.error(), ProvisioningPolicyError::not_found);
}

TEST_CASE(provisioning_command_uses_fixed_system_powershell_and_bounded_io) {
    const auto command = build_provisioning_command(
        verified_new_disk(), L"C:\\Windows");
    REQUIRE(command.ok());
    REQUIRE_EQ(
        command.value().executable,
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    REQUIRE_EQ(
        command.value().arguments,
        (std::vector<std::wstring>{
            L"-NoLogo",
            L"-NoProfile",
            L"-NonInteractive",
            L"-ExecutionPolicy",
            L"Bypass",
            L"-EncodedCommand",
            command.value().arguments.back()}));
    REQUIRE_EQ(command.value().timeout, std::chrono::minutes(5));
    REQUIRE_EQ(command.value().maximum_stdout_bytes, 64ULL * 1024ULL);
    REQUIRE_EQ(command.value().maximum_stderr_bytes, 64ULL * 1024ULL);
    REQUIRE_EQ(command.value().category, std::string("provision.gpt_ntfs"));
}

TEST_CASE(provisioning_command_script_uses_only_verified_number_and_constants) {
    const auto command = build_provisioning_command(
        verified_new_disk(), L"C:\\Windows");
    REQUIRE(command.ok());
    const auto script = decode_encoded_command(
        command.value().arguments.back());
    const auto folded = lower(script);

    REQUIRE(script.find(L"$diskNumber=[uint32]42") != std::wstring::npos);
    REQUIRE(script.find(L"$expectedSize=[uint64]8589934592")
        != std::wstring::npos);
    REQUIRE(script.find(L"$expectedDiskGuid=[guid]'")
        != std::wstring::npos);
    REQUIRE(script.find(L"$expectedPartitionGuid=[guid]'")
        != std::wstring::npos);
    REQUIRE(script.find(L"Get-Partition")
        != std::wstring::npos);
    REQUIRE(script.find(L"$_.Guid -eq $expectedPartitionGuid")
        != std::wstring::npos);
    REQUIRE(script.find(L"Format-Volume -Partition $partition")
        != std::wstring::npos);
    REQUIRE(script.find(L"-FileSystem NTFS") != std::wstring::npos);
    REQUIRE(script.find(L"MYRD_PROVISIONING_V1") != std::wstring::npos);
    REQUIRE(script.find(L"disk_guid=") != std::wstring::npos);
    REQUIRE(script.find(L"partition_guid=") != std::wstring::npos);
    REQUIRE(script.find(L"volume_guid=") != std::wstring::npos);

    REQUIRE(folded.find(L"physicaldrive") == std::wstring::npos);
    REQUIRE(folded.find(L"diskpart") == std::wstring::npos);
    REQUIRE(folded.find(L"initialize-disk") == std::wstring::npos);
    REQUIRE(folded.find(L"new-partition") == std::wstring::npos);
    REQUIRE(folded.find(L"invoke-expression") == std::wstring::npos);
    REQUIRE(folded.find(L"start-process") == std::wstring::npos);
    REQUIRE(folded.find(L"cmd.exe") == std::wstring::npos);
    REQUIRE(folded.find(L"00112233-4455-4677-8899-aabbccddeeff")
        == std::wstring::npos);
    REQUIRE(folded.find(L"10213243-5465-4687-98a9-bacbdcedfe0f")
        == std::wstring::npos);
    REQUIRE(folded.find(L"30000000-0000-4000-8000-000000000001")
        == std::wstring::npos);
}

TEST_CASE(provisioning_command_script_rechecks_every_available_guard) {
    const auto command = build_provisioning_command(
        verified_new_disk(), L"C:\\Windows");
    REQUIRE(command.ok());
    const auto script = decode_encoded_command(
        command.value().arguments.back());
    for (const std::wstring_view required : {
             L"@($disk).Count -ne 1",
             L"$disk.Number -ne $diskNumber",
             L"[uint64]$disk.Size -ne $expectedSize",
             L"[string]$disk.PartitionStyle -ne 'GPT'",
             L"[guid]$disk.Guid -ne $expectedDiskGuid",
             L"@($partitions).Count -ne 1",
             L"[guid]$partition.Guid -ne $expectedPartitionGuid",
             L"$disk.IsSystem",
             L"$disk.IsBoot",
             L"$disk.BootFromDisk",
             L"$disk.IsClustered",
             L"$disk.IsHighlyAvailable",
             L"$disk.IsReadOnly",
             L"$disk.IsOffline",
             L"Get-Partition",
             L"$ErrorActionPreference='Stop'"}) {
        REQUIRE(script.find(required) != std::wstring::npos);
    }
}

TEST_CASE(gpt_layout_plan_is_deterministic_aligned_and_operation_bound) {
    const auto first = testing::plan_windows_gpt_layout(
        expectation().operation_id,
        8ULL << 30,
        17ULL * 1024ULL,
        (8ULL << 30) - 34ULL * 1024ULL,
        4096);
    const auto second = testing::plan_windows_gpt_layout(
        expectation().operation_id,
        8ULL << 30,
        17ULL * 1024ULL,
        (8ULL << 30) - 34ULL * 1024ULL,
        4096);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE_EQ(first.value(), second.value());
    REQUIRE_EQ(first.value().partition_starting_offset, 1ULL << 20);
    REQUIRE(first.value().partition_length_bytes > 0);
    REQUIRE_EQ(first.value().partition_length_bytes % 4096, 0U);
    REQUIRE(first.value().partition_starting_offset
        + first.value().partition_length_bytes <= 8ULL << 30);
    REQUIRE(first.value().disk_guid != first.value().partition_guid);

    auto other = expectation();
    other.operation_id = uuid("30000000-0000-4000-8000-000000000002");
    const auto different = testing::plan_windows_gpt_layout(
        other.operation_id,
        8ULL << 30,
        17ULL * 1024ULL,
        (8ULL << 30) - 34ULL * 1024ULL,
        4096);
    REQUIRE(different.ok());
    REQUIRE(different.value().disk_guid != first.value().disk_guid);
    REQUIRE(different.value().partition_guid
        != first.value().partition_guid);
}

TEST_CASE(gpt_layout_plan_rejects_invalid_or_too_small_geometry) {
    for (const auto& request : std::array{
             std::array<std::uint64_t, 4>{0, 0, 0, 512},
             std::array<std::uint64_t, 4>{8ULL << 30, 0, 0, 512},
             std::array<std::uint64_t, 4>{8ULL << 30, 4096, 4096, 512},
             std::array<std::uint64_t, 4>{8ULL << 30, 4096,
                                          8ULL << 30, 123}}) {
        const auto planned = testing::plan_windows_gpt_layout(
            expectation().operation_id,
            request[0], request[1], request[2],
            static_cast<std::uint32_t>(request[3]));
        REQUIRE(!planned.ok());
    }
}

TEST_CASE(provisioning_command_is_deterministic_and_rejects_untrusted_root) {
    const auto first = build_provisioning_command(
        verified_new_disk(), L"C:\\Windows");
    const auto second = build_provisioning_command(
        verified_new_disk(), L"C:\\Windows");
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE_EQ(first.value(), second.value());

    for (const std::wstring_view invalid : {
             L"Windows",
             L"C:\\Windows\\",
             L"C:\\Windows\\..\\Temp",
             L"\\\\server\\Windows",
             L"C:/Windows"}) {
        const auto rejected = build_provisioning_command(
            verified_new_disk(), invalid);
        REQUIRE(!rejected.ok());
        require_error(
            rejected.error(), ProvisioningPolicyError::invalid_environment);
    }
}

namespace {

std::string successful_process_output() {
    const auto planned = testing::plan_windows_gpt_layout(
        expectation().operation_id,
        8ULL << 30,
        17ULL * 1024ULL,
        (8ULL << 30) - 34ULL * 1024ULL,
        4096);
    REQUIRE(planned.ok());
    return
        "MYRD_PROVISIONING_V1\r\n"
        "disk_guid=" + planned.value().disk_guid.to_string() + "\r\n"
        "partition_guid=" + planned.value().partition_guid.to_string()
        + "\r\n"
        "volume_guid=40000000-0000-4000-8000-000000000003\r\n";
}

Error execution_error(ProvisioningExecutionError code) {
    return {
        ErrorDomain::internal,
        static_cast<std::uint32_t>(code),
        L"FakeProvisioningSession",
        L"injected provisioning failure"};
}

struct ProvisioningProbe final {
    std::vector<std::string> events;
    NewDiskObservation observation = exact_observation();
    ProvisioningProcessResult process{
        0, false, false, false, successful_process_output(), {}};
    ProvisioningMutationAudit audit{
        ProvisioningMutationState::raw_unchanged,
        expected_disk(),
        ObservedDiskLayout::raw,
        0,
        0,
        0,
        false,
        false,
        false,
        false};
    std::optional<Error> open_error;
    std::optional<Error> observe_error;
    std::optional<Error> run_error;
    std::optional<Error> audit_error;
    std::optional<Error> postcondition_error;
    std::optional<VolumeIdentityExpectation> postcondition_expectation;
    std::stop_source* cancel_during_run{};
};

ProvisioningMutationAudit successful_postformat_audit() {
    return {
        ProvisioningMutationState::planned_partition_with_volume,
        expected_disk(),
        ObservedDiskLayout::gpt,
        1,
        1,
        1,
        true,
        true,
        true,
        true};
}

class FakeProvisioningSession final : public IProvisioningSession {
public:
    explicit FakeProvisioningSession(std::shared_ptr<ProvisioningProbe> probe)
        : probe_(std::move(probe)) {}

    Result<NewDiskObservation> observe(
        const OperationContext&) override {
        probe_->events.emplace_back("observe");
        if (probe_->observe_error.has_value()) {
            return Result<NewDiskObservation>::failure(
                *probe_->observe_error);
        }
        return Result<NewDiskObservation>::success(probe_->observation);
    }

    Result<ProvisioningProcessResult> run(
        const ProvisioningCommand&,
        const OperationContext&) override {
        probe_->events.emplace_back("run");
        if (probe_->cancel_during_run != nullptr) {
            probe_->cancel_during_run->request_stop();
        }
        if (probe_->run_error.has_value()) {
            return Result<ProvisioningProcessResult>::failure(
                *probe_->run_error);
        }
        return Result<ProvisioningProcessResult>::success(probe_->process);
    }

    Result<ProvisioningMutationAudit> audit_after_mutation(
        const OperationContext&) override {
        probe_->events.emplace_back("audit");
        if (probe_->audit_error.has_value()) {
            return Result<ProvisioningMutationAudit>::failure(
                *probe_->audit_error);
        }
        return Result<ProvisioningMutationAudit>::success(probe_->audit);
    }

    Result<VerifiedVolumeIdentity> verify_postcondition(
        const VolumeIdentityExpectation& expected,
        const OperationContext&) override {
        probe_->events.emplace_back("verify");
        probe_->postcondition_expectation = expected;
        if (probe_->postcondition_error.has_value()) {
            return Result<VerifiedVolumeIdentity>::failure(
                *probe_->postcondition_error);
        }
        const VolumeIdentityObservation observation{
            L"\\\\?\\Volume{40000000-0000-4000-8000-000000000003}\\",
            expected.volume_guid,
            0xA1B2C3D4,
            L"NTFS",
            kStorageDeviceTypeDisk,
            expected.physical_disk.physical_drive_number,
            1,
            {{expected.physical_disk.physical_drive_number,
              1ULL << 20,
              expected.physical_disk.length_bytes - (1ULL << 20)}},
            expected.physical_disk,
            ObservedPartitionStyle::gpt,
            expected.gpt_disk_guid,
            1,
            expected.partition_guid,
            1ULL << 20,
            expected.physical_disk.length_bytes - (1ULL << 20)};
        const std::array observations{observation};
        return match_volume_identity(expected, observations);
    }

private:
    std::shared_ptr<ProvisioningProbe> probe_;
};

class FakeProvisioningPlatform final : public IProvisioningPlatform {
public:
    explicit FakeProvisioningPlatform(std::shared_ptr<ProvisioningProbe> probe)
        : probe_(std::move(probe)) {}

    Result<std::unique_ptr<IProvisioningSession>> open_exact(
        const NewDiskProvisioningExpectation&,
        const OperationContext&) override {
        probe_->events.emplace_back("open");
        if (probe_->open_error.has_value()) {
            return Result<std::unique_ptr<IProvisioningSession>>::failure(
                *probe_->open_error);
        }
        std::unique_ptr<IProvisioningSession> session =
            std::make_unique<FakeProvisioningSession>(probe_);
        return Result<std::unique_ptr<IProvisioningSession>>::success(
            std::move(session));
    }

    Result<std::wstring> windows_directory() override {
        probe_->events.emplace_back("windows_directory");
        return Result<std::wstring>::success(L"C:\\Windows");
    }

private:
    std::shared_ptr<ProvisioningProbe> probe_;
};

} // namespace

TEST_CASE(provisioning_result_parser_accepts_only_canonical_complete_output) {
    const auto parsed = parse_provisioning_result(successful_process_output());
    REQUIRE(parsed.ok());
    REQUIRE_EQ(
        parsed.value().gpt_disk_guid,
        testing::plan_windows_gpt_layout(
            expectation().operation_id,
            8ULL << 30,
            17ULL * 1024ULL,
            (8ULL << 30) - 34ULL * 1024ULL,
            4096).value().disk_guid);
    REQUIRE_EQ(
        parsed.value().partition_guid,
        testing::plan_windows_gpt_layout(
            expectation().operation_id,
            8ULL << 30,
            17ULL * 1024ULL,
            (8ULL << 30) - 34ULL * 1024ULL,
            4096).value().partition_guid);
    REQUIRE_EQ(
        parsed.value().volume_guid,
        uuid("40000000-0000-4000-8000-000000000003"));
}

TEST_CASE(provisioning_result_parser_rejects_missing_duplicate_unknown_or_paths) {
    const std::array malformed{
        std::string{},
        std::string("MYRD_PROVISIONING_V1\n"),
        successful_process_output() + "unknown=value\r\n",
        successful_process_output()
            + "volume_guid=40000000-0000-4000-8000-000000000003\r\n",
        std::string(
            "MYRD_PROVISIONING_V1\n"
            "disk_guid=40000000-0000-4000-8000-000000000001\n"
            "partition_guid=40000000-0000-4000-8000-000000000002\n"
            "volume_guid=\\\\?\\Volume{40000000-0000-4000-8000-000000000003}\\\n"),
        std::string(
            "MYRD_PROVISIONING_V1\n"
            "disk_guid=00000000-0000-0000-0000-000000000000\n"
            "partition_guid=40000000-0000-4000-8000-000000000002\n"
            "volume_guid=40000000-0000-4000-8000-000000000003\n")};
    for (const auto& text : malformed) {
        const auto parsed = parse_provisioning_result(text);
        REQUIRE(!parsed.ok());
        require_error(
            parsed.error(), ProvisioningPolicyError::malformed_result);
    }
}

TEST_CASE(provisioning_operation_orders_preflight_mutation_and_postcondition) {
    auto probe = std::make_shared<ProvisioningProbe>();
    probe->audit = successful_postformat_audit();
    FakeProvisioningPlatform platform(probe);

    const auto outcome = provision_new_disk(platform, expectation(), {});

    REQUIRE(outcome.ok());
    REQUIRE(outcome.opened);
    REQUIRE(outcome.preflight_verified);
    REQUIRE(outcome.process_started);
    REQUIRE(outcome.process_completed);
    REQUIRE(outcome.postmutation_audited);
    REQUIRE(outcome.audit.has_value());
    REQUIRE_EQ(
        outcome.audit->state,
        ProvisioningMutationState::planned_partition_with_volume);
    REQUIRE(outcome.audit->planned_partition_layout_matches);
    REQUIRE(outcome.audit->volume_extent_matches);
    REQUIRE(outcome.postcondition_verified);
    REQUIRE(outcome.proof.has_value());
    REQUIRE_EQ(
        outcome.proof->operation_id(), expectation().operation_id);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{
            "open", "observe", "windows_directory", "run", "verify",
            "audit"}));
    REQUIRE(probe->postcondition_expectation.has_value());
    REQUIRE_EQ(
        probe->postcondition_expectation->owner,
        expectation().owner);
    REQUIRE_EQ(
        probe->postcondition_expectation->physical_disk,
        expectation().physical_disk);
}

TEST_CASE(provisioning_operation_never_runs_after_policy_or_context_failure) {
    auto probe = std::make_shared<ProvisioningProbe>();
    probe->observation.layout = ObservedDiskLayout::gpt;
    FakeProvisioningPlatform platform(probe);
    auto outcome = provision_new_disk(platform, expectation(), {});
    REQUIRE(!outcome.ok());
    REQUIRE(!outcome.process_started);
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{"open", "observe"}));

    probe = std::make_shared<ProvisioningProbe>();
    FakeProvisioningPlatform cancelled_platform(probe);
    std::stop_source cancelled;
    cancelled.request_stop();
    outcome = provision_new_disk(
        cancelled_platform,
        expectation(),
        {cancelled.get_token(),
         std::chrono::steady_clock::time_point::max()});
    REQUIRE(!outcome.ok());
    REQUIRE(probe->events.empty());
}

TEST_CASE(provisioning_operation_rejects_failed_truncated_or_malformed_child) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        auto probe = std::make_shared<ProvisioningProbe>();
        if (scenario == 0) probe->process.exit_code = 5;
        if (scenario == 1) probe->process.timed_out = true;
        if (scenario == 2) probe->process.stdout_truncated = true;
        if (scenario == 3) probe->process.stderr_truncated = true;
        if (scenario == 4) probe->process.stdout_text = "bad";
        FakeProvisioningPlatform platform(probe);

        const auto outcome = provision_new_disk(platform, expectation(), {});
        REQUIRE(!outcome.ok());
        REQUIRE(outcome.process_started);
        REQUIRE(outcome.postmutation_audited);
        REQUIRE(outcome.audit.has_value());
        REQUIRE_EQ(
            outcome.audit->state,
            ProvisioningMutationState::raw_unchanged);
        REQUIRE(!outcome.postcondition_verified);
        REQUIRE(!outcome.proof.has_value());
        REQUIRE_EQ(probe->events.back(), "audit");
    }
}

TEST_CASE(provisioning_operation_mandatorily_verifies_after_late_cancel) {
    auto probe = std::make_shared<ProvisioningProbe>();
    probe->audit = successful_postformat_audit();
    std::stop_source cancelled;
    probe->cancel_during_run = &cancelled;
    FakeProvisioningPlatform platform(probe);

    const auto outcome = provision_new_disk(
        platform,
        expectation(),
        {cancelled.get_token(),
         std::chrono::steady_clock::time_point::max()});

    REQUIRE(cancelled.stop_requested());
    REQUIRE(outcome.ok());
    REQUIRE(outcome.postmutation_audited);
    REQUIRE(outcome.audit.has_value());
    REQUIRE(outcome.postcondition_verified);
    REQUIRE_EQ(probe->events.back(), "audit");
}

TEST_CASE(provisioning_operation_never_claims_success_on_postcondition_failure) {
    auto probe = std::make_shared<ProvisioningProbe>();
    probe->postcondition_error = execution_error(
        ProvisioningExecutionError::postcondition_failed);
    FakeProvisioningPlatform platform(probe);

    const auto outcome = provision_new_disk(platform, expectation(), {});

    REQUIRE(!outcome.ok());
    REQUIRE(outcome.process_completed);
    REQUIRE(outcome.postmutation_audited);
    REQUIRE(outcome.audit.has_value());
    REQUIRE(!outcome.postcondition_verified);
    REQUIRE(!outcome.proof.has_value());
    REQUIRE_EQ(probe->events.back(), "audit");
}

TEST_CASE(provisioning_operation_requires_exact_final_postformat_audit) {
    auto probe = std::make_shared<ProvisioningProbe>();
    probe->audit = successful_postformat_audit();
    probe->audit.state = ProvisioningMutationState::unexpected_layout;
    probe->audit.planned_partition_layout_matches = false;
    FakeProvisioningPlatform platform(probe);

    const auto outcome = provision_new_disk(platform, expectation(), {});

    REQUIRE(!outcome.ok());
    REQUIRE(outcome.process_completed);
    REQUIRE(outcome.postcondition_verified);
    REQUIRE(outcome.postmutation_audited);
    REQUIRE(outcome.audit.has_value());
    REQUIRE_EQ(
        outcome.audit->state,
        ProvisioningMutationState::unexpected_layout);
    REQUIRE(!outcome.proof.has_value());
    REQUIRE(outcome.error.has_value());
    REQUIRE_EQ(probe->events.back(), "audit");
}

TEST_CASE(provisioning_operation_audits_platform_failure_after_launch) {
    auto probe = std::make_shared<ProvisioningProbe>();
    probe->run_error = execution_error(
        ProvisioningExecutionError::process_failed);
    FakeProvisioningPlatform platform(probe);

    const auto outcome = provision_new_disk(platform, expectation(), {});

    REQUIRE(!outcome.ok());
    REQUIRE(outcome.process_started);
    REQUIRE(outcome.postmutation_audited);
    REQUIRE(!outcome.proof.has_value());
    REQUIRE_EQ(
        probe->events,
        (std::vector<std::string>{
            "open", "observe", "windows_directory", "run", "audit"}));
}

TEST_CASE(provisioning_operation_reports_failed_postmutation_audit) {
    auto probe = std::make_shared<ProvisioningProbe>();
    probe->run_error = execution_error(
        ProvisioningExecutionError::process_failed);
    probe->audit_error = execution_error(
        ProvisioningExecutionError::postcondition_failed);
    FakeProvisioningPlatform platform(probe);

    const auto outcome = provision_new_disk(platform, expectation(), {});

    REQUIRE(!outcome.ok());
    REQUIRE(!outcome.postmutation_audited);
    REQUIRE(outcome.error.has_value());
    REQUIRE(outcome.audit_error.has_value());
    REQUIRE(!outcome.proof.has_value());
}

#ifdef MYRD_ENABLE_TEST_SEAMS
TEST_CASE(windows_provisioning_runner_captures_both_streams_and_exit_code) {
    const auto command = benign_command(
        L"[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
        L"[Console]::Error.WriteLine('MYRD_STDERR');"
        L"[Console]::Out.WriteLine('MYRD_STDOUT');exit 7");

    const auto result = testing::run_windows_provisioning_process(
        command, {});

    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().exit_code, 7U);
    REQUIRE(!result.value().timed_out);
    REQUIRE(!result.value().stdout_truncated);
    REQUIRE(!result.value().stderr_truncated);
    REQUIRE(result.value().stdout_text.find("MYRD_STDOUT")
        != std::string::npos);
    REQUIRE(result.value().stderr_text.find("MYRD_STDERR")
        != std::string::npos);
}

TEST_CASE(windows_provisioning_runner_drains_and_marks_bounded_output) {
    const auto command = benign_command(
        L"[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
        L"[Console]::Out.Write(('o'*4096));"
        L"[Console]::Error.Write(('e'*4096))",
        std::chrono::seconds(5), 31, 47);

    const auto result = testing::run_windows_provisioning_process(
        command, {});

    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().exit_code, 0U);
    REQUIRE(result.value().stdout_truncated);
    REQUIRE(result.value().stderr_truncated);
    REQUIRE_EQ(result.value().stdout_text.size(), 31U);
    REQUIRE_EQ(result.value().stderr_text.size(), 47U);
}

TEST_CASE(windows_provisioning_runner_enforces_timeout_and_entry_context) {
    const auto slow = benign_command(
        L"Start-Sleep -Seconds 5",
        std::chrono::milliseconds(100));
    const auto timed = testing::run_windows_provisioning_process(slow, {});
    REQUIRE(timed.ok());
    REQUIRE(timed.value().timed_out);

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto rejected = testing::run_windows_provisioning_process(
        benign_command(L"exit 0"),
        {cancelled.get_token(),
         std::chrono::steady_clock::time_point::max()});
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(
        rejected.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));
}

TEST_CASE(windows_disk_observer_is_fixed_read_only_and_bounded) {
    const auto command = testing::build_windows_disk_observation_command(
        expected_disk(), L"C:\\Windows");
    REQUIRE(command.ok());
    REQUIRE_EQ(command.value().timeout, std::chrono::seconds(30));
    REQUIRE_EQ(command.value().maximum_stdout_bytes, 4096U);
    REQUIRE_EQ(command.value().maximum_stderr_bytes, 4096U);
    REQUIRE_EQ(command.value().category, std::string("observe.new_disk"));
    const auto script = decode_encoded_command(
        command.value().arguments.back());
    const auto folded = lower(script);
    REQUIRE(script.find(L"$diskNumber=[uint32]42") != std::wstring::npos);
    REQUIRE(script.find(L"$expectedSize=[uint64]8589934592")
        != std::wstring::npos);
    REQUIRE(script.find(L"Get-Disk -Number $diskNumber")
        != std::wstring::npos);
    REQUIRE(script.find(L"MYRD_DISK_OBSERVATION_V1")
        != std::wstring::npos);
    for (const std::wstring_view forbidden : {
             L"initialize-disk", L"new-partition", L"format-volume",
             L"clear-disk", L"set-disk", L"remove-partition"}) {
        REQUIRE(folded.find(forbidden) == std::wstring::npos);
    }
}

TEST_CASE(windows_disk_observation_parser_reconciles_handle_and_storage_data) {
    const std::string output =
        "MYRD_DISK_OBSERVATION_V1\r\n"
        "number=42\r\n"
        "size=8589934592\r\n"
        "partition_style=RAW\r\n"
        "partition_count=0\r\n"
        "is_system=0\r\n"
        "is_boot=0\r\n"
        "boots_from_disk=0\r\n"
        "is_clustered=0\r\n"
        "is_highly_available=0\r\n"
        "is_read_only=0\r\n"
        "is_offline=0\r\n";

    const auto observed = testing::parse_windows_disk_observation(
        expected_disk(), ObservedDiskLayout::raw, 0, 0, 0,
        false, false, output);
    REQUIRE(observed.ok());
    REQUIRE_EQ(observed.value(), exact_observation());

    const auto changed_size = testing::parse_windows_disk_observation(
        expected_disk(), ObservedDiskLayout::raw, 0, 0, 0, false, false,
        std::string(output).replace(
            output.find("8589934592"), 10, "8589934593"));
    REQUIRE(!changed_size.ok());
    const auto changed_native_layout = testing::parse_windows_disk_observation(
        expected_disk(), ObservedDiskLayout::gpt, 1, 1, 1,
        false, false, output);
    REQUIRE(!changed_native_layout.ok());
}

TEST_CASE(windows_disk_observation_parser_rejects_malformed_and_protected_data) {
    const std::string prefix =
        "MYRD_DISK_OBSERVATION_V1\nnumber=42\nsize=8589934592\n"
        "partition_style=RAW\npartition_count=0\n";
    const std::string suffix =
        "is_boot=0\nboots_from_disk=0\nis_clustered=0\n"
        "is_highly_available=0\nis_read_only=0\nis_offline=0\n";
    auto observed = testing::parse_windows_disk_observation(
        expected_disk(), ObservedDiskLayout::raw, 0, 0, 0, false, false,
        prefix + "is_system=1\n" + suffix);
    REQUIRE(observed.ok());
    REQUIRE(observed.value().protection.is_system);

    observed = testing::parse_windows_disk_observation(
        expected_disk(), ObservedDiskLayout::raw, 0, 0, 0, true, false,
        prefix + "is_system=0\n" + suffix);
    REQUIRE(!observed.ok());

    for (const std::string malformed : {
             std::string{},
             prefix + "is_system=yes\n" + suffix,
             prefix + "is_system=0\n" + suffix + "unknown=0\n"}) {
        observed = testing::parse_windows_disk_observation(
            expected_disk(), ObservedDiskLayout::raw, 0, 0, 0,
            false, false,
            malformed);
        REQUIRE(!observed.ok());
    }
}

TEST_CASE(windows_disk_observation_uses_independent_volume_extent_evidence) {
    const std::string output =
        "MYRD_DISK_OBSERVATION_V1\nnumber=42\nsize=8589934592\n"
        "partition_style=RAW\npartition_count=0\n"
        "is_system=0\nis_boot=0\nboots_from_disk=0\n"
        "is_clustered=0\nis_highly_available=0\n"
        "is_read_only=0\nis_offline=0\n";
    const auto observed = testing::parse_windows_disk_observation(
        expected_disk(), ObservedDiskLayout::raw, 0, 1, 2,
        false, false, output);
    REQUIRE(observed.ok());
    REQUIRE_EQ(observed.value().volume_count, 1U);
    REQUIRE_EQ(observed.value().extent_count, 2U);
    REQUIRE(observed.value().protection.has_pagefile);
    REQUIRE(observed.value().protection.has_hibernation_file);
    REQUIRE(observed.value().protection.has_crash_dump);
    REQUIRE(observed.value().protection.is_shadow_copy);
}

TEST_CASE(provisioning_volume_extent_policy_skips_only_optical_devices) {
    constexpr std::uint32_t file_device_cd_rom = 0x00000002;
    constexpr std::uint32_t file_device_disk = 0x00000007;
    constexpr std::uint32_t unknown_device_type = 0xFFFFFFFF;

    REQUIRE(!testing::provisioning_volume_requires_disk_extents(
        file_device_cd_rom));
    REQUIRE(testing::provisioning_volume_requires_disk_extents(
        file_device_disk));
    REQUIRE(testing::provisioning_volume_requires_disk_extents(
        unknown_device_type));
}

TEST_CASE(provisioning_layout_normalizes_only_zeroed_empty_mbr_to_raw) {
    constexpr std::uint32_t partition_style_mbr = 0;
    constexpr std::uint32_t partition_style_gpt = 1;
    constexpr std::uint32_t partition_style_raw = 2;

    REQUIRE_EQ(
        testing::classify_windows_provisioning_layout(
            partition_style_mbr, 0, 0, 0),
        ObservedDiskLayout::raw);
    REQUIRE_EQ(
        testing::classify_windows_provisioning_layout(
            partition_style_mbr, 0, 1, 0),
        ObservedDiskLayout::mbr);
    REQUIRE_EQ(
        testing::classify_windows_provisioning_layout(
            partition_style_mbr, 0, 0, 1),
        ObservedDiskLayout::mbr);
    REQUIRE_EQ(
        testing::classify_windows_provisioning_layout(
            partition_style_mbr, 1, 0, 0),
        ObservedDiskLayout::mbr);
    REQUIRE_EQ(
        testing::classify_windows_provisioning_layout(
            partition_style_gpt, 0, 0, 0),
        ObservedDiskLayout::gpt);
    REQUIRE_EQ(
        testing::classify_windows_provisioning_layout(
            partition_style_raw, 0, 0, 0),
        ObservedDiskLayout::raw);
}

TEST_CASE(windows_provisioning_platform_factory_is_nonmutating_until_open) {
    auto platform = make_windows_provisioning_platform();
    REQUIRE(platform != nullptr);
    const auto directory = platform->windows_directory();
    REQUIRE(directory.ok());
    REQUIRE(!directory.value().empty());

    auto invalid = expectation();
    invalid.physical_disk.path = L"R:";
    const auto opened = platform->open_exact(invalid, {});
    REQUIRE(!opened.ok());
}

TEST_CASE(windows_provisioning_requires_explicit_disposable_target_tuple) {
    const auto enabled = environment(
        L"MYRD_M4_ALLOW_DESTRUCTIVE_PROVISIONING");
    if (!enabled.has_value() || *enabled != L"1") return;
    const auto confirmation = environment(
        L"MYRD_M4_PROVISIONING_CONFIRM");
    REQUIRE(confirmation.has_value());
    REQUIRE_EQ(
        *confirmation,
        L"FORMAT_ONLY_THE_EXACT_VERIFIED_DISPOSABLE_DISK");

    const auto number_value = environment_u64(
        L"MYRD_M4_PROVISIONING_PHYSICAL_DRIVE_NUMBER");
    const auto port_value = environment_u64(
        L"MYRD_M4_PROVISIONING_SCSI_PORT_NUMBER");
    const auto path_value = environment_u64(
        L"MYRD_M4_PROVISIONING_SCSI_PATH_ID");
    const auto target_value = environment_u64(
        L"MYRD_M4_PROVISIONING_SCSI_TARGET_ID");
    const auto lun_value = environment_u64(
        L"MYRD_M4_PROVISIONING_SCSI_LUN");
    const auto device_value = environment_u64(
        L"MYRD_M4_PROVISIONING_AIM_DEVICE_ID");
    REQUIRE(number_value <= (std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(port_value <= (std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(path_value <= (std::numeric_limits<std::uint8_t>::max)());
    REQUIRE(target_value <= (std::numeric_limits<std::uint8_t>::max)());
    REQUIRE(lun_value <= (std::numeric_limits<std::uint8_t>::max)());
    REQUIRE(device_value <= (std::numeric_limits<std::uint32_t>::max)());
    const aim::DeviceId aim_device{
        static_cast<std::uint32_t>(device_value)};
    const auto decoded = aim::decode_device_address(aim_device);
    REQUIRE_EQ(decoded.path_id, static_cast<std::uint8_t>(path_value));
    REQUIRE_EQ(decoded.target_id, static_cast<std::uint8_t>(target_value));
    REQUIRE_EQ(decoded.lun, static_cast<std::uint8_t>(lun_value));

    const auto number = static_cast<std::uint32_t>(number_value);
    const NewDiskProvisioningExpectation expected{
        {environment_uuid(L"MYRD_M4_PROVISIONING_PROFILE_UUID"),
         environment_uuid(L"MYRD_M4_PROVISIONING_IMAGE_UUID"),
         environment_u64(L"MYRD_M4_PROVISIONING_PROFILE_REVISION"),
         environment_u64(L"MYRD_M4_PROVISIONING_SESSION_GENERATION"),
         aim_device},
        environment_uuid(L"MYRD_M4_PROVISIONING_OPERATION_UUID"),
        {number,
         L"\\\\.\\PhysicalDrive" + std::to_wstring(number),
         environment_u64(L"MYRD_M4_PROVISIONING_DISK_LENGTH_BYTES"),
         static_cast<std::uint32_t>(port_value),
         static_cast<std::uint8_t>(path_value),
         static_cast<std::uint8_t>(target_value),
         static_cast<std::uint8_t>(lun_value)},
        ProvisioningLifecycle::provisioning,
        ProvisioningSource::new_image,
        true,
        true,
        false,
        false,
        false};

    auto platform = make_windows_provisioning_platform();
    REQUIRE(platform != nullptr);
    const auto outcome = provision_new_disk(*platform, expected, {});
    if (!outcome.ok()) {
        std::wcout
            << L"MYRD_PROVISIONING_OUTCOME"
            << L" opened=" << outcome.opened
            << L" preflight_verified=" << outcome.preflight_verified
            << L" process_started=" << outcome.process_started
            << L" process_completed=" << outcome.process_completed
            << L" postmutation_audited=" << outcome.postmutation_audited
            << L" postcondition_verified=" << outcome.postcondition_verified
            << L'\n';
        if (outcome.error.has_value()) {
            std::wcout
                << L"MYRD_PROVISIONING_ERROR"
                << L" domain=" << static_cast<std::uint32_t>(
                    outcome.error->domain)
                << L" code=" << outcome.error->code
                << L" native_code=" << outcome.error->native_code
                << L" operation=" << outcome.error->operation
                << L" message=" << outcome.error->message
                << L'\n';
        }
        if (outcome.audit_error.has_value()) {
            std::wcout
                << L"MYRD_PROVISIONING_AUDIT_ERROR"
                << L" domain=" << static_cast<std::uint32_t>(
                    outcome.audit_error->domain)
                << L" code=" << outcome.audit_error->code
                << L" native_code=" << outcome.audit_error->native_code
                << L" operation=" << outcome.audit_error->operation
                << L" message=" << outcome.audit_error->message
                << L'\n';
        }
        if (outcome.audit.has_value()) {
            std::wcout
                << L"MYRD_PROVISIONING_AUDIT"
                << L" state=" << static_cast<std::uint32_t>(
                    outcome.audit->state)
                << L" layout=" << static_cast<std::uint32_t>(
                    outcome.audit->layout)
                << L" partitions=" << outcome.audit->partition_count
                << L" volumes=" << outcome.audit->volume_count
                << L" extents=" << outcome.audit->extent_count
                << L'\n';
        }
        std::wcout.flush();
    }
    REQUIRE(outcome.ok());
    REQUIRE(outcome.proof.has_value());
    REQUIRE_EQ(outcome.proof->operation_id(), expected.operation_id);
    REQUIRE_EQ(
        outcome.proof->identity().owner(), expected.owner);
    REQUIRE_EQ(
        outcome.proof->identity().observation().physical_disk,
        expected.physical_disk);
    REQUIRE_EQ(
        outcome.proof->identity().observation().filesystem_name,
        L"NTFS");
}
#endif
