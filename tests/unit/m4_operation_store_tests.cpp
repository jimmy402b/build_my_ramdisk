#include "tests/support/test_framework.h"

#include "src/management/operation_store.h"

#define NOMINMAX
#include <Windows.h>
#include <Sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

std::array<std::byte, 32> argument_digest() {
    std::array<std::byte, 32> digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        digest[index] = static_cast<std::byte>(index + 1U);
    }
    return digest;
}

std::wstring uuid_wstring(const myrd::product::Uuid& value) {
    const auto text = value.to_string();
    return std::wstring(text.begin(), text.end());
}

myrd::management::OperationRecord queued_record() {
    return myrd::management::OperationRecord{
        1,
        uuid("10000000-0000-4000-8000-000000000001"),
        uuid("20000000-0000-4000-8000-000000000002"),
        uuid("30000000-0000-4000-8000-000000000003"),
        L"S-1-5-21-100-200-300-1001",
        "disk.checkpoint",
        argument_digest(),
        myrd::management::OperationState::queued,
        "queued",
        1700000000000ULL,
        1700000000000ULL,
        std::nullopt,
        true,
        false,
        {}};
}

class TestDirectory final {
public:
    TestDirectory() {
        static std::atomic_uint32_t sequence{};
        root_ = std::filesystem::temp_directory_path()
            / (L"myrd-m4-operation-store-"
               + std::to_wstring(GetCurrentProcessId()) + L"-"
               + std::to_wstring(sequence.fetch_add(1)));
        REQUIRE(std::filesystem::create_directories(program_data()));
    }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    std::filesystem::path program_data() const {
        return root_ / L"ProgramData";
    }

private:
    std::filesystem::path root_;
};

void create_secure_directory(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
        &descriptor, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const auto error = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    REQUIRE(created || error == ERROR_ALREADY_EXISTS);
}

void create_operation_layout(const std::filesystem::path& program_data) {
    create_secure_directory(program_data / L"MyRamDisk");
    create_secure_directory(program_data / L"MyRamDisk" / L"operations");
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    REQUIRE(stream.good());
    const auto size = stream.tellg();
    REQUIRE(size >= 0);
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(result.data()), size);
    REQUIRE(stream.good());
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

myrd::management::OperationRecord terminal_record(
    myrd::management::OperationRecord value,
    std::uint64_t terminal_time) {
    ++value.revision;
    value.state = myrd::management::OperationState::succeeded;
    value.phase = "terminal";
    value.updated_unix_ms = terminal_time;
    value.terminal_unix_ms = terminal_time;
    value.cancellable = false;
    value.terminal_result = {std::byte{'o'}, std::byte{'k'}};
    return value;
}

myrd::product::Uuid numbered_uuid(std::uint32_t value) {
    char text[37]{};
    const int written = std::snprintf(
        text, sizeof(text), "90000000-0000-4000-8000-%012x", value);
    REQUIRE_EQ(written, 36);
    return uuid(text);
}

std::uint64_t read_u64(
    std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

} // namespace

TEST_CASE(operation_record_v1_has_a_canonical_checksummed_fixture) {
    const auto record = queued_record();
    auto encoded = myrd::management::encode_operation_record(record);
    REQUIRE(encoded.ok());
    REQUIRE(encoded.value().size() >= myrd::management::kOperationHeaderBytes);
    REQUIRE(std::equal(
        myrd::management::kOperationMagic.begin(),
        myrd::management::kOperationMagic.end(),
        encoded.value().begin()));
    REQUIRE_EQ(read_u64(encoded.value(), 16), encoded.value().size());
    REQUIRE_EQ(read_u64(encoded.value(), 24), record.revision);

    auto repeated = myrd::management::encode_operation_record(record);
    REQUIRE(repeated.ok());
    REQUIRE_EQ(repeated.value(), encoded.value());
    auto decoded = myrd::management::decode_operation_record(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), record);
}

TEST_CASE(operation_record_v1_round_trips_an_exact_terminal_result) {
    auto record = queued_record();
    record.revision = 4;
    record.state = myrd::management::OperationState::succeeded;
    record.phase = "terminal";
    record.updated_unix_ms = 1700000004000ULL;
    record.terminal_unix_ms = record.updated_unix_ms;
    record.cancellable = false;
    record.terminal_result = {
        std::byte{'{'}, std::byte{'"'}, std::byte{'o'}, std::byte{'k'},
        std::byte{'"'}, std::byte{':'}, std::byte{'t'}, std::byte{'r'},
        std::byte{'u'}, std::byte{'e'}, std::byte{'}'}};
    auto encoded = myrd::management::encode_operation_record(record);
    REQUIRE(encoded.ok());
    auto decoded = myrd::management::decode_operation_record(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), record);
    REQUIRE_EQ(decoded.value().terminal_result, record.terminal_result);
}

TEST_CASE(operation_record_v1_rejects_corruption_and_invalid_state_contracts) {
    auto encoded = myrd::management::encode_operation_record(queued_record());
    REQUIRE(encoded.ok());
    auto corrupt = encoded.value();
    corrupt.back() ^= std::byte{0xff};
    REQUIRE(!myrd::management::decode_operation_record(corrupt).ok());
    corrupt = encoded.value();
    corrupt.pop_back();
    REQUIRE(!myrd::management::decode_operation_record(corrupt).ok());
    corrupt = encoded.value();
    corrupt.push_back(std::byte{0});
    REQUIRE(!myrd::management::decode_operation_record(corrupt).ok());

    auto invalid = queued_record();
    invalid.terminal_unix_ms = invalid.updated_unix_ms;
    REQUIRE(!myrd::management::encode_operation_record(invalid).ok());
    invalid = queued_record();
    invalid.caller_sid = L"not-a-sid";
    REQUIRE(!myrd::management::encode_operation_record(invalid).ok());
    invalid = queued_record();
    invalid.command_scope = "Disk.Checkpoint";
    REQUIRE(!myrd::management::encode_operation_record(invalid).ok());
}

TEST_CASE(operation_record_revision_is_checksum_bound) {
    auto encoded = myrd::management::encode_operation_record(queued_record());
    REQUIRE(encoded.ok());
    encoded.value()[25] ^= std::byte{0x01};
    auto decoded = myrd::management::decode_operation_record(encoded.value());
    REQUIRE(!decoded.ok());
}

TEST_CASE(operation_record_decoder_survives_deterministic_malformed_matrix) {
    auto encoded = myrd::management::encode_operation_record(queued_record());
    REQUIRE(encoded.ok());
    const auto verify_canonical_or_rejected = [](const auto& candidate) {
        auto decoded = myrd::management::decode_operation_record(candidate);
        if (!decoded.ok()) return;
        auto canonical = myrd::management::encode_operation_record(
            decoded.value());
        REQUIRE(canonical.ok());
        REQUIRE_EQ(canonical.value(), candidate);
    };

    for (std::size_t size = 0; size < encoded.value().size(); ++size) {
        verify_canonical_or_rejected(std::vector<std::byte>(
            encoded.value().begin(), encoded.value().begin() + size));
    }
    for (std::size_t count = 1; count <= 16; ++count) {
        auto trailing = encoded.value();
        trailing.insert(trailing.end(), count, std::byte{0xa5});
        verify_canonical_or_rejected(trailing);
    }

    std::minstd_rand random(0x5eedU);
    for (std::size_t iteration = 0; iteration < 20'000; ++iteration) {
        auto mutated = encoded.value();
        const auto mutations = 1U + random() % 4U;
        for (std::uint32_t index = 0; index < mutations; ++index) {
            const auto offset = static_cast<std::size_t>(random())
                % mutated.size();
            const auto delta = static_cast<unsigned char>(
                1U + random() % 255U);
            mutated[offset] ^= static_cast<std::byte>(delta);
        }
        verify_canonical_or_rejected(mutated);
    }
    for (std::size_t iteration = 0; iteration < 2'000; ++iteration) {
        const auto size = static_cast<std::size_t>(random()) % 1025U;
        std::vector<std::byte> arbitrary(size);
        for (auto& value : arbitrary) {
            value = static_cast<std::byte>(random() & 0xffU);
        }
        verify_canonical_or_rejected(arbitrary);
    }

    std::vector<std::byte> oversized(
        myrd::management::kOperationMaximumBytes + 1U, std::byte{0});
    REQUIRE(!myrd::management::decode_operation_record(oversized).ok());
}

TEST_CASE(operation_store_atomically_persists_revision_cas_and_previous) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());

    const auto queued = queued_record();
    REQUIRE(store.value().write_record(0, queued).ok());
    auto loaded = store.value().load_record(queued.operation_id);
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value(), queued);

    auto running = queued;
    running.revision = 2;
    running.state = myrd::management::OperationState::running;
    running.phase = "dispatch";
    running.updated_unix_ms += 1;
    REQUIRE(store.value().write_record(1, running).ok());
    REQUIRE_EQ(store.value().load_record(queued.operation_id).value(), running);
    const auto previous_path = std::filesystem::path(
        store.value().paths().operations_root)
        / (uuid_wstring(queued.operation_id) + L".prev");
    auto previous = myrd::management::decode_operation_record(
        read_bytes(previous_path));
    REQUIRE(previous.ok());
    REQUIRE_EQ(previous.value(), queued);

    auto stale = running;
    stale.revision = 3;
    stale.phase = "stale";
    stale.updated_unix_ms += 1;
    auto rejected = store.value().write_record(1, stale);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::revision_conflict));
    REQUIRE_EQ(store.value().load_record(queued.operation_id).value(), running);
}

TEST_CASE(operation_store_replays_exact_request_and_terminal_result) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());
    const myrd::management::OperationRequest request{
        uuid("40000000-0000-4000-8000-000000000004"),
        uuid("30000000-0000-4000-8000-000000000003"),
        L"S-1-5-21-100-200-300-1001",
        "disk.checkpoint",
        argument_digest(),
        1700000000000ULL};
    auto admitted = store.value().admit(request);
    REQUIRE(admitted.ok());
    REQUIRE(!admitted.value().replayed);
    auto replay = store.value().admit(request);
    REQUIRE(replay.ok());
    REQUIRE(replay.value().replayed);
    REQUIRE_EQ(replay.value().record, admitted.value().record);

    auto terminal = admitted.value().record;
    terminal.revision = 2;
    terminal.state = myrd::management::OperationState::succeeded;
    terminal.phase = "terminal";
    terminal.updated_unix_ms += 10;
    terminal.terminal_unix_ms = terminal.updated_unix_ms;
    terminal.cancellable = false;
    terminal.terminal_result = {
        std::byte{'e'}, std::byte{'x'}, std::byte{'a'}, std::byte{'c'},
        std::byte{'t'}};
    REQUIRE(store.value().write_record(1, terminal).ok());
    replay = store.value().admit(request);
    REQUIRE(replay.ok());
    REQUIRE(replay.value().replayed);
    REQUIRE_EQ(replay.value().record.terminal_result,
               terminal.terminal_result);

    auto reopened = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(reopened.ok());
    auto restart_replay = reopened.value().admit(request);
    REQUIRE(restart_replay.ok());
    REQUIRE(restart_replay.value().replayed);
    REQUIRE_EQ(restart_replay.value().record, terminal);
}

TEST_CASE(operation_store_conflicts_changed_arguments_but_scopes_callers) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());
    myrd::management::OperationRequest request{
        uuid("40000000-0000-4000-8000-000000000004"),
        uuid("30000000-0000-4000-8000-000000000003"),
        L"S-1-5-21-100-200-300-1001",
        "disk.checkpoint",
        argument_digest(),
        1700000000000ULL};
    auto first = store.value().admit(request);
    REQUIRE(first.ok());
    auto changed = request;
    changed.argument_digest.front() ^= std::byte{0xff};
    auto conflict = store.value().admit(changed);
    REQUIRE(!conflict.ok());
    REQUIRE_EQ(conflict.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::request_id_conflict));

    auto another_caller = request;
    another_caller.caller_sid = L"S-1-5-18";
    auto caller_admission = store.value().admit(another_caller);
    REQUIRE(caller_admission.ok());
    REQUIRE(caller_admission.value().record.operation_id
            != first.value().record.operation_id);
    auto another_command = request;
    another_command.command_scope = "disk.compact";
    auto command_admission = store.value().admit(another_command);
    REQUIRE(command_admission.ok());
    REQUIRE(command_admission.value().record.operation_id
            != first.value().record.operation_id);
}

TEST_CASE(operation_store_replays_across_independent_store_instances) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto first_store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    auto second_store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(first_store.ok());
    REQUIRE(second_store.ok());
    const myrd::management::OperationRequest request{
        uuid("60000000-0000-4000-8000-000000000006"),
        uuid("30000000-0000-4000-8000-000000000003"),
        L"S-1-5-21-100-200-300-1001",
        "disk.checkpoint", argument_digest(), 1700000000000ULL};
    auto admitted = first_store.value().admit(request);
    REQUIRE(admitted.ok());
    auto replay = second_store.value().admit(request);
    REQUIRE(replay.ok());
    REQUIRE(replay.value().replayed);
    REQUIRE_EQ(replay.value().record.operation_id,
               admitted.value().record.operation_id);
}

TEST_CASE(operation_store_rejects_case_alias_current_filenames) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto record = queued_record();
    auto encoded = myrd::management::encode_operation_record(record);
    REQUIRE(encoded.ok());
    const auto operation_root = sandbox.program_data()
        / L"MyRamDisk" / L"operations";
    write_bytes(
        operation_root / (uuid_wstring(record.operation_id) + L".MYOP"),
        encoded.value());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(!store.ok());
    REQUIRE_EQ(store.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::storage_corrupt));
}

TEST_CASE(operation_store_lists_by_creation_time_then_operation_id) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());
    for (const auto [suffix, timestamp] : {
             std::pair{4, 1700000003000ULL},
             std::pair{5, 1700000001000ULL},
             std::pair{6, 1700000002000ULL}}) {
        const auto request_id = std::string(
            "40000000-0000-4000-8000-00000000000")
            + static_cast<char>('0' + suffix);
        auto request = myrd::management::OperationRequest{
            uuid(request_id),
            uuid("30000000-0000-4000-8000-000000000003"),
            L"S-1-5-21-100-200-300-1001",
            "disk.checkpoint",
            argument_digest(),
            timestamp};
        REQUIRE(store.value().admit(request).ok());
    }
    auto records = store.value().list_records();
    REQUIRE(records.ok());
    REQUIRE_EQ(records.value().size(), 3U);
    REQUIRE(records.value()[0].created_unix_ms
            < records.value()[1].created_unix_ms);
    REQUIRE(records.value()[1].created_unix_ms
            < records.value()[2].created_unix_ms);
}

TEST_CASE(operation_store_persists_idempotent_cancellation_and_rejects_late_cancel) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());
    const myrd::management::OperationRequest first_request{
        uuid("50000000-0000-4000-8000-000000000005"),
        uuid("30000000-0000-4000-8000-000000000003"),
        L"S-1-5-21-100-200-300-1001",
        "disk.checkpoint", argument_digest(), 1700000000000ULL};
    auto first = store.value().admit(first_request);
    REQUIRE(first.ok());
    auto cancelled = store.value().request_cancel(
        first.value().record.operation_id, 1700000000010ULL);
    REQUIRE(cancelled.ok());
    REQUIRE(cancelled.value().cancel_requested);
    REQUIRE_EQ(cancelled.value().revision, 2ULL);
    auto repeated = store.value().request_cancel(
        first.value().record.operation_id, 1700000000020ULL);
    REQUIRE(repeated.ok());
    REQUIRE_EQ(repeated.value(), cancelled.value());

    auto second_request = first_request;
    second_request.request_id =
        uuid("50000000-0000-4000-8000-000000000006");
    auto second = store.value().admit(second_request);
    REQUIRE(second.ok());
    auto running = second.value().record;
    ++running.revision;
    running.state = myrd::management::OperationState::running;
    running.phase = "dispatch";
    running.updated_unix_ms += 1;
    REQUIRE(store.value().write_record(1, running).ok());
    auto committed = running;
    ++committed.revision;
    committed.phase = "commit-boundary";
    committed.updated_unix_ms += 1;
    committed.cancellable = false;
    REQUIRE(store.value().write_record(2, committed).ok());
    auto too_late = store.value().request_cancel(
        committed.operation_id, committed.updated_unix_ms + 1);
    REQUIRE(!too_late.ok());
    REQUIRE_EQ(too_late.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::cancellation_too_late));
    REQUIRE_EQ(store.value().load_record(committed.operation_id).value(),
               committed);

    auto reopened = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(reopened.ok());
    REQUIRE(reopened.value().load_record(
        cancelled.value().operation_id).value().cancel_requested);
}

TEST_CASE(operation_retention_selects_seven_day_expiry_and_hard_cap) {
    constexpr std::uint64_t now = 1800000000000ULL;
    auto old_terminal = terminal_record(
        queued_record(), now - myrd::management::kOperationRetentionMilliseconds);
    old_terminal.operation_id = numbered_uuid(1);
    auto active = queued_record();
    active.operation_id = numbered_uuid(2);
    active.created_unix_ms = now - 2 * myrd::management::kOperationRetentionMilliseconds;
    active.updated_unix_ms = active.created_unix_ms;
    std::array<myrd::management::OperationRecord, 2> basic{
        old_terminal, active};
    auto selected = myrd::management::select_operation_retention_candidates(
        basic, now);
    REQUIRE(selected.ok());
    REQUIRE_EQ(selected.value().size(), 1U);
    REQUIRE_EQ(selected.value()[0], old_terminal.operation_id);

    std::vector<myrd::management::OperationRecord> capped;
    capped.reserve(myrd::management::kOperationMaximumTerminalRecords + 1U);
    for (std::size_t index = 0;
         index <= myrd::management::kOperationMaximumTerminalRecords; ++index) {
        auto record = terminal_record(
            queued_record(), now - 1000ULL + index);
        record.operation_id = numbered_uuid(static_cast<std::uint32_t>(index + 10));
        capped.push_back(std::move(record));
    }
    selected = myrd::management::select_operation_retention_candidates(
        capped, now);
    REQUIRE(selected.ok());
    REQUIRE_EQ(selected.value().size(), 1U);
    REQUIRE_EQ(selected.value()[0], capped.front().operation_id);
}

TEST_CASE(operation_cleanup_removes_only_expired_metadata_and_rejects_hardlinks) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());
    constexpr std::uint64_t now = 1800000000000ULL;
    auto expired = queued_record();
    expired.operation_id = numbered_uuid(100);
    expired.request_id = numbered_uuid(101);
    expired.created_unix_ms = now
        - myrd::management::kOperationRetentionMilliseconds - 1000ULL;
    expired.updated_unix_ms = expired.created_unix_ms;
    REQUIRE(store.value().write_record(0, expired).ok());
    auto expired_terminal = terminal_record(expired, expired.created_unix_ms + 1);
    REQUIRE(store.value().write_record(1, expired_terminal).ok());
    auto active = queued_record();
    active.operation_id = numbered_uuid(102);
    active.request_id = numbered_uuid(103);
    active.created_unix_ms = expired.created_unix_ms;
    active.updated_unix_ms = active.created_unix_ms;
    REQUIRE(store.value().write_record(0, active).ok());

    auto cleaned = store.value().cleanup_terminal_records(now);
    REQUIRE(cleaned.ok());
    REQUIRE_EQ(cleaned.value().removed_operation_ids.size(), 1U);
    REQUIRE_EQ(cleaned.value().removed_operation_ids[0],
               expired.operation_id);
    REQUIRE(!store.value().load_record(expired.operation_id).ok());
    REQUIRE(store.value().load_record(active.operation_id).ok());

    auto unsafe = queued_record();
    unsafe.operation_id = numbered_uuid(104);
    unsafe.request_id = numbered_uuid(105);
    unsafe.created_unix_ms = expired.created_unix_ms;
    unsafe.updated_unix_ms = unsafe.created_unix_ms;
    REQUIRE(store.value().write_record(0, unsafe).ok());
    auto unsafe_terminal = terminal_record(unsafe, unsafe.created_unix_ms + 1);
    REQUIRE(store.value().write_record(1, unsafe_terminal).ok());
    const auto operations = std::filesystem::path(
        store.value().paths().operations_root);
    const auto previous = operations
        / (uuid_wstring(unsafe.operation_id) + L".prev");
    REQUIRE(std::filesystem::remove(previous));
    const auto sentinel = sandbox.program_data() / L"sentinel.bin";
    {
        std::ofstream stream(sentinel, std::ios::binary);
        stream << "sentinel";
    }
    REQUIRE(CreateHardLinkW(previous.c_str(), sentinel.c_str(), nullptr));
    cleaned = store.value().cleanup_terminal_records(now);
    REQUIRE(!cleaned.ok());
    REQUIRE(std::filesystem::exists(
        operations / (uuid_wstring(unsafe.operation_id) + L".myop")));
    REQUIRE_EQ(read_bytes(sentinel),
               (std::vector<std::byte>{
                   std::byte{'s'}, std::byte{'e'}, std::byte{'n'},
                   std::byte{'t'}, std::byte{'i'}, std::byte{'n'},
                   std::byte{'e'}, std::byte{'l'}}));
}

TEST_CASE(operation_cleanup_removes_safe_orphan_previous_after_interruption) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());
    auto queued = queued_record();
    queued.operation_id = numbered_uuid(200);
    queued.request_id = numbered_uuid(201);
    REQUIRE(store.value().write_record(0, queued).ok());
    auto terminal = terminal_record(queued, queued.updated_unix_ms + 1);
    REQUIRE(store.value().write_record(1, terminal).ok());
    const auto operations = std::filesystem::path(
        store.value().paths().operations_root);
    const auto current = operations
        / (uuid_wstring(queued.operation_id) + L".myop");
    const auto previous = operations
        / (uuid_wstring(queued.operation_id) + L".prev");
    REQUIRE(std::filesystem::exists(previous));
    REQUIRE(std::filesystem::remove(current));

    auto cleaned = store.value().cleanup_terminal_records(
        terminal.terminal_unix_ms.value()
        + myrd::management::kOperationRetentionMilliseconds);
    REQUIRE(cleaned.ok());
    REQUIRE(!std::filesystem::exists(previous));
}

TEST_CASE(operation_store_open_removes_only_canonical_safe_orphan_temps) {
    TestDirectory sandbox;
    create_operation_layout(sandbox.program_data());
    const auto operations = sandbox.program_data()
        / L"MyRamDisk" / L"operations";
    const auto orphan = operations
        / L".next.10000000-0000-4000-8000-000000000001.tmp";
    write_bytes(orphan, {std::byte{'o'}, std::byte{'r'}, std::byte{'p'}});

    auto store = myrd::management::OperationStore::open_existing(
        sandbox.program_data().wstring());
    REQUIRE(store.ok());
    REQUIRE(!std::filesystem::exists(orphan));
}

TEST_CASE(operation_store_open_rejects_unsafe_or_noncanonical_orphan_temps) {
    {
        TestDirectory sandbox;
        create_operation_layout(sandbox.program_data());
        const auto operations = sandbox.program_data()
            / L"MyRamDisk" / L"operations";
        const auto sentinel = sandbox.program_data() / L"sentinel.bin";
        write_bytes(sentinel, {std::byte{'s'}, std::byte{'a'}, std::byte{'f'},
                               std::byte{'e'}});
        const auto orphan = operations
            / L".previous.10000000-0000-4000-8000-000000000001.tmp";
        REQUIRE(CreateHardLinkW(orphan.c_str(), sentinel.c_str(), nullptr));

        auto store = myrd::management::OperationStore::open_existing(
            sandbox.program_data().wstring());
        REQUIRE(!store.ok());
        REQUIRE(std::filesystem::exists(orphan));
        REQUIRE_EQ(read_bytes(sentinel),
                   (std::vector<std::byte>{std::byte{'s'}, std::byte{'a'},
                                           std::byte{'f'}, std::byte{'e'}}));
    }
    {
        TestDirectory sandbox;
        create_operation_layout(sandbox.program_data());
        const auto operations = sandbox.program_data()
            / L"MyRamDisk" / L"operations";
        const auto noncanonical = operations
            / L".NEXT.10000000-0000-4000-8000-000000000001.tmp";
        write_bytes(noncanonical, {std::byte{'x'}});

        auto store = myrd::management::OperationStore::open_existing(
            sandbox.program_data().wstring());
        REQUIRE(!store.ok());
        REQUIRE_EQ(store.error().code, static_cast<std::uint32_t>(
            myrd::management::OperationStoreError::storage_corrupt));
        REQUIRE(std::filesystem::exists(noncanonical));
    }
}
