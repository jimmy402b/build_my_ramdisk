#include "src/persistence/vhdx_writer.h"

#include "src/common/crc32c.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/writer_lock.h"
#include "src/storage/block_constants.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

namespace myrd::persistence {
namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint64_t kMaximumVirtualBytes =
    64ull * 1024ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kBatEntryBytes = 8;
constexpr std::size_t kIdentifierBytes = 64 * 1024;
constexpr std::size_t kHeaderBytes = 4096;
constexpr std::size_t kRegionBytes = 64 * 1024;
constexpr std::size_t kMetadataTableBytes = 64 * 1024;
constexpr std::uint64_t kHeader1Offset = 64 * 1024;
constexpr std::uint64_t kHeader2Offset = 128 * 1024;
constexpr std::uint64_t kRegion1Offset = 192 * 1024;
constexpr std::uint64_t kRegion2Offset = 256 * 1024;

#if defined(MYRD_ENABLE_TEST_SEAMS)
thread_local const VhdxPublicationTestOptions* g_publication_options{};
#endif

using Guid = std::array<std::byte, 16>;

constexpr Guid guid(std::array<std::uint8_t, 16> source) {
    Guid result{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        result[index] = static_cast<std::byte>(source[index]);
    }
    return result;
}

constexpr auto kBatGuid = guid({
    0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0x00, 0x42,
    0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 0x08});
constexpr auto kMetadataGuid = guid({
    0x06, 0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
    0xb8, 0xfe, 0x57, 0x5f, 0x05, 0x0f, 0x88, 0x6e});
constexpr auto kFileParametersGuid = guid({
    0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
    0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b});
constexpr auto kVirtualSizeGuid = guid({
    0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
    0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8});
constexpr auto kVirtualIdGuid = guid({
    0xab, 0x12, 0xca, 0xbe, 0xe6, 0xb2, 0x23, 0x45,
    0x93, 0xef, 0xc3, 0x09, 0xe0, 0x00, 0xc7, 0x46});
constexpr auto kLogicalSectorGuid = guid({
    0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 0x09, 0x47,
    0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f});
constexpr auto kPhysicalSectorGuid = guid({
    0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44,
    0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56});

template <typename T>
Result<T> writer_failure(
    std::wstring_view message,
    PersistenceErrorCode code = PersistenceErrorCode::InvalidFormat,
    std::wstring_view operation = L"plan_sparse_vhdx") {
    return Result<T>::failure({
        ErrorDomain::persistence, static_cast<std::uint32_t>(code),
        std::wstring{operation}, std::wstring{message}});
}

Result<void> publication_phase(
    const OperationContext& context,
    const VhdxPublicationPhase phase) {
    auto live = check_operation_context(context);
    if (!live.ok()) return live;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (g_publication_options != nullptr &&
        g_publication_options->phase_hook) {
        try {
            auto injected = g_publication_options->phase_hook(phase);
            if (!injected.ok()) return injected;
        } catch (...) {
            return writer_failure<void>(
                L"VHDX publication test hook threw",
                PersistenceErrorCode::FatalInternalInvariant,
                L"publish_sparse_vhdx_base");
        }
    }
#else
    (void)phase;
#endif
    return check_operation_context(context);
}

Result<void> file_write_phase(
    const OperationContext& context,
    const VhdxFileWritePhase phase) {
    auto live = check_operation_context(context);
    if (!live.ok()) return live;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (g_publication_options != nullptr &&
        g_publication_options->file_write_hook) {
        try {
            auto injected = g_publication_options->file_write_hook(phase);
            if (!injected.ok()) return injected;
        } catch (...) {
            return writer_failure<void>(
                L"VHDX file-write test hook threw",
                PersistenceErrorCode::FatalInternalInvariant,
                L"write_sparse_vhdx_file");
        }
    }
#else
    (void)phase;
#endif
    return check_operation_context(context);
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool checked_mul(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (left != 0 &&
        right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool round_up_mib(
    const std::uint64_t value,
    std::uint64_t& result) noexcept {
    std::uint64_t adjusted = 0;
    if (!checked_add(value, kMiB - 1, adjusted)) return false;
    result = adjusted / kMiB * kMiB;
    return true;
}

void put_ascii(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::string_view value) {
    std::transform(
        value.begin(), value.end(), bytes.begin() +
            static_cast<std::ptrdiff_t>(offset),
        [](const char item) { return static_cast<std::byte>(item); });
}

void put_u16(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void put_u64(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void put_guid(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const Guid& value) {
    std::copy(value.begin(), value.end(), bytes.begin() +
        static_cast<std::ptrdiff_t>(offset));
}

void seal_checksum(const std::span<std::byte> bytes) {
    put_u32(bytes, 4, 0);
    put_u32(bytes, 4, crc32c(bytes));
}

template <typename T>
Result<T> translate_write_error(
    const Error& error,
    const std::wstring_view operation) {
    if (error.domain == ErrorDomain::storage &&
        (error.code == static_cast<std::uint32_t>(
             OperationErrorCode::cancelled) ||
         error.code == static_cast<std::uint32_t>(
             OperationErrorCode::timed_out))) {
        return Result<T>::failure(error);
    }
    const auto code = error.native_code == ERROR_ACCESS_DENIED
        ? PersistenceErrorCode::AccessDenied
        : PersistenceErrorCode::IoError;
    return Result<T>::failure({
        ErrorDomain::persistence, static_cast<std::uint32_t>(code),
        std::wstring{operation}, L"writing VHDX temporary file failed",
        error.native_code});
}

template <typename Container>
Result<void> write_bytes(
    IPersistenceFile& file,
    const OperationContext& context,
    const std::uint64_t offset,
    const Container& bytes,
    const std::wstring_view operation) {
    auto result = file.write_exact(context, offset, std::span{bytes});
    if (!result.ok()) {
        return translate_write_error<void>(result.error(), operation);
    }
    auto live = check_operation_context(context);
    if (!live.ok()) return live;
    return Result<void>::success();
}

std::array<std::byte, kHeaderBytes> make_header(
    const VhdxWritePlan& plan,
    const std::uint64_t sequence) {
    std::array<std::byte, kHeaderBytes> bytes{};
    put_ascii(bytes, 0, "head");
    put_u64(bytes, 8, sequence);
    put_guid(bytes, 16, plan.identity().file_identity.file_write_guid);
    put_guid(bytes, 32, plan.identity().file_identity.data_write_guid);
    put_u16(bytes, 64, 0);
    put_u16(bytes, 66, 1);
    put_u32(bytes, 68, static_cast<std::uint32_t>(kMiB));
    put_u64(bytes, 72, plan.log_offset());
    seal_checksum(bytes);
    return bytes;
}

void put_region_entry(
    const std::span<std::byte> bytes,
    const std::size_t index,
    const Guid& id,
    const std::uint64_t offset,
    const std::uint32_t length) {
    const auto base = 16 + index * 32;
    put_guid(bytes, base, id);
    put_u64(bytes, base + 16, offset);
    put_u32(bytes, base + 24, length);
    put_u32(bytes, base + 28, 1);
}

std::array<std::byte, kRegionBytes> make_region_table(
    const VhdxWritePlan& plan) {
    std::array<std::byte, kRegionBytes> bytes{};
    put_ascii(bytes, 0, "regi");
    put_u32(bytes, 8, 2);
    put_region_entry(
        bytes, 0, kBatGuid, plan.bat_offset(),
        static_cast<std::uint32_t>(plan.bat_length()));
    put_region_entry(
        bytes, 1, kMetadataGuid, plan.metadata_offset(),
        static_cast<std::uint32_t>(kMiB));
    seal_checksum(bytes);
    return bytes;
}

void put_metadata_entry(
    const std::span<std::byte> bytes,
    const std::size_t index,
    const Guid& id,
    const std::uint32_t offset,
    const std::uint32_t length,
    const std::uint32_t flags) {
    const auto base = 32 + index * 32;
    put_guid(bytes, base, id);
    put_u32(bytes, base + 16, offset);
    put_u32(bytes, base + 20, length);
    put_u32(bytes, base + 24, flags);
}

std::array<std::byte, kMetadataTableBytes> make_metadata_table() {
    std::array<std::byte, kMetadataTableBytes> bytes{};
    put_ascii(bytes, 0, "metadata");
    put_u16(bytes, 10, 5);
    put_metadata_entry(bytes, 0, kFileParametersGuid, 0x10000, 8, 4);
    put_metadata_entry(bytes, 1, kVirtualSizeGuid, 0x10008, 8, 6);
    put_metadata_entry(bytes, 2, kVirtualIdGuid, 0x10010, 16, 6);
    put_metadata_entry(bytes, 3, kLogicalSectorGuid, 0x10020, 4, 6);
    put_metadata_entry(bytes, 4, kPhysicalSectorGuid, 0x10024, 4, 6);
    return bytes;
}

std::array<std::byte, 40> make_metadata_items(
    const VhdxWritePlan& plan) {
    std::array<std::byte, 40> bytes{};
    put_u32(bytes, 0, kVhdxDataBlockBytes);
    put_u32(bytes, 4, 0);
    put_u64(bytes, 8, plan.virtual_size());
    put_guid(bytes, 16, plan.identity().virtual_disk_id);
    put_u32(bytes, 32, plan.logical_sector_size());
    put_u32(bytes, 36, kPhysicalSectorSize);
    return bytes;
}

bool same_plan_layout(
    const VhdxWritePlan& plan,
    const VhdxLayout& layout) noexcept {
    return layout.virtual_size() == plan.virtual_size() &&
        layout.logical_sector_size() == plan.logical_sector_size() &&
        layout.physical_sector_size() == kPhysicalSectorSize &&
        layout.block_size() == kVhdxDataBlockBytes &&
        layout.payload_block_count() == plan.payload_block_count() &&
        layout.chunk_ratio() == plan.chunk_ratio() &&
        layout.bat_entry_count() == plan.bat_entry_count() &&
        layout.bat_offset() == plan.bat_offset() &&
        layout.bat_length() == plan.bat_length() &&
        layout.metadata_offset() == plan.metadata_offset() &&
        layout.metadata_length() == kMiB &&
        layout.log_offset() == plan.log_offset() &&
        layout.log_length() == kMiB &&
        layout.file_length() == plan.file_length() &&
        layout.file_identity() == plan.identity().file_identity &&
        layout.virtual_disk_id() == plan.identity().virtual_disk_id;
}

template <std::size_t Size>
bool nil(const std::array<std::byte, Size>& value) noexcept {
    return std::ranges::all_of(value, [](const std::byte item) {
        return item == std::byte{0};
    });
}

}  // namespace

std::uint64_t VhdxWritePlan::virtual_size() const noexcept {
    return geometry_.virtual_size;
}

std::uint32_t VhdxWritePlan::logical_sector_size() const noexcept {
    return geometry_.logical_sector_size;
}

std::uint64_t VhdxWritePlan::payload_block_count() const noexcept {
    return payload_block_count_;
}

std::uint64_t VhdxWritePlan::chunk_ratio() const noexcept {
    return chunk_ratio_;
}

std::uint64_t VhdxWritePlan::bat_entry_count() const noexcept {
    return bat_entry_count_;
}

std::uint64_t VhdxWritePlan::bat_offset() const noexcept {
    return bat_offset_;
}

std::uint64_t VhdxWritePlan::bat_length() const noexcept {
    return bat_length_;
}

std::uint64_t VhdxWritePlan::metadata_offset() const noexcept {
    return metadata_offset_;
}

std::uint64_t VhdxWritePlan::log_offset() const noexcept {
    return log_offset_;
}

std::uint64_t VhdxWritePlan::payload_offset() const noexcept {
    return payload_offset_;
}

std::uint64_t VhdxWritePlan::file_length() const noexcept {
    return file_length_;
}

const VhdxWriterIdentity& VhdxWritePlan::identity() const noexcept {
    return identity_;
}

const std::vector<VhdxPlannedPayload>&
VhdxWritePlan::payloads() const noexcept {
    return payloads_;
}

Result<VhdxWritePlan> plan_sparse_vhdx(
    const OperationContext& context,
    const VhdxWriterGeometry geometry,
    VhdxWriterIdentity identity,
    const std::span<const std::uint64_t> candidate_payloads,
    const IVhdxPayloadSource& source,
    const VhdxWriterLimits limits) {
    try {
        auto live = check_operation_context(context);
        if (!live.ok()) return Result<VhdxWritePlan>::failure(live.error());
        if (geometry.virtual_size == 0 ||
            geometry.virtual_size > kMaximumVirtualBytes ||
            (geometry.logical_sector_size != kLogicalSectorSize512 &&
             geometry.logical_sector_size != kLogicalSectorSize4096) ||
            geometry.virtual_size % geometry.logical_sector_size != 0 ||
            nil(identity.file_identity.file_write_guid) ||
            nil(identity.file_identity.data_write_guid) ||
            nil(identity.virtual_disk_id) ||
            limits.max_candidate_payloads == 0 ||
            limits.max_allocated_payloads == 0 ||
            limits.scratch_bytes < kVhdxDataBlockBytes ||
            candidate_payloads.size() > limits.max_candidate_payloads) {
            return writer_failure<VhdxWritePlan>(
                L"VHDX writer geometry, identity or limits are invalid");
        }

        VhdxWritePlan plan{};
        plan.geometry_ = geometry;
        plan.identity_ = std::move(identity);
        plan.payload_block_count_ =
            (geometry.virtual_size - 1) / kVhdxDataBlockBytes + 1;
        const auto chunk_bytes =
            (std::uint64_t{1} << 23U) * geometry.logical_sector_size;
        plan.chunk_ratio_ = chunk_bytes / kVhdxDataBlockBytes;
        const auto bitmap_entries =
            (plan.payload_block_count_ - 1) / plan.chunk_ratio_;
        if (!checked_add(
                plan.payload_block_count_, bitmap_entries,
                plan.bat_entry_count_)) {
            return writer_failure<VhdxWritePlan>(
                L"VHDX BAT entry count overflows");
        }
        std::uint64_t bat_bytes = 0;
        if (!checked_mul(plan.bat_entry_count_, kBatEntryBytes, bat_bytes) ||
            !round_up_mib(bat_bytes, plan.bat_length_)) {
            return writer_failure<VhdxWritePlan>(
                L"VHDX BAT length overflows");
        }
        plan.bat_offset_ = kMiB;
        if (!checked_add(
                plan.bat_offset_, plan.bat_length_,
                plan.metadata_offset_) ||
            !checked_add(plan.metadata_offset_, kMiB, plan.log_offset_) ||
            !checked_add(plan.log_offset_, kMiB, plan.payload_offset_)) {
            return writer_failure<VhdxWritePlan>(
                L"VHDX structural layout overflows");
        }

        plan.payloads_.reserve(candidate_payloads.size());
        plan.candidates_.reserve(candidate_payloads.size());
        std::vector<std::byte> scratch(kVhdxDataBlockBytes);
        std::uint64_t previous = 0;
        bool have_previous = false;
        for (const auto candidate : candidate_payloads) {
            if (candidate >= plan.payload_block_count_ ||
                (have_previous && candidate <= previous)) {
                return writer_failure<VhdxWritePlan>(
                    L"VHDX payload candidates must be sorted, unique and in range");
            }
            previous = candidate;
            have_previous = true;
            std::fill(scratch.begin(), scratch.end(), std::byte{0});
            auto read = source.read_payload(context, candidate, scratch);
            if (!read.ok()) {
                return Result<VhdxWritePlan>::failure(read.error());
            }
            live = check_operation_context(context);
            if (!live.ok()) {
                return Result<VhdxWritePlan>::failure(live.error());
            }
            const auto final_payload_bytes =
                geometry.virtual_size % kVhdxDataBlockBytes;
            if (candidate + 1 == plan.payload_block_count_ &&
                final_payload_bytes != 0 &&
                !std::ranges::all_of(
                    scratch.begin() + static_cast<std::ptrdiff_t>(
                        final_payload_bytes),
                    scratch.end(),
                    [](const std::byte item) {
                        return item == std::byte{0};
                    })) {
                return writer_failure<VhdxWritePlan>(
                    L"final VHDX payload has nonzero bytes beyond virtual size");
            }
            auto digest = sha256_digest(scratch);
            if (!digest.ok()) {
                return Result<VhdxWritePlan>::failure(digest.error());
            }
            const auto value = digest.move_value();
            const auto all_zero = std::ranges::all_of(
                scratch, [](const std::byte item) {
                    return item == std::byte{0};
                });
            if (all_zero) {
                plan.candidates_.push_back({candidate, 0, value, false});
                continue;
            }
            if (plan.payloads_.size() >= limits.max_allocated_payloads) {
                return writer_failure<VhdxWritePlan>(
                    L"allocated VHDX payload count exceeds caller budget",
                    PersistenceErrorCode::IoError);
            }
            std::uint64_t payload_delta = 0;
            std::uint64_t file_offset = 0;
            if (!checked_mul(
                    static_cast<std::uint64_t>(plan.payloads_.size()),
                    kVhdxDataBlockBytes, payload_delta) ||
                !checked_add(
                    plan.payload_offset_, payload_delta, file_offset)) {
                return writer_failure<VhdxWritePlan>(
                    L"VHDX payload layout overflows");
            }
            plan.payloads_.push_back({candidate, file_offset, value});
            plan.candidates_.push_back(
                {candidate, file_offset, value, true});
            continue;
        }
        std::uint64_t payload_bytes = 0;
        if (!checked_mul(
                static_cast<std::uint64_t>(plan.payloads_.size()),
                kVhdxDataBlockBytes, payload_bytes) ||
            !checked_add(
                plan.payload_offset_, payload_bytes, plan.file_length_)) {
            return writer_failure<VhdxWritePlan>(
                L"VHDX final length overflows");
        }
        return Result<VhdxWritePlan>::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return writer_failure<VhdxWritePlan>(
            L"allocating bounded VHDX writer plan failed",
            PersistenceErrorCode::IoError);
    }
}

VhdxWrittenFile::VhdxWrittenFile(
    std::unique_ptr<IPersistenceFile> file,
    VhdxLayout layout) noexcept
    : file_(std::move(file)), layout_(std::move(layout)) {}

const VhdxLayout& VhdxWrittenFile::layout() const noexcept {
    return layout_;
}

std::unique_ptr<IPersistenceFile>
VhdxWrittenFile::release_file() noexcept {
    return std::move(file_);
}

Result<VhdxWrittenFile> write_sparse_vhdx_file(
    const OperationContext& context,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source,
    std::unique_ptr<IPersistenceFile> temporary_file) {
    constexpr auto operation = L"write_sparse_vhdx_file";
    if (!temporary_file) {
        return writer_failure<VhdxWrittenFile>(
            L"VHDX temporary file is null",
            PersistenceErrorCode::InvalidFormat, operation);
    }
    try {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<VhdxWrittenFile>::failure(live.error());
        }
        auto initial_length = temporary_file->length(context);
        if (!initial_length.ok()) {
            return translate_write_error<VhdxWrittenFile>(
                initial_length.error(), operation);
        }
        if (initial_length.value() != 0) {
            return writer_failure<VhdxWrittenFile>(
                L"VHDX temporary file must be newly created and empty",
                PersistenceErrorCode::InvalidFormat, operation);
        }
        auto file_phase = file_write_phase(
            context, VhdxFileWritePhase::BeforeExtend);
        if (!file_phase.ok()) {
            return Result<VhdxWrittenFile>::failure(file_phase.error());
        }
        auto extend = temporary_file->truncate(context, plan.file_length());
        if (!extend.ok()) {
            return translate_write_error<VhdxWrittenFile>(
                extend.error(), operation);
        }
        live = check_operation_context(context);
        if (!live.ok()) {
            return Result<VhdxWrittenFile>::failure(live.error());
        }

        file_phase = file_write_phase(
            context, VhdxFileWritePhase::BeforeStructure);
        if (!file_phase.ok()) {
            return Result<VhdxWrittenFile>::failure(file_phase.error());
        }
        std::array<std::byte, kIdentifierBytes> identifier{};
        put_ascii(identifier, 0, "vhdxfile");
        auto write = write_bytes(
            *temporary_file, context, 0, identifier, operation);
        if (!write.ok()) {
            return Result<VhdxWrittenFile>::failure(write.error());
        }
        const auto header1 = make_header(plan, 1);
        write = write_bytes(
            *temporary_file, context, kHeader1Offset, header1, operation);
        if (!write.ok()) {
            return Result<VhdxWrittenFile>::failure(write.error());
        }
        const auto header2 = make_header(plan, 2);
        write = write_bytes(
            *temporary_file, context, kHeader2Offset, header2, operation);
        if (!write.ok()) {
            return Result<VhdxWrittenFile>::failure(write.error());
        }
        const auto regions = make_region_table(plan);
        write = write_bytes(
            *temporary_file, context, kRegion1Offset, regions, operation);
        if (!write.ok()) {
            return Result<VhdxWrittenFile>::failure(write.error());
        }
        write = write_bytes(
            *temporary_file, context, kRegion2Offset, regions, operation);
        if (!write.ok()) {
            return Result<VhdxWrittenFile>::failure(write.error());
        }
        file_phase = file_write_phase(
            context, VhdxFileWritePhase::BeforeMetadata);
        if (!file_phase.ok()) {
            return Result<VhdxWrittenFile>::failure(file_phase.error());
        }
        const auto metadata = make_metadata_table();
        write = write_bytes(
            *temporary_file, context, plan.metadata_offset(),
            metadata, operation);
        if (!write.ok()) {
            return Result<VhdxWrittenFile>::failure(write.error());
        }
        const auto metadata_items = make_metadata_items(plan);
        write = write_bytes(
            *temporary_file, context,
            plan.metadata_offset() + kMetadataTableBytes,
            metadata_items, operation);
        if (!write.ok()) {
            return Result<VhdxWrittenFile>::failure(write.error());
        }

        file_phase = file_write_phase(
            context, VhdxFileWritePhase::BeforeBat);
        if (!file_phase.ok()) {
            return Result<VhdxWrittenFile>::failure(file_phase.error());
        }
        for (const auto& payload : plan.payloads()) {
            const auto bitmap_entries_before =
                payload.payload_index / plan.chunk_ratio();
            std::uint64_t bat_index = 0;
            std::uint64_t entry_delta = 0;
            std::uint64_t entry_offset = 0;
            if (!checked_add(
                    payload.payload_index, bitmap_entries_before,
                    bat_index) ||
                !checked_mul(bat_index, kBatEntryBytes, entry_delta) ||
                !checked_add(plan.bat_offset(), entry_delta, entry_offset)) {
                return writer_failure<VhdxWrittenFile>(
                    L"planned VHDX BAT entry offset overflows",
                    PersistenceErrorCode::InvalidFormat, operation);
            }
            std::array<std::byte, kBatEntryBytes> entry{};
            const auto raw =
                ((payload.file_offset / kMiB) << 20U) | 6U;
            put_u64(entry, 0, raw);
            write = write_bytes(
                *temporary_file, context, entry_offset, entry, operation);
            if (!write.ok()) {
                return Result<VhdxWrittenFile>::failure(write.error());
            }
        }

        file_phase = file_write_phase(
            context, VhdxFileWritePhase::BeforePayload);
        if (!file_phase.ok()) {
            return Result<VhdxWrittenFile>::failure(file_phase.error());
        }
        std::vector<std::byte> scratch(kVhdxDataBlockBytes);
        for (const auto& candidate : plan.candidates_) {
            std::fill(scratch.begin(), scratch.end(), std::byte{0});
            auto read = source.read_payload(
                context, candidate.payload_index, scratch);
            if (!read.ok()) {
                return Result<VhdxWrittenFile>::failure(read.error());
            }
            live = check_operation_context(context);
            if (!live.ok()) {
                return Result<VhdxWrittenFile>::failure(live.error());
            }
            auto digest = sha256_digest(scratch);
            if (!digest.ok()) {
                return Result<VhdxWrittenFile>::failure(digest.error());
            }
            if (digest.value() != candidate.digest) {
                return writer_failure<VhdxWrittenFile>(
                    L"VHDX payload source changed after layout planning",
                    PersistenceErrorCode::BackingDataCorrupt, operation);
            }
            if (!candidate.allocated) continue;
            write = write_bytes(
                *temporary_file, context, candidate.file_offset,
                scratch, operation);
            if (!write.ok()) {
                return Result<VhdxWrittenFile>::failure(write.error());
            }
        }

        file_phase = file_write_phase(
            context, VhdxFileWritePhase::BeforeFlush);
        if (!file_phase.ok()) {
            return Result<VhdxWrittenFile>::failure(file_phase.error());
        }
        auto flushed = temporary_file->flush(context);
        if (!flushed.ok()) {
            return translate_write_error<VhdxWrittenFile>(
                flushed.error(), operation);
        }
        live = check_operation_context(context);
        if (!live.ok()) {
            return Result<VhdxWrittenFile>::failure(live.error());
        }
        file_phase = file_write_phase(
            context, VhdxFileWritePhase::BeforeReadback);
        if (!file_phase.ok()) {
            return Result<VhdxWrittenFile>::failure(file_phase.error());
        }
        const std::shared_ptr<IPersistenceFile> parse_view(
            temporary_file.get(), [](IPersistenceFile*) noexcept {});
        auto parsed = parse_vhdx_layout(context, parse_view);
        if (!parsed.ok()) {
            return Result<VhdxWrittenFile>::failure(parsed.error());
        }
        if (!same_plan_layout(plan, parsed.value())) {
            return writer_failure<VhdxWrittenFile>(
                L"written VHDX layout differs from its immutable plan",
                PersistenceErrorCode::BackingDataCorrupt, operation);
        }
        return Result<VhdxWrittenFile>::success(VhdxWrittenFile(
            std::move(temporary_file), parsed.move_value()));
    } catch (const std::bad_alloc&) {
        return writer_failure<VhdxWrittenFile>(
            L"allocating bounded VHDX write state failed",
            PersistenceErrorCode::IoError, operation);
    }
}

VhdxPublishedBase::VhdxPublishedBase(
    std::string relative_name,
    WindowsFileIdentity windows_identity,
    VhdxLayout layout,
    std::shared_ptr<const ValidatedBundleFile> capability_anchor) noexcept
    : relative_name_(std::move(relative_name)),
      windows_identity_(std::move(windows_identity)),
      layout_(std::move(layout)),
      capability_anchor_(std::move(capability_anchor)) {}

const std::string& VhdxPublishedBase::relative_name() const noexcept {
    return relative_name_;
}

const WindowsFileIdentity&
VhdxPublishedBase::windows_identity() const noexcept {
    return windows_identity_;
}

const VhdxLayout& VhdxPublishedBase::layout() const noexcept {
    return layout_;
}

const std::shared_ptr<const ValidatedBundleFile>&
VhdxPublishedBase::capability_anchor() const noexcept {
    return capability_anchor_;
}

Result<VhdxPublishedBase> publish_sparse_vhdx_base(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    const std::string_view temporary_name,
    const std::string_view final_name,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source,
    VhdxPreRenameValidator pre_rename_validator) {
    constexpr auto operation = L"publish_sparse_vhdx_base";
    auto temporary_component = validate_image_component(temporary_name);
    if (!temporary_component.ok()) {
        return Result<VhdxPublishedBase>::failure(
            temporary_component.error());
    }
    auto final_component = validate_image_component(final_name);
    if (!final_component.ok()) {
        return Result<VhdxPublishedBase>::failure(final_component.error());
    }
    const auto names_alias = CompareStringOrdinal(
        temporary_component.value().wide_name.data(),
        static_cast<int>(temporary_component.value().wide_name.size()),
        final_component.value().wide_name.data(),
        static_cast<int>(final_component.value().wide_name.size()),
        TRUE) == CSTR_EQUAL;
    if (names_alias) {
        return writer_failure<VhdxPublishedBase>(
            L"VHDX temporary and final names must not alias on Windows",
            PersistenceErrorCode::InvalidFormat, operation);
    }
    try {
        auto phase = publication_phase(
            context, VhdxPublicationPhase::BeforeCreate);
        if (!phase.ok()) {
            return Result<VhdxPublishedBase>::failure(phase.error());
        }
        auto created = create_bundle_file_for_mutation(
            writer_lock, temporary_name);
        if (!created.ok()) {
            return Result<VhdxPublishedBase>::failure(created.error());
        }
        // Virtdisk rejects VHD/VHDX containers carrying the NTFS sparse
        // attribute (ERROR_VIRTUAL_DISK_LIMITATION). Dynamic sparseness is
        // represented by absent BAT entries and a short ordinary file.
        auto opened = open_validated_persistence_file(
            created.move_value());
        if (!opened.ok()) {
            return Result<VhdxPublishedBase>::failure(opened.error());
        }
        std::unique_ptr<IPersistenceFile> generic_file(
            opened.move_value().release());
        auto written = write_sparse_vhdx_file(
            context, plan, source, std::move(generic_file));
        if (!written.ok()) {
            return Result<VhdxPublishedBase>::failure(written.error());
        }
        auto parsed_layout = written.value().layout();
        auto generic_written = written.value().release_file();
        auto* validated_raw = dynamic_cast<ValidatedPersistenceFile*>(
            generic_written.get());
        if (validated_raw == nullptr) {
            return writer_failure<VhdxPublishedBase>(
                L"VHDX publisher lost its validated file capability",
                PersistenceErrorCode::FatalInternalInvariant, operation);
        }
        static_cast<void>(generic_written.release());
        std::unique_ptr<ValidatedPersistenceFile> validated_file(
            validated_raw);
        const auto expected_identity = validated_file->observed_identity();
        const auto temporary_path = validated_file->observed_final_path();
        if (temporary_path.empty()) {
            return writer_failure<VhdxPublishedBase>(
                L"VHDX publisher lost its validated temporary path",
                PersistenceErrorCode::FatalInternalInvariant, operation);
        }
        const auto bundle_root =
            std::filesystem::path(temporary_path).parent_path();
        phase = publication_phase(
            context, VhdxPublicationPhase::BeforeClose);
        if (!phase.ok()) {
            return Result<VhdxPublishedBase>::failure(phase.error());
        }
        Result<void> closed =
#if defined(MYRD_ENABLE_TEST_SEAMS)
            g_publication_options != nullptr &&
                g_publication_options->close_operation
            ? g_publication_options->close_operation(*validated_file)
            :
#endif
              validated_file->close();
        if (!closed.ok()) {
            return Result<VhdxPublishedBase>::failure(closed.error());
        }
        // Destroy the adapter and its retained capability before reopening.
        // close() above closes the duplicate I/O handle; reset closes the
        // original mutable handle and root handle as well.
        validated_file.reset();

        phase = publication_phase(
            context, VhdxPublicationPhase::BeforeReopen);
        if (!phase.ok()) {
            return Result<VhdxPublishedBase>::failure(phase.error());
        }
#if defined(MYRD_ENABLE_TEST_SEAMS)
        if (g_publication_options != nullptr &&
            g_publication_options->before_reopen_operation) {
            auto injected =
                g_publication_options->before_reopen_operation(
                    temporary_path);
            if (!injected.ok()) {
                return Result<VhdxPublishedBase>::failure(injected.error());
            }
        }
#endif
        auto inspection = open_bundle_file_for_inspection(
            bundle_root, temporary_name, expected_identity);
        if (!inspection.ok()) {
            return Result<VhdxPublishedBase>::failure(inspection.error());
        }
        auto read_file = open_validated_persistence_file(
            inspection.move_value());
        if (!read_file.ok()) {
            return Result<VhdxPublishedBase>::failure(read_file.error());
        }
        {
            const auto integrity_budget = (std::max)(
                std::uint64_t{2},
                static_cast<std::uint64_t>(plan.payloads().size()) + 2);
            auto reader_result = VhdxReader::create(
                context, read_file.move_value(), parsed_layout,
                {{16 * kVhdxBatPageBytes, 16},
                 integrity_budget, integrity_budget});
            if (!reader_result.ok()) {
                return Result<VhdxPublishedBase>::failure(
                    reader_result.error());
            }
            auto reader = reader_result.move_value();
            std::vector<std::byte> payload_bytes(kVhdxDataBlockBytes);
            std::array<std::byte, kBlockSizeBytes> block{};
            if (plan.payloads().empty()) {
                auto first = reader->read_block(context, 0, block);
                if (!first.ok()) {
                    return Result<VhdxPublishedBase>::failure(first.error());
                }
                const auto last_block =
                    (plan.virtual_size() - 1) / kBlockSizeBytes;
                if (last_block != 0) {
                    auto last = reader->read_block(
                        context, last_block, block);
                    if (!last.ok()) {
                        return Result<VhdxPublishedBase>::failure(last.error());
                    }
                }
            }
            for (const auto& payload : plan.payloads()) {
                std::fill(
                    payload_bytes.begin(), payload_bytes.end(),
                    std::byte{0});
                const auto virtual_offset =
                    payload.payload_index * kVhdxDataBlockBytes;
                const auto logical_bytes = (std::min)(
                    static_cast<std::uint64_t>(kVhdxDataBlockBytes),
                    plan.virtual_size() - virtual_offset);
                const auto block_count =
                    (logical_bytes + kBlockSizeBytes - 1) /
                    kBlockSizeBytes;
                for (std::uint64_t slice = 0;
                     slice < block_count; ++slice) {
                    auto loaded = reader->read_block(
                        context,
                        payload.payload_index *
                            (kVhdxDataBlockBytes / kBlockSizeBytes) + slice,
                        block);
                    if (!loaded.ok()) {
                        return Result<VhdxPublishedBase>::failure(
                            loaded.error());
                    }
                    if (loaded.value().kind != BlockLoadKind::Data) {
                        return writer_failure<VhdxPublishedBase>(
                            L"published VHDX payload read back as zero",
                            PersistenceErrorCode::BackingDataCorrupt,
                            operation);
                    }
                    std::copy(
                        block.begin(), block.end(),
                        payload_bytes.begin() +
                            static_cast<std::ptrdiff_t>(
                                slice * kBlockSizeBytes));
                }
                auto digest = sha256_digest(payload_bytes);
                if (!digest.ok()) {
                    return Result<VhdxPublishedBase>::failure(digest.error());
                }
                if (digest.value() != payload.digest) {
                    return writer_failure<VhdxPublishedBase>(
                        L"published VHDX payload readback digest differs",
                        PersistenceErrorCode::BackingDataCorrupt,
                        operation);
                }
            }
        }

        // Keep a read-only identity pin while the system VHDX validator has the
        // file open. A GENERIC_WRITE/DELETE handle with share-read only is
        // intentionally not used here: OpenVirtualDisk cannot coexist with
        // that share contract.
        {
            auto validation_anchor = open_bundle_file_for_offline_validation(
                bundle_root, temporary_name, expected_identity);
            if (!validation_anchor.ok()) {
                return Result<VhdxPublishedBase>::failure(
                    validation_anchor.error());
            }
            if (pre_rename_validator) {
                auto validated = Result<WindowsFileIdentity>::failure(
                    writer_failure<void>(
                        L"VHDX pre-rename validator did not execute",
                        PersistenceErrorCode::FatalInternalInvariant,
                        operation).error());
                try {
                    validated = pre_rename_validator(context, temporary_path);
                } catch (...) {
                    return writer_failure<VhdxPublishedBase>(
                        L"VHDX pre-rename validator threw an exception",
                        PersistenceErrorCode::FatalInternalInvariant,
                        operation);
                }
                if (!validated.ok()) {
                    return Result<VhdxPublishedBase>::failure(
                        validated.error());
                }
                if (!same_file_object(validated.value(), expected_identity) ||
                    validated.value().size != expected_identity.size) {
                    return writer_failure<VhdxPublishedBase>(
                        L"VHDX offline validation observed a different file object",
                        PersistenceErrorCode::GenerationConflict, operation);
                }
            }

        }

        // Validation has released every external VHDX handle. Reacquire the
        // same object with write/delete authority because FlushFileBuffers on
        // the renamed file requires write access. The expected identity check
        // rejects a namespace substitution between the two pins.
        {
            auto rename_capability = open_bundle_file_for_mutation(
                writer_lock, temporary_name, expected_identity);
            if (!rename_capability.ok()) {
                return Result<VhdxPublishedBase>::failure(
                    rename_capability.error());
            }
            phase = publication_phase(
                context, VhdxPublicationPhase::BeforeRename);
            if (!phase.ok()) {
                return Result<VhdxPublishedBase>::failure(phase.error());
            }
            Result<void> renamed =
#if defined(MYRD_ENABLE_TEST_SEAMS)
                g_publication_options != nullptr &&
                    g_publication_options->rename_backend != nullptr
                ? test::write_through_rename_validated_bundle_file_with_backend(
                      context, writer_lock, rename_capability.value(), final_name,
                      *g_publication_options->rename_backend)
                :
#endif
                  write_through_rename_validated_bundle_file(
                      context, writer_lock, rename_capability.value(),
                      final_name);
            if (!renamed.ok()) {
                return Result<VhdxPublishedBase>::failure(renamed.error());
            }
        }

        // The validation and rename handles are now closed.  Reopen the exact
        // renamed object with no write/delete sharing and retain that anchor
        // through manifest commit/reconciliation.  Identity validation closes
        // the small namespace-swap window between rename and this reopen.
        auto final_anchor = open_bundle_file_for_immutable_anchor(
            writer_lock, final_name, expected_identity);
        if (!final_anchor.ok()) {
            return Result<VhdxPublishedBase>::failure(final_anchor.error());
        }
        auto retained_anchor = std::make_shared<ValidatedBundleFile>(
            final_anchor.move_value());
        const auto final_identity = retained_anchor->identity();
        return Result<VhdxPublishedBase>::success(VhdxPublishedBase(
            std::string{final_name}, final_identity,
            std::move(parsed_layout), std::move(retained_anchor)));
    } catch (const std::bad_alloc&) {
        return writer_failure<VhdxPublishedBase>(
            L"allocating VHDX publication state failed",
            PersistenceErrorCode::IoError, operation);
    }
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<VhdxPublishedBase> publish_sparse_vhdx_base_for_testing(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    const std::string_view temporary_name,
    const std::string_view final_name,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source,
    VhdxPublicationHook hook) {
    VhdxPublicationTestOptions options;
    options.phase_hook = std::move(hook);
    return publish_sparse_vhdx_base_for_testing(
        context, writer_lock, temporary_name, final_name,
        plan, source, std::move(options));
}

Result<VhdxPublishedBase> publish_sparse_vhdx_base_for_testing(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    const std::string_view temporary_name,
    const std::string_view final_name,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source,
    VhdxPublicationTestOptions options) {
    if ((!options.phase_hook && !options.file_write_hook &&
         options.rename_backend == nullptr && !options.close_operation &&
         !options.before_reopen_operation &&
         !options.pre_rename_validator) ||
        g_publication_options != nullptr) {
        return writer_failure<VhdxPublishedBase>(
            L"VHDX publication test options must be nonempty and nonnested",
            PersistenceErrorCode::FatalInternalInvariant,
            L"publish_sparse_vhdx_base_for_testing");
    }
    struct ScopedOptions final {
        explicit ScopedOptions(
            const VhdxPublicationTestOptions& value) noexcept {
            g_publication_options = &value;
        }
        ~ScopedOptions() noexcept { g_publication_options = nullptr; }
    } scoped_options(options);
    return publish_sparse_vhdx_base(
        context, writer_lock, temporary_name, final_name, plan, source,
        std::move(options.pre_rename_validator));
}
#endif

}  // namespace myrd::persistence
