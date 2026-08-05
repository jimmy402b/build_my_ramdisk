#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include "test_framework.h"

#include "src/persistence/persistence_types.h"
#include "src/common/sha256_stream.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/native_rename_backend.h"
#include "src/persistence/writer_lock.h"
#include "src/persistence/vhdx_format.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/vhdx_writer.h"
#include "tests/support/fault_file.h"
#include "src/common/unique_handle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <stop_token>
#include <vector>

namespace myrd::persistence {
namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

class WriterSandbox final {
public:
    WriterSandbox() {
        static std::uint64_t sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-task23-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(sequence++));
        std::filesystem::create_directories(root_);
        std::ofstream manifest(
            root_ / L"disk.rdm", std::ios::binary | std::ios::trunc);
        const std::vector<char> bytes(8192, 'm');
        manifest.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        manifest.close();
    }

    ~WriterSandbox() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

VhdxWriterIdentity writer_identity() {
    VhdxWriterIdentity identity{};
    identity.file_identity.file_write_guid[0] = std::byte{0x11};
    identity.file_identity.data_write_guid[0] = std::byte{0x22};
    identity.virtual_disk_id[0] = std::byte{0x33};
    return identity;
}

class MapPayloadSource final : public IVhdxPayloadSource {
public:
    std::map<std::uint64_t, std::vector<std::byte>> payloads;
    mutable std::vector<std::uint64_t> calls;

    Result<void> read_payload(
        const OperationContext&,
        const std::uint64_t payload_index,
        const std::span<std::byte> destination) const override {
        calls.push_back(payload_index);
        const auto found = payloads.find(payload_index);
        if (found == payloads.end() ||
            found->second.size() != destination.size()) {
            return Result<void>::failure({
                ErrorDomain::persistence, 0x3fff,
                L"MapPayloadSource::read_payload",
                L"test payload is missing"});
        }
        std::copy(
            found->second.begin(), found->second.end(),
            destination.begin());
        return Result<void>::success();
    }
};

class ImmediateRenameFailure final
    : public detail::INativeRenameBackend {
public:
    LONG begin(
        HANDLE,
        detail::NativeRenameIoStatus*,
        void*,
        ULONG) override {
        ++begin_calls;
        return static_cast<LONG>(0xC0000022UL);
    }
    DWORD wait(std::span<const HANDLE>, DWORD) override {
        return WAIT_FAILED;
    }
    void cancel(HANDLE) noexcept override {}
    ULONG status_to_dos(LONG) noexcept override {
        return ERROR_ACCESS_DENIED;
    }
    bool flush(HANDLE, DWORD& native_error) noexcept override {
        native_error = ERROR_SUCCESS;
        return true;
    }
    std::size_t begin_calls{};
};

class RenameThenFlushFailure final
    : public detail::INativeRenameBackend {
public:
    RenameThenFlushFailure() noexcept {
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        set_information_ = reinterpret_cast<SetInformationFunction>(
            GetProcAddress(ntdll, "NtSetInformationFile"));
        status_to_dos_ = reinterpret_cast<StatusToDosFunction>(
            GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    }

    LONG begin(
        HANDLE file,
        detail::NativeRenameIoStatus* io_status,
        void* information,
        const ULONG information_size) override {
        if (set_information_ == nullptr) {
            return static_cast<LONG>(0xC000007AUL);
        }
        constexpr ULONG kFileRenameInformation = 10;
        return set_information_(
            file, io_status, information, information_size,
            kFileRenameInformation);
    }

    DWORD wait(
        const std::span<const HANDLE> handles,
        const DWORD timeout_ms) override {
        if (handles.empty() || handles.size() > MAXIMUM_WAIT_OBJECTS) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return WAIT_FAILED;
        }
        return WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()), handles.data(), FALSE,
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

    bool flush(HANDLE, DWORD& native_error) noexcept override {
        ++flush_calls;
        native_error = ERROR_WRITE_FAULT;
        return false;
    }

    std::size_t flush_calls{};

private:
    using SetInformationFunction = LONG(NTAPI*)(
        HANDLE, detail::NativeRenameIoStatus*, void*, ULONG, ULONG);
    using StatusToDosFunction = ULONG(WINAPI*)(LONG);
    SetInformationFunction set_information_{};
    StatusToDosFunction status_to_dos_{};
};

std::vector<std::byte> deterministic_payload(std::uint64_t state) {
    std::vector<std::byte> payload(kVhdxDataBlockBytes);
    for (auto& value : payload) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        value = static_cast<std::byte>(state & 0xffU);
    }
    return payload;
}

std::uint64_t decode_u64(const std::span<const std::byte> bytes) {
    REQUIRE(bytes.size() >= sizeof(std::uint64_t));
    std::uint64_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
    }
    return value;
}

std::unique_ptr<VhdxReader> open_published_reader(
    const WriterSandbox& sandbox,
    const VhdxPublishedBase& published,
    const std::uint64_t budget) {
    auto capability = open_bundle_file_for_inspection(
        sandbox.root(), published.relative_name(),
        published.windows_identity());
    REQUIRE(capability.ok());
    auto file = open_validated_persistence_file(capability.move_value());
    REQUIRE(file.ok());
    auto reader = VhdxReader::create(
        OperationContext{}, file.move_value(), published.layout(),
        {{16 * kVhdxBatPageBytes, 16}, budget, budget});
    REQUIRE(reader.ok());
    return reader.move_value();
}

VhdxWriterLimits writer_limits() {
    return {4096, 4096, kVhdxDataBlockBytes};
}

TEST_CASE(vhdx_writer_errors_name_the_failing_public_boundary) {
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto invalid_plan = plan_sparse_vhdx(
        OperationContext{}, {0, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(!invalid_plan.ok());
    REQUIRE_EQ(invalid_plan.error().operation,
               std::wstring(L"plan_sparse_vhdx"));

    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    auto invalid_write = write_sparse_vhdx_file(
        OperationContext{}, plan.value(), source, nullptr);
    REQUIRE(!invalid_write.ok());
    REQUIRE_EQ(invalid_write.error().operation,
               std::wstring(L"write_sparse_vhdx_file"));

    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto invalid_publish = publish_sparse_vhdx_base(
        OperationContext{}, writer, "same.vhdx", "same.vhdx",
        plan.value(), source);
    REQUIRE(!invalid_publish.ok());
    REQUIRE_EQ(invalid_publish.error().operation,
               std::wstring(L"publish_sparse_vhdx_base"));
#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto invalid_test_publish = publish_sparse_vhdx_base_for_testing(
        OperationContext{}, writer, "temp.vhdx", "final.vhdx",
        plan.value(), source, VhdxPublicationTestOptions{});
    REQUIRE(!invalid_test_publish.ok());
    REQUIRE_EQ(invalid_test_publish.error().operation,
               std::wstring(L"publish_sparse_vhdx_base_for_testing"));
#endif
}

TEST_CASE(vhdx_writer_rejects_case_aliasing_temp_and_final_names) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());

    auto result = publish_sparse_vhdx_base(
        OperationContext{}, writer, "Alias.vhdx", "alias.VHDX",
        plan.value(), source);

    REQUIRE(!result.ok());
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"Alias.vhdx"));
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"alias.VHDX"));
}

TEST_CASE(vhdx_writer_rejects_ntfs_short_name_aliases_before_create) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());

    auto result = publish_sparse_vhdx_base(
        OperationContext{}, writer,
        "LongTemporaryFilename.vhdx", "LONGTE~1.VHD",
        plan.value(), source);

    REQUIRE(!result.ok());
    REQUIRE(!std::filesystem::exists(
        sandbox.root() / L"LongTemporaryFilename.vhdx"));
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"LONGTE~1.VHD"));
}

TEST_CASE(vhdx_writer_plans_empty_minimum_geometry_without_source_io) {
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto result = plan_sparse_vhdx(
        OperationContext{},
        {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().virtual_size(), kLogicalSectorSize512);
    REQUIRE_EQ(result.value().payload_block_count(), 1ULL);
    REQUIRE_EQ(result.value().bat_offset(), 1 * kMiB);
    REQUIRE_EQ(result.value().bat_length(), 1 * kMiB);
    REQUIRE_EQ(result.value().metadata_offset(), 2 * kMiB);
    REQUIRE_EQ(result.value().log_offset(), 3 * kMiB);
    REQUIRE_EQ(result.value().payload_offset(), 4 * kMiB);
    REQUIRE_EQ(result.value().file_length(), 4 * kMiB);
    REQUIRE(result.value().payloads().empty());
    REQUIRE(source.calls.empty());
}

TEST_CASE(vhdx_writer_plan_skips_zero_and_packs_noncontiguous_payloads) {
    MapPayloadSource source;
    for (const auto payload : {0ULL, 2ULL, 5ULL}) {
        source.payloads.emplace(
            payload,
            std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0}));
    }
    source.payloads.at(0)[0] = std::byte{0x41};
    source.payloads.at(5)[kVhdxDataBlockBytes - 1] = std::byte{0x52};
    const std::vector<std::uint64_t> candidates{0, 2, 5};
    auto result = plan_sparse_vhdx(
        OperationContext{},
        {12 * kMiB + kLogicalSectorSize4096, kLogicalSectorSize4096},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(result.ok());
    REQUIRE_EQ(source.calls, candidates);
    REQUIRE_EQ(result.value().payloads().size(), 2ULL);
    REQUIRE_EQ(result.value().payloads()[0].payload_index, 0ULL);
    REQUIRE_EQ(result.value().payloads()[0].file_offset, 4 * kMiB);
    REQUIRE_EQ(result.value().payloads()[1].payload_index, 5ULL);
    REQUIRE_EQ(result.value().payloads()[1].file_offset, 6 * kMiB);
    REQUIRE_EQ(result.value().file_length(), 8 * kMiB);
}

TEST_CASE(vhdx_writer_serializes_sparse_plan_and_product_reader_reopens_it) {
    MapPayloadSource source;
    for (const auto payload : {0ULL, 2ULL, 5ULL}) {
        source.payloads.emplace(
            payload,
            std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0}));
    }
    source.payloads.at(0)[0] = std::byte{0x41};
    source.payloads.at(5)[kVhdxDataBlockBytes - 1] = std::byte{0x52};
    const std::vector<std::uint64_t> candidates{0, 2, 5};
    auto plan = plan_sparse_vhdx(
        OperationContext{},
        {12 * kMiB + kLogicalSectorSize4096, kLogicalSectorSize4096},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());

    auto fault = test::make_fault_file();
    auto written = write_sparse_vhdx_file(
        OperationContext{}, plan.value(), source, std::move(fault.file));
    REQUIRE(written.ok());
    const std::vector<std::uint64_t> expected_calls{0, 2, 5, 0, 2, 5};
    REQUIRE_EQ(source.calls, expected_calls);
    REQUIRE_EQ(written.value().layout().virtual_size(),
               12 * kMiB + kLogicalSectorSize4096);
    REQUIRE_EQ(written.value().layout().file_length(), 8 * kMiB);

    WindowsFileIdentity windows_identity{};
    windows_identity.volume_serial_number = 7;
    windows_identity.file_id[0] = std::byte{0x71};
    windows_identity.size = 8 * kMiB;
    windows_identity.attributes = 1;
    windows_identity.hard_link_count = 1;
    auto serialized_file = written.value().release_file();
    const auto require_zero = [&serialized_file](
                                  const std::uint64_t offset,
                                  const std::size_t length) {
        std::vector<std::byte> bytes(length, std::byte{0xff});
        REQUIRE(serialized_file->read_exact(offset, bytes).ok());
        REQUIRE(std::all_of(
            bytes.begin(), bytes.end(),
            [](const std::byte value) { return value == std::byte{0}; }));
    };
    require_zero(8, 64 * 1024 - 8);
    require_zero(64 * 1024 + 80, 4096 - 80);
    require_zero(192 * 1024 + 80, 64 * 1024 - 80);
    require_zero(320 * 1024, 704 * 1024);
    require_zero(plan.value().bat_offset() + 2 * 8, 8);
    require_zero(plan.value().metadata_offset() + 192, 64 * 1024 - 192);
    require_zero(
        plan.value().metadata_offset() + 64 * 1024 + 40,
        static_cast<std::size_t>(kMiB - 64 * 1024 - 40));
    require_zero(plan.value().log_offset(), static_cast<std::size_t>(kMiB));
    auto reader_result = VhdxReader::create_for_testing(
        std::move(serialized_file), windows_identity,
        written.value().layout(), {{4 * kVhdxBatPageBytes, 4}, 16, 16});
    REQUIRE(reader_result.ok());
    auto reader = reader_result.move_value();
    std::array<std::byte, kM2BlockSize> destination{};
    REQUIRE(reader->read_block(
        OperationContext{}, 0, destination).ok());
    REQUIRE_EQ(destination[0], std::byte{0x41});
    REQUIRE(reader->read_block(
        OperationContext{}, 2 * 32, destination).ok());
    REQUIRE_EQ(reader->stats().payload_read_calls, 1ULL);
    REQUIRE(reader->read_block(
        OperationContext{}, 5 * 32 + 31, destination).ok());
    REQUIRE_EQ(destination.back(), std::byte{0x52});
}

TEST_CASE(vhdx_writer_serializes_the_512_sector_bat_chunk_boundary) {
    MapPayloadSource source;
    source.payloads.emplace(2047, deterministic_payload(0x2047));
    source.payloads.emplace(2048, deterministic_payload(0x2048));
    const std::vector<std::uint64_t> candidates{2047, 2048};
    auto plan = plan_sparse_vhdx(
        OperationContext{},
        {2049ULL * kVhdxDataBlockBytes, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    REQUIRE_EQ(plan.value().chunk_ratio(), 2048ULL);
    REQUIRE_EQ(plan.value().payloads().size(), std::size_t{2});

    auto fault = test::make_fault_file();
    auto written = write_sparse_vhdx_file(
        OperationContext{}, plan.value(), source, std::move(fault.file));
    REQUIRE(written.ok());
    auto serialized = written.value().release_file();
    std::array<std::byte, 3 * sizeof(std::uint64_t)> entries{};
    REQUIRE(serialized->read_exact(
        plan.value().bat_offset() + 2047 * sizeof(std::uint64_t),
        entries).ok());
    const auto first = decode_u64(std::span(entries).subspan(0, 8));
    const auto bitmap = decode_u64(std::span(entries).subspan(8, 8));
    const auto second = decode_u64(std::span(entries).subspan(16, 8));
    REQUIRE_EQ(first,
        ((plan.value().payloads()[0].file_offset / kMiB) << 20U) | 6U);
    REQUIRE_EQ(bitmap, 0ULL);
    REQUIRE_EQ(second,
        ((plan.value().payloads()[1].file_offset / kMiB) << 20U) | 6U);

    WindowsFileIdentity identity{};
    identity.volume_serial_number = 9;
    identity.file_id[0] = std::byte{0x73};
    identity.size = written.value().layout().file_length();
    identity.attributes = 1;
    identity.hard_link_count = 1;
    auto reader = VhdxReader::create_for_testing(
        std::move(serialized), identity, written.value().layout(),
        {{4 * kVhdxBatPageBytes, 4}, 8, 8});
    REQUIRE(reader.ok());
    std::array<std::byte, kM2BlockSize> destination{};
    auto before_boundary = reader.value()->read_block(
        OperationContext{}, 2047 * 32, destination);
    REQUIRE(before_boundary.ok());
    REQUIRE_EQ(before_boundary.value().kind, BlockLoadKind::Data);
    REQUIRE_EQ(destination[0], source.payloads.at(2047)[0]);
    auto after_boundary = reader.value()->read_block(
        OperationContext{}, 2048 * 32, destination);
    REQUIRE(after_boundary.ok());
    REQUIRE_EQ(after_boundary.value().kind, BlockLoadKind::Data);
    REQUIRE_EQ(destination[0], source.payloads.at(2048)[0]);
}

TEST_CASE(vhdx_writer_plans_maximum_geometry_with_checked_large_bat) {
    constexpr std::uint64_t maximum_virtual = 64ULL * 1024 * 1024 * 1024 * 1024;
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {maximum_virtual, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    REQUIRE_EQ(plan.value().payload_block_count(), 33554432ULL);
    REQUIRE_EQ(plan.value().bat_entry_count(), 33570815ULL);
    REQUIRE_EQ(plan.value().bat_length(), 257 * kMiB);
    REQUIRE_EQ(plan.value().metadata_offset(), 258 * kMiB);
    REQUIRE_EQ(plan.value().log_offset(), 259 * kMiB);
    REQUIRE_EQ(plan.value().payload_offset(), 260 * kMiB);
    REQUIRE_EQ(plan.value().file_length(), 260 * kMiB);
}

TEST_CASE(vhdx_writer_rejects_nonzero_bytes_beyond_final_virtual_tail) {
    MapPayloadSource source;
    source.payloads.emplace(
        1, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0}));
    source.payloads.at(1)[kLogicalSectorSize4096] = std::byte{0x7e};
    const std::vector<std::uint64_t> candidates{1};
    auto plan = plan_sparse_vhdx(
        OperationContext{},
        {2 * kMiB + kLogicalSectorSize4096, kLogicalSectorSize4096},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(!plan.ok());
    REQUIRE_EQ(plan.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::InvalidFormat));
}

TEST_CASE(vhdx_writer_round_trips_a_valid_final_partial_payload) {
    MapPayloadSource source;
    source.payloads.emplace(
        1, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0}));
    source.payloads.at(1)[0] = std::byte{0x63};
    source.payloads.at(1)[kLogicalSectorSize4096 - 1] = std::byte{0x71};
    const std::vector<std::uint64_t> candidates{1};
    auto plan = plan_sparse_vhdx(
        OperationContext{},
        {2 * kMiB + kLogicalSectorSize4096, kLogicalSectorSize4096},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());

    auto fault = test::make_fault_file();
    auto written = write_sparse_vhdx_file(
        OperationContext{}, plan.value(), source, std::move(fault.file));
    REQUIRE(written.ok());
    WindowsFileIdentity windows_identity{};
    windows_identity.volume_serial_number = 8;
    windows_identity.file_id[0] = std::byte{0x72};
    windows_identity.size = written.value().layout().file_length();
    windows_identity.attributes = 1;
    windows_identity.hard_link_count = 1;
    auto reader_result = VhdxReader::create_for_testing(
        written.value().release_file(), windows_identity,
        written.value().layout(), {{4 * kVhdxBatPageBytes, 4}, 8, 8});
    REQUIRE(reader_result.ok());
    std::array<std::byte, kM2BlockSize> destination{};
    auto loaded = reader_result.value()->read_block(
        OperationContext{}, 32, destination);
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
    REQUIRE_EQ(destination[0], std::byte{0x63});
    REQUIRE_EQ(destination[kLogicalSectorSize4096 - 1], std::byte{0x71});
    REQUIRE(std::all_of(
        destination.begin() + kLogicalSectorSize4096,
        destination.end(),
        [](const std::byte value) { return value == std::byte{0}; }));
}

TEST_CASE(vhdx_writer_detects_source_drift_before_flush) {
    MapPayloadSource source;
    source.payloads.emplace(
        0, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0}));
    const std::vector<std::uint64_t> candidates{0};
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {2 * kMiB, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    source.payloads.at(0)[0] = std::byte{0x19};
    auto fault = test::make_fault_file();
    auto written = write_sparse_vhdx_file(
        OperationContext{}, plan.value(), source, std::move(fault.file));
    REQUIRE(!written.ok());
    REQUIRE_EQ(written.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::BackingDataCorrupt));
    REQUIRE_EQ(fault.state->calls(test::FaultFilePhase::Flush), 0ULL);
}

TEST_CASE(vhdx_writer_propagates_mutation_flush_and_readback_faults) {
    MapPayloadSource source;
    source.payloads.emplace(
        0, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0x27}));
    const std::vector<std::uint64_t> candidates{0};
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {2 * kMiB, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());

    const std::vector<test::FaultFileRule> rules{
        {test::FaultFilePhase::Truncate, 1, std::nullopt,
         test::FaultFileAction::NativeError, 1117},
        {test::FaultFilePhase::Write, 1, 0,
         test::FaultFileAction::NativeError, 1117},
        {test::FaultFilePhase::Write, 1, 64 * 1024,
         test::FaultFileAction::NativeError, 1117},
        {test::FaultFilePhase::Write, 1, plan.value().metadata_offset(),
         test::FaultFileAction::NativeError, 1117},
        {test::FaultFilePhase::Write, 1, plan.value().bat_offset(),
         test::FaultFileAction::NativeError, 1117},
        {test::FaultFilePhase::Write, 1, plan.value().payload_offset(),
         test::FaultFileAction::NativeError, 1117},
        {test::FaultFilePhase::Flush, 1, std::nullopt,
         test::FaultFileAction::FlushError, 1117},
        {test::FaultFilePhase::Read, 1, std::nullopt,
         test::FaultFileAction::NativeError, 1117}};
    for (const auto& rule : rules) {
        auto fault = test::make_fault_file(0, {rule});
        auto result = write_sparse_vhdx_file(
            OperationContext{}, plan.value(), source,
            std::move(fault.file));
        REQUIRE(!result.ok());
    }
}

TEST_CASE(vhdx_writer_actual_io_order_flushes_before_product_readback) {
    MapPayloadSource source;
    source.payloads.emplace(
        0, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0x37}));
    const std::vector<std::uint64_t> candidates{0};
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {2 * kMiB, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    auto fault = test::make_fault_file();
    auto state = fault.state;
    auto written = write_sparse_vhdx_file(
        OperationContext{}, plan.value(), source, std::move(fault.file));
    REQUIRE(written.ok());
    const auto requests = state->requests();
    const auto find_request = [&requests](
        const test::FaultFilePhase phase,
        const std::optional<std::uint64_t> offset = std::nullopt) {
        for (std::size_t index = 0; index < requests.size(); ++index) {
            if (requests[index].phase == phase &&
                (!offset.has_value() || requests[index].offset == *offset)) {
                return index;
            }
        }
        return requests.size();
    };
    const auto truncate = find_request(test::FaultFilePhase::Truncate);
    const auto structure = find_request(test::FaultFilePhase::Write, 0);
    const auto metadata = find_request(
        test::FaultFilePhase::Write, plan.value().metadata_offset());
    const auto bat = find_request(
        test::FaultFilePhase::Write, plan.value().bat_offset());
    const auto payload = find_request(
        test::FaultFilePhase::Write, plan.value().payload_offset());
    const auto flush = find_request(test::FaultFilePhase::Flush);
    const auto readback = find_request(test::FaultFilePhase::Read);
    REQUIRE(readback != requests.size());
    REQUIRE(truncate < structure);
    REQUIRE(structure < metadata);
    REQUIRE(metadata < bat);
    REQUIRE(bat < payload);
    REQUIRE(payload < flush);
    REQUIRE(flush < readback);
}

TEST_CASE(vhdx_writer_publishes_windows_compatible_dynamic_file_after_reopen) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    source.payloads.emplace(
        0, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0x31}));
    const std::vector<std::uint64_t> candidates{0};
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {256 * kMiB, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());

    auto published = publish_sparse_vhdx_base(
        OperationContext{}, writer, "base.tmp.vhdx", "base.vhdx",
        plan.value(), source);
    REQUIRE(published.ok());
    REQUIRE_EQ(published.value().relative_name(), std::string("base.vhdx"));
    REQUIRE_EQ(published.value().layout().file_length(), 6 * kMiB);
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"base.tmp.vhdx"));
    REQUIRE(std::filesystem::exists(sandbox.root() / L"base.vhdx"));
    const auto attributes = GetFileAttributesW(
        (sandbox.root() / L"base.vhdx").c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    // Windows Virtdisk rejects VHD/VHDX containers carrying the NTFS sparse
    // attribute (ERROR_VIRTUAL_DISK_LIMITATION). Dynamic sparseness is encoded
    // by absent BAT payloads and a short container, not FSCTL_SET_SPARSE.
    REQUIRE((attributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0);
    REQUIRE_EQ(
        std::filesystem::file_size(sandbox.root() / L"base.vhdx"),
        published.value().layout().file_length());
    REQUIRE(published.value().layout().file_length() < plan.value().virtual_size());
}

TEST_CASE(vhdx_writer_published_empty_image_reads_as_product_zero) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{},
        {3 * kVhdxDataBlockBytes + kLogicalSectorSize512,
         kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    auto published = publish_sparse_vhdx_base(
        OperationContext{}, writer, "empty.tmp.vhdx", "empty.vhdx",
        plan.value(), source);
    REQUIRE(published.ok());
    auto reader = open_published_reader(sandbox, published.value(), 8);
    std::array<std::byte, kM2BlockSize> destination{};
    destination.fill(std::byte{0x7f});
    auto first = reader->read_block(OperationContext{}, 0, destination);
    REQUIRE(first.ok());
    REQUIRE_EQ(first.value().kind, BlockLoadKind::AllZero);
    REQUIRE(std::all_of(destination.begin(), destination.end(),
        [](const std::byte value) { return value == std::byte{0x7f}; }));
    destination.fill(std::byte{0x7f});
    const auto last_block =
        (plan.value().virtual_size() - 1) / kM2BlockSize;
    auto last = reader->read_block(
        OperationContext{}, last_block, destination);
    REQUIRE(last.ok());
    REQUIRE_EQ(last.value().kind, BlockLoadKind::AllZero);
    REQUIRE(std::all_of(destination.begin(), destination.end(),
        [](const std::byte value) { return value == std::byte{0x7f}; }));
}

TEST_CASE(vhdx_writer_random_sparse_product_matches_independent_oracle) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    constexpr std::uint64_t kSeed = 0x6a09e667f3bcc909ULL;
    constexpr std::uint64_t kPayloadCount = 24;
    auto state = kSeed;
    const auto next_random = [&state]() {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    std::vector<std::uint64_t> candidates;
    std::size_t zero_candidates{};
    for (std::uint64_t payload = 0; payload < kPayloadCount; ++payload) {
        const auto mapping_bits = next_random();
        if ((mapping_bits & 3U) == 0) continue;
        candidates.push_back(payload);
        const auto all_zero = (mapping_bits & 0x38U) == 0;
        if (all_zero) ++zero_candidates;
        source.payloads.emplace(
            payload,
            all_zero
                ? std::vector<std::byte>(
                      kVhdxDataBlockBytes, std::byte{0})
                : deterministic_payload(next_random() ^ payload));
    }
    REQUIRE(!candidates.empty());
    REQUIRE(zero_candidates != 0);
    REQUIRE(zero_candidates != candidates.size());
    auto plan = plan_sparse_vhdx(
        OperationContext{},
        {kPayloadCount * kVhdxDataBlockBytes, kLogicalSectorSize4096},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    auto published = publish_sparse_vhdx_base(
        OperationContext{}, writer, "random.tmp.vhdx", "random.vhdx",
        plan.value(), source);
    REQUIRE(published.ok());
    auto reader = open_published_reader(sandbox, published.value(), 32);
    std::array<std::byte, kM2BlockSize> destination{};
    std::array<std::byte, kM2BlockSize> logical_zero{};
    Sha256Stream expected_hash;
    Sha256Stream actual_hash;
    constexpr std::uint64_t kBlocksPerPayload =
        kVhdxDataBlockBytes / kM2BlockSize;
    for (std::uint64_t block = 0;
         block < plan.value().virtual_size() / kM2BlockSize; ++block) {
        destination.fill(std::byte{0xa5});
        auto loaded = reader->read_block(
            OperationContext{}, block, destination);
        REQUIRE(loaded.ok());
        const auto payload_index = block / kBlocksPerPayload;
        const auto slice = block % kBlocksPerPayload;
        const auto found = source.payloads.find(payload_index);
        const auto allocated = found != source.payloads.end() &&
            std::any_of(found->second.begin(), found->second.end(),
                [](const std::byte value) { return value != std::byte{0}; });
        REQUIRE_EQ(loaded.value().kind,
            allocated ? BlockLoadKind::Data : BlockLoadKind::AllZero);
        if (!allocated) {
            REQUIRE(std::all_of(destination.begin(), destination.end(),
                [](const std::byte value) {
                    return value == std::byte{0xa5};
                }));
            REQUIRE(expected_hash.update(logical_zero).ok());
            REQUIRE(actual_hash.update(logical_zero).ok());
            continue;
        }
        const auto begin = found->second.begin() +
            static_cast<std::ptrdiff_t>(slice * kM2BlockSize);
        REQUIRE(std::equal(
            destination.begin(), destination.end(), begin));
        REQUIRE(expected_hash.update(std::span(
            &*begin, static_cast<std::size_t>(kM2BlockSize))).ok());
        REQUIRE(actual_hash.update(destination).ok());
    }
    auto expected_digest = expected_hash.finalize_digest();
    auto actual_digest = actual_hash.finalize_digest();
    REQUIRE(expected_digest.ok());
    REQUIRE(actual_digest.ok());
    REQUIRE_EQ(actual_digest.value(), expected_digest.value());
}

TEST_CASE(vhdx_writer_real_create_collision_never_exposes_final_name) {
    WriterSandbox sandbox;
    const auto temporary_path = sandbox.root() / L"occupied.tmp.vhdx";
    {
        std::ofstream occupied(
            temporary_path, std::ios::binary | std::ios::trunc);
        occupied << "temp-sentinel";
    }
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());

    auto result = publish_sparse_vhdx_base(
        OperationContext{}, writer, "occupied.tmp.vhdx", "create.vhdx",
        plan.value(), source);

    REQUIRE(!result.ok());
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"create.vhdx"));
    std::ifstream occupied(temporary_path, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(occupied),
        std::istreambuf_iterator<char>()};
    REQUIRE_EQ(contents, std::string("temp-sentinel"));
}

TEST_CASE(vhdx_writer_never_overwrites_an_existing_final_base) {
    WriterSandbox sandbox;
    const auto final_path = sandbox.root() / L"base.vhdx";
    {
        std::ofstream existing(
            final_path, std::ios::binary | std::ios::trunc);
        existing << "sentinel";
    }
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    auto published = publish_sparse_vhdx_base(
        OperationContext{}, writer, "collision.tmp.vhdx", "base.vhdx",
        plan.value(), source);
    REQUIRE(!published.ok());
    REQUIRE(std::filesystem::exists(
        sandbox.root() / L"collision.tmp.vhdx"));
    std::ifstream existing(final_path, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(existing),
        std::istreambuf_iterator<char>()};
    REQUIRE_EQ(contents, std::string("sentinel"));
}

TEST_CASE(vhdx_writer_pre_cancel_does_not_create_a_temporary_file) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    std::stop_source stopped;
    stopped.request_stop();

    auto result = publish_sparse_vhdx_base(
        OperationContext{stopped.get_token()}, writer,
        "cancelled.tmp.vhdx", "cancelled.vhdx", plan.value(), source);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
        OperationErrorCode::cancelled));
    REQUIRE(!std::filesystem::exists(
        sandbox.root() / L"cancelled.tmp.vhdx"));
    REQUIRE(!std::filesystem::exists(
        sandbox.root() / L"cancelled.vhdx"));
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
TEST_CASE(vhdx_writer_closes_every_old_handle_before_product_reopen) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    const auto temporary_path = sandbox.root() / L"closed.tmp.vhdx";

    auto published = publish_sparse_vhdx_base_for_testing(
        OperationContext{}, writer, "closed.tmp.vhdx", "closed.vhdx",
        plan.value(), source,
        [&temporary_path](const VhdxPublicationPhase phase) {
            if (phase != VhdxPublicationPhase::BeforeReopen) {
                return Result<void>::success();
            }
            const auto exclusive = CreateFileW(
                temporary_path.c_str(), GENERIC_READ, 0, nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (exclusive == INVALID_HANDLE_VALUE) {
                const auto native_code = GetLastError();
                return Result<void>::failure({
                    ErrorDomain::win32, native_code,
                    L"vhdx_writer_test.exclusive_reopen",
                    L"an old VHDX handle survived close", native_code});
            }
            REQUIRE(CloseHandle(exclusive));
            return Result<void>::success();
        });
    REQUIRE(published.ok());
    REQUIRE(std::filesystem::exists(sandbox.root() / L"closed.vhdx"));
}

TEST_CASE(vhdx_writer_real_publication_stage_failures_never_create_final) {
    const auto run = [](VhdxPublicationTestOptions options,
                        const std::string& stem) {
        WriterSandbox sandbox;
        auto writer_result = acquire_bundle_writer_lock(
            sandbox.root(), "disk.rdm");
        REQUIRE(writer_result.ok());
        auto writer = writer_result.move_value();
        MapPayloadSource source;
        source.payloads.emplace(
            0, std::vector<std::byte>(
                   kVhdxDataBlockBytes, std::byte{0x2d}));
        const std::vector<std::uint64_t> candidates{0};
        auto plan = plan_sparse_vhdx(
            OperationContext{}, {2 * kMiB, kLogicalSectorSize512},
            writer_identity(), candidates, source, writer_limits());
        REQUIRE(plan.ok());
        const auto temporary = stem + ".tmp.vhdx";
        const auto final = stem + ".vhdx";

        auto result = publish_sparse_vhdx_base_for_testing(
            OperationContext{}, writer, temporary, final,
            plan.value(), source, std::move(options));

        REQUIRE(!result.ok());
        REQUIRE(!std::filesystem::exists(
            sandbox.root() / std::filesystem::path(final)));
    };

    VhdxPublicationTestOptions close_options;
    close_options.close_operation = [](ValidatedPersistenceFile& file) {
        auto closed = file.close();
        if (!closed.ok()) return closed;
        return Result<void>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"vhdx_writer_test.close_backend",
            L"injected close failure"});
    };
    run(std::move(close_options), "close-failure");

    UniqueHandle reopen_blocker;
    VhdxPublicationTestOptions reopen_options;
    reopen_options.phase_hook = [&reopen_blocker](
        const VhdxPublicationPhase phase) {
        if (phase != VhdxPublicationPhase::BeforeReopen) {
            return Result<void>::success();
        }
        return Result<void>::success();
    };
    reopen_options.before_reopen_operation =
        [&reopen_blocker](const std::filesystem::path& temporary_path) {
            reopen_blocker.reset(CreateFileW(
                temporary_path.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!reopen_blocker) {
                return Result<void>::failure({
                    ErrorDomain::win32, GetLastError(),
                    L"vhdx_writer_test.block_reopen",
                    L"opening the reopen blocker failed", GetLastError()});
            }
            return Result<void>::success();
        };
    run(std::move(reopen_options), "reopen-failure");
    reopen_blocker.reset();

    ImmediateRenameFailure rename_must_not_run;
    VhdxPublicationTestOptions corrupt_reopen_options;
    corrupt_reopen_options.rename_backend = &rename_must_not_run;
    corrupt_reopen_options.before_reopen_operation = [](
        const std::filesystem::path& temporary_path) {
        UniqueHandle file(CreateFileW(
            temporary_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!file) {
            return Result<void>::failure({
                ErrorDomain::win32, GetLastError(),
                L"vhdx_writer_test.corrupt_before_reopen",
                L"opening the temporary VHDX for corruption failed",
                GetLastError()});
        }
        constexpr std::array<char, 8> corrupt{
            'b', 'r', 'o', 'k', 'e', 'n', '!', '!'};
        DWORD written{};
        if (!WriteFile(
                file.get(), corrupt.data(), static_cast<DWORD>(corrupt.size()),
                &written, nullptr) ||
            written != static_cast<DWORD>(corrupt.size())) {
            return Result<void>::failure({
                ErrorDomain::win32, GetLastError(),
                L"vhdx_writer_test.corrupt_before_reopen",
                L"corrupting the temporary VHDX failed", GetLastError()});
        }
        return Result<void>::success();
    };
    run(std::move(corrupt_reopen_options), "reopen-corrupt");
    REQUIRE_EQ(rename_must_not_run.begin_calls, std::size_t{0});

    ImmediateRenameFailure rename_failure;
    VhdxPublicationTestOptions rename_options;
    rename_options.rename_backend = &rename_failure;
    run(std::move(rename_options), "rename-failure");
}

TEST_CASE(vhdx_writer_rename_flush_failure_reports_indeterminate_publish) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {kLogicalSectorSize512, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    RenameThenFlushFailure rename_failure;
    VhdxPublicationTestOptions options;
    options.rename_backend = &rename_failure;

    auto result = publish_sparse_vhdx_base_for_testing(
        OperationContext{}, writer, "flush.tmp.vhdx", "flush.vhdx",
        plan.value(), source, std::move(options));

    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::CommitIndeterminate));
    REQUIRE_EQ(rename_failure.flush_calls, std::size_t{1});
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"flush.tmp.vhdx"));
    REQUIRE(std::filesystem::exists(sandbox.root() / L"flush.vhdx"));
}

TEST_CASE(vhdx_writer_file_phase_failures_never_escape_the_temp_namespace) {
    const std::array phases{
        VhdxFileWritePhase::BeforeExtend,
        VhdxFileWritePhase::BeforeStructure,
        VhdxFileWritePhase::BeforeMetadata,
        VhdxFileWritePhase::BeforeBat,
        VhdxFileWritePhase::BeforePayload,
        VhdxFileWritePhase::BeforeFlush,
        VhdxFileWritePhase::BeforeReadback,
    };
    for (std::size_t index = 0; index < phases.size(); ++index) {
        WriterSandbox sandbox;
        auto writer_result = acquire_bundle_writer_lock(
            sandbox.root(), "disk.rdm");
        REQUIRE(writer_result.ok());
        auto writer = writer_result.move_value();
        MapPayloadSource source;
        source.payloads.emplace(
            0, std::vector<std::byte>(
                   kVhdxDataBlockBytes, std::byte{0x5c}));
        const std::vector<std::uint64_t> candidates{0};
        auto plan = plan_sparse_vhdx(
            OperationContext{}, {2 * kMiB, kLogicalSectorSize512},
            writer_identity(), candidates, source, writer_limits());
        REQUIRE(plan.ok());
        VhdxPublicationTestOptions options;
        const auto injected = phases[index];
        options.file_write_hook = [injected](VhdxFileWritePhase phase) {
            if (phase != injected) return Result<void>::success();
            return Result<void>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"vhdx_writer_test.file_phase",
                L"injected file phase failure"});
        };
        const auto stem = "file-phase-" + std::to_string(index);
        auto result = publish_sparse_vhdx_base_for_testing(
            OperationContext{}, writer, stem + ".tmp.vhdx",
            stem + ".vhdx", plan.value(), source, std::move(options));
        REQUIRE(!result.ok());
        REQUIRE(!std::filesystem::exists(
            sandbox.root() / std::filesystem::path(stem + ".vhdx")));
    }
}

TEST_CASE(vhdx_writer_publish_order_is_frozen_before_namespace_commit) {
    WriterSandbox sandbox;
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    MapPayloadSource source;
    source.payloads.emplace(
        0, std::vector<std::byte>(kVhdxDataBlockBytes, std::byte{0x6d}));
    const std::vector<std::uint64_t> candidates{0};
    auto plan = plan_sparse_vhdx(
        OperationContext{}, {2 * kMiB, kLogicalSectorSize512},
        writer_identity(), candidates, source, writer_limits());
    REQUIRE(plan.ok());
    std::vector<std::string> order;
    VhdxPublicationTestOptions options;
    options.phase_hook = [&order](VhdxPublicationPhase phase) {
        order.push_back("p" + std::to_string(static_cast<int>(phase)));
        return Result<void>::success();
    };
    options.file_write_hook = [&order](VhdxFileWritePhase phase) {
        order.push_back("f" + std::to_string(static_cast<int>(phase)));
        return Result<void>::success();
    };

    auto result = publish_sparse_vhdx_base_for_testing(
        OperationContext{}, writer, "ordered.tmp.vhdx", "ordered.vhdx",
        plan.value(), source, std::move(options));

    REQUIRE(result.ok());
    const std::vector<std::string> expected{
        "p0", "f0", "f1", "f2", "f3", "f4", "f5", "f6",
        "p1", "p2", "p3"};
    REQUIRE_EQ(order, expected);
}

TEST_CASE(vhdx_writer_publication_faults_never_expose_a_final_name) {
    const std::array phases{
        VhdxPublicationPhase::BeforeCreate,
        VhdxPublicationPhase::BeforeClose,
        VhdxPublicationPhase::BeforeReopen,
        VhdxPublicationPhase::BeforeRename,
    };

    for (std::size_t index = 0; index < phases.size(); ++index) {
        WriterSandbox sandbox;
        auto writer_result = acquire_bundle_writer_lock(
            sandbox.root(), "disk.rdm");
        REQUIRE(writer_result.ok());
        auto writer = writer_result.move_value();
        MapPayloadSource source;
        source.payloads.emplace(
            0, std::vector<std::byte>(
                   kVhdxDataBlockBytes, std::byte{0x4a}));
        const std::vector<std::uint64_t> candidates{0};
        auto plan = plan_sparse_vhdx(
            OperationContext{}, {2 * kMiB, kLogicalSectorSize512},
            writer_identity(), candidates, source, writer_limits());
        REQUIRE(plan.ok());

        const auto temporary_name =
            "fault-" + std::to_string(index) + ".tmp.vhdx";
        const auto final_name =
            "fault-" + std::to_string(index) + ".vhdx";
        const auto injected_phase = phases[index];
        auto result = publish_sparse_vhdx_base_for_testing(
            OperationContext{}, writer, temporary_name, final_name,
            plan.value(), source,
            [injected_phase](const VhdxPublicationPhase phase) {
                if (phase != injected_phase) {
                    return Result<void>::success();
                }
                return Result<void>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::IoError),
                    L"vhdx_writer_test.publication_phase",
                    L"injected publication failure"});
            });
        REQUIRE(!result.ok());
        REQUIRE(!std::filesystem::exists(
            sandbox.root() / std::filesystem::path(final_name)));
        REQUIRE_EQ(result.error().operation,
                   std::wstring(L"vhdx_writer_test.publication_phase"));
    }
}
#endif

}  // namespace
}  // namespace myrd::persistence
