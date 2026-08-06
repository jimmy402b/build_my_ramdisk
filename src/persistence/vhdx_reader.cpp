#include "src/persistence/vhdx_reader.h"

#include "src/persistence/image_paths.h"
#include "src/persistence/persistence_types.h"
#include "src/storage/block_constants.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <list>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace myrd::persistence {
namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint64_t kBatEntryBytes = 8;
constexpr std::uint64_t kBatStateMask = 0x7;
constexpr std::uint64_t kBatReservedMask = 0x00000000000ffff8ULL;
constexpr unsigned kBatFileOffsetShift = 20;

enum class PayloadState : std::uint8_t {
    NotPresent = 0,
    Undefined = 1,
    Zero = 2,
    Unmapped = 3,
    FullyPresent = 6,
    PartiallyPresent = 7,
};

template <typename T>
Result<T> reader_failure(
    std::wstring_view message,
    PersistenceErrorCode code = PersistenceErrorCode::BackingDataCorrupt,
    std::uint32_t native_code = 0,
    std::wstring_view operation = L"VhdxReader::read_block") {
    return Result<T>::failure({
        ErrorDomain::persistence, static_cast<std::uint32_t>(code),
        std::wstring{operation}, std::wstring{message}, native_code});
}

bool context_error(const Error& error) noexcept {
    if (error.domain != ErrorDomain::storage) return false;
    return error.code == static_cast<std::uint32_t>(
                             OperationErrorCode::cancelled) ||
           error.code == static_cast<std::uint32_t>(
                             OperationErrorCode::timed_out);
}

template <typename T>
Result<T> translate_io_error(
    const Error& error,
    std::wstring_view operation,
    bool eof_is_corruption = true) {
    if (context_error(error)) return Result<T>::failure(error);
    if (eof_is_corruption && error.native_code == ERROR_HANDLE_EOF) {
        return reader_failure<T>(
            L"referenced VHDX bytes are truncated",
            PersistenceErrorCode::BackingDataCorrupt, error.native_code,
            operation);
    }
    const auto code = error.native_code == ERROR_ACCESS_DENIED
        ? PersistenceErrorCode::AccessDenied
        : PersistenceErrorCode::IoError;
    return reader_failure<T>(
        L"reading VHDX failed", code, error.native_code, operation);
}

bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t& result) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool checked_mul(std::uint64_t left, std::uint64_t right,
                 std::uint64_t& result) noexcept {
    if (left != 0 &&
        right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool overlaps(std::uint64_t left_offset, std::uint64_t left_length,
              std::uint64_t right_offset, std::uint64_t right_length) noexcept {
    std::uint64_t left_end = 0;
    std::uint64_t right_end = 0;
    if (!checked_add(left_offset, left_length, left_end) ||
        !checked_add(right_offset, right_length, right_end)) {
        return true;
    }
    return left_offset < right_end && right_offset < left_end;
}

std::uint64_t decode_u64(std::span<const std::byte, 8> bytes) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
    }
    return result;
}

bool bytes_less(const std::array<std::byte, 16>& left,
                const std::array<std::byte, 16>& right) noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end());
}

bool same_layout(const VhdxLayout& left, const VhdxLayout& right) noexcept {
    return left.virtual_size() == right.virtual_size() &&
           left.logical_sector_size() == right.logical_sector_size() &&
           left.physical_sector_size() == right.physical_sector_size() &&
           left.block_size() == right.block_size() &&
           left.payload_block_count() == right.payload_block_count() &&
           left.chunk_ratio() == right.chunk_ratio() &&
           left.bat_entry_count() == right.bat_entry_count() &&
           left.bat_offset() == right.bat_offset() &&
           left.bat_length() == right.bat_length() &&
           left.metadata_offset() == right.metadata_offset() &&
           left.metadata_length() == right.metadata_length() &&
           left.log_offset() == right.log_offset() &&
           left.log_length() == right.log_length() &&
           left.file_length() == right.file_length() &&
           left.file_identity() == right.file_identity() &&
           left.virtual_disk_id() == right.virtual_disk_id();
}

struct BatPageKey final {
    WindowsFileIdentity windows{};
    VhdxFileIdentity vhdx{};
    std::uint64_t page_index{};
};

struct BatPageKeyLess final {
    bool operator()(const BatPageKey& left,
                    const BatPageKey& right) const noexcept {
        if (left.windows.volume_serial_number !=
            right.windows.volume_serial_number) {
            return left.windows.volume_serial_number <
                   right.windows.volume_serial_number;
        }
        if (left.windows.file_id != right.windows.file_id) {
            return bytes_less(left.windows.file_id, right.windows.file_id);
        }
        if (left.windows.size != right.windows.size) {
            return left.windows.size < right.windows.size;
        }
        if (left.windows.attributes != right.windows.attributes) {
            return left.windows.attributes < right.windows.attributes;
        }
        if (left.windows.reparse_tag != right.windows.reparse_tag) {
            return left.windows.reparse_tag < right.windows.reparse_tag;
        }
        if (left.windows.hard_link_count != right.windows.hard_link_count) {
            return left.windows.hard_link_count < right.windows.hard_link_count;
        }
        if (left.vhdx.file_write_guid != right.vhdx.file_write_guid) {
            return bytes_less(
                left.vhdx.file_write_guid, right.vhdx.file_write_guid);
        }
        if (left.vhdx.data_write_guid != right.vhdx.data_write_guid) {
            return bytes_less(
                left.vhdx.data_write_guid, right.vhdx.data_write_guid);
        }
        return left.page_index < right.page_index;
    }
};

struct DecodedPayloadEntry final {
    PayloadState state{};
    std::uint64_t file_offset{};
};

}  // namespace

struct VhdxReader::Impl final {
    struct CacheEntry final {
        std::array<std::byte, kVhdxBatPageBytes> bytes{};
        std::list<BatPageKey>::iterator lru{};
    };

    Impl(std::unique_ptr<IPersistenceFile> source_file,
         WindowsFileIdentity source_windows_identity,
         VhdxLayout source_layout,
         VhdxReaderOptions source_options)
        : file(std::move(source_file)),
          windows_identity(std::move(source_windows_identity)),
          layout(std::move(source_layout)),
          options(source_options) {}

    std::unique_ptr<IPersistenceFile> file;
    WindowsFileIdentity windows_identity{};
    VhdxLayout layout;
    VhdxReaderOptions options{};
    mutable std::mutex mutex;
    mutable std::map<BatPageKey, CacheEntry, BatPageKeyLess> cache;
    mutable std::list<BatPageKey> lru;
    mutable VhdxReaderStats statistics{};
    mutable std::uint64_t cache_generation{};
    struct ObservedPayload final {
        std::uint64_t raw{};
        std::uint64_t file_offset{};
        std::uint64_t file_end{};
    };
    mutable std::mutex integrity_mutex;
    mutable std::map<std::uint64_t, ObservedPayload> observed_payloads;
    mutable std::map<std::uint64_t, std::uint64_t>
        observed_physical_ranges;
    mutable bool integrity_corrupt{};

    Result<void> verify_length(
        const OperationContext& context,
        std::wstring_view operation) const {
        auto live = check_operation_context(context);
        if (!live.ok()) return live;
        auto length = file->length(context);
        if (!length.ok()) {
            return translate_io_error<void>(
                length.error(), operation, false);
        }
        if (length.value() != layout.file_length() ||
            length.value() != windows_identity.size) {
            return reader_failure<void>(
                L"VHDX length changed after structural validation",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        live = check_operation_context(context);
        if (!live.ok()) return live;
        return Result<void>::success();
    }

    BatPageKey page_key(std::uint64_t page_index) const {
        return {windows_identity, layout.file_identity(), page_index};
    }

    Result<std::uint64_t> read_bat_entry(
        const OperationContext& context,
        std::uint64_t entry_index,
        std::wstring_view operation) const {
        if (entry_index >= layout.bat_entry_count()) {
            return reader_failure<std::uint64_t>(
                L"BAT entry index exceeds validated geometry",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        std::uint64_t entry_offset = 0;
        if (!checked_mul(entry_index, kBatEntryBytes, entry_offset)) {
            return reader_failure<std::uint64_t>(
                L"BAT entry offset overflows",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        const auto page_index = entry_offset / kVhdxBatPageBytes;
        const auto within_page = static_cast<std::size_t>(
            entry_offset % kVhdxBatPageBytes);
        const auto key = page_key(page_index);

        try {
            std::uint64_t observed_generation = 0;
            {
                std::unique_lock lock(mutex);
                auto live = check_operation_context(context);
                if (!live.ok()) {
                    return Result<std::uint64_t>::failure(live.error());
                }
                const auto found = cache.find(key);
                if (found != cache.end()) {
                    ++statistics.bat_cache_hits;
                    lru.splice(lru.begin(), lru, found->second.lru);
                    return Result<std::uint64_t>::success(decode_u64(
                        std::span<const std::byte, 8>{
                            found->second.bytes.data() + within_page, 8}));
                }
                ++statistics.bat_cache_misses;
                ++statistics.bat_read_calls;
                observed_generation = cache_generation;
            }

            std::uint64_t page_relative = 0;
            std::uint64_t page_offset = 0;
            std::uint64_t page_end = 0;
            std::uint64_t bat_end = 0;
            if (!checked_mul(page_index, kVhdxBatPageBytes, page_relative) ||
                !checked_add(layout.bat_offset(), page_relative, page_offset) ||
                !checked_add(page_offset, kVhdxBatPageBytes, page_end) ||
                !checked_add(layout.bat_offset(), layout.bat_length(), bat_end) ||
                page_end > bat_end || page_end > layout.file_length()) {
                return reader_failure<std::uint64_t>(
                    L"BAT cache page exceeds validated region",
                    PersistenceErrorCode::BackingDataCorrupt, 0, operation);
            }

            std::array<std::byte, kVhdxBatPageBytes> bytes{};
            auto read = file->read_exact(context, page_offset, bytes);
            if (!read.ok()) {
                return translate_io_error<std::uint64_t>(
                    read.error(), operation);
            }
            auto live = check_operation_context(context);
            if (!live.ok()) {
                return Result<std::uint64_t>::failure(live.error());
            }

            std::unique_lock lock(mutex);
            const auto raced = cache.find(key);
            if (raced != cache.end()) {
                ++statistics.bat_cache_hits;
                lru.splice(lru.begin(), lru, raced->second.lru);
                return Result<std::uint64_t>::success(decode_u64(
                    std::span<const std::byte, 8>{
                        raced->second.bytes.data() + within_page, 8}));
            }
            if (cache_generation != observed_generation) {
                return Result<std::uint64_t>::success(decode_u64(
                    std::span<const std::byte, 8>{
                        bytes.data() + within_page, 8}));
            }

            lru.push_front(key);
            CacheEntry entry{};
            entry.bytes = bytes;
            entry.lru = lru.begin();
            try {
                const auto inserted = cache.emplace(key, std::move(entry));
                if (!inserted.second) {
                    lru.pop_front();
                    return reader_failure<std::uint64_t>(
                        L"BAT cache insertion conflicted",
                        PersistenceErrorCode::FatalInternalInvariant, 0,
                        operation);
                }
            } catch (...) {
                lru.pop_front();
                throw;
            }
            while (cache.size() > options.bat_cache.max_entries ||
                   cache.size() * kVhdxBatPageBytes >
                       options.bat_cache.max_bytes) {
                if (lru.empty()) {
                    return reader_failure<std::uint64_t>(
                        L"BAT cache accounting is inconsistent",
                        PersistenceErrorCode::FatalInternalInvariant, 0,
                        operation);
                }
                const auto victim = lru.back();
                cache.erase(victim);
                lru.pop_back();
                ++statistics.bat_cache_evictions;
            }
            statistics.current_cache_entries = cache.size();
            statistics.current_cache_bytes =
                cache.size() * kVhdxBatPageBytes;
            statistics.peak_cache_entries = (std::max)(
                statistics.peak_cache_entries,
                statistics.current_cache_entries);
            statistics.peak_cache_bytes = (std::max)(
                statistics.peak_cache_bytes,
                statistics.current_cache_bytes);
            const auto inserted = cache.find(key);
            if (inserted == cache.end()) {
                return reader_failure<std::uint64_t>(
                    L"BAT cache evicted the page being inserted",
                    PersistenceErrorCode::FatalInternalInvariant, 0,
                    operation);
            }
            return Result<std::uint64_t>::success(decode_u64(
                std::span<const std::byte, 8>{
                    inserted->second.bytes.data() + within_page, 8}));
        } catch (const std::bad_alloc&) {
            return reader_failure<std::uint64_t>(
                L"allocating bounded BAT cache state failed",
                PersistenceErrorCode::IoError, 0, operation);
        }
    }

    Result<DecodedPayloadEntry> decode_payload_entry(
        std::uint64_t raw,
        std::wstring_view operation) const {
        if ((raw & kBatReservedMask) != 0) {
            return reader_failure<DecodedPayloadEntry>(
                L"payload BAT entry has nonzero reserved bits",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        const auto state_value = raw & kBatStateMask;
        const auto offset_mb = raw >> kBatFileOffsetShift;
        if (state_value == 4 || state_value == 5 || state_value > 7) {
            return reader_failure<DecodedPayloadEntry>(
                L"payload BAT state is reserved",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        const auto state = static_cast<PayloadState>(state_value);
        if (state == PayloadState::PartiallyPresent) {
            return reader_failure<DecodedPayloadEntry>(
                L"partially present payload requires differencing VHDX",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        if ((state == PayloadState::NotPresent ||
             state == PayloadState::Zero) && offset_mb != 0) {
            return reader_failure<DecodedPayloadEntry>(
                L"unallocated payload BAT entry has a physical offset",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        std::uint64_t file_offset = 0;
        if (!checked_mul(offset_mb, kMiB, file_offset)) {
            return reader_failure<DecodedPayloadEntry>(
                L"payload BAT file offset overflows",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        return Result<DecodedPayloadEntry>::success({state, file_offset});
    }

    Result<void> validate_payload_range(
        std::uint64_t offset,
        std::wstring_view operation) const {
        std::uint64_t end = 0;
        if (offset < kMiB ||
            !checked_add(offset, layout.block_size(), end) ||
            end > layout.file_length() ||
            overlaps(offset, layout.block_size(), 0, kMiB) ||
            overlaps(offset, layout.block_size(),
                     layout.bat_offset(), layout.bat_length()) ||
            overlaps(offset, layout.block_size(),
                     layout.metadata_offset(), layout.metadata_length()) ||
            overlaps(offset, layout.block_size(),
                     layout.log_offset(), layout.log_length())) {
            return reader_failure<void>(
                L"payload BAT range overlaps structure or file boundary",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        return Result<void>::success();
    }

    Result<std::uint64_t> payload_bat_index(
        std::uint64_t payload_index,
        std::wstring_view operation) const {
        if (payload_index >= layout.payload_block_count() ||
            layout.chunk_ratio() == 0) {
            return reader_failure<std::uint64_t>(
                L"payload block index exceeds validated geometry",
                PersistenceErrorCode::InvalidFormat, 0, operation);
        }
        const auto bitmap_entries_before =
            payload_index / layout.chunk_ratio();
        std::uint64_t result = 0;
        if (!checked_add(payload_index, bitmap_entries_before, result) ||
            result >= layout.bat_entry_count()) {
            return reader_failure<std::uint64_t>(
                L"payload BAT index overflows validated table",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        return Result<std::uint64_t>::success(result);
    }

    Result<void> validate_bitmap_entry(
        const OperationContext& context,
        std::uint64_t entry_index,
        std::wstring_view operation) const {
        auto raw = read_bat_entry(context, entry_index, operation);
        if (!raw.ok()) return Result<void>::failure(raw.error());
        if (raw.value() != 0) {
            return reader_failure<void>(
                L"dynamic VHDX sector-bitmap BAT entry is allocated",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        return Result<void>::success();
    }

    Result<void> validate_relevant_bitmap_entries(
        const OperationContext& context,
        std::uint64_t payload_index,
        std::wstring_view operation) const {
        const auto chunk = payload_index / layout.chunk_ratio();
        const auto chunk_start = chunk * layout.chunk_ratio();
        if (chunk != 0) {
            auto first = payload_bat_index(chunk_start, operation);
            if (!first.ok()) return Result<void>::failure(first.error());
            auto previous = validate_bitmap_entry(
                context, first.value() - 1, operation);
            if (!previous.ok()) return previous;
        }
        std::uint64_t next_chunk_start = 0;
        if (checked_add(chunk_start, layout.chunk_ratio(), next_chunk_start) &&
            next_chunk_start < layout.payload_block_count()) {
            auto next = payload_bat_index(next_chunk_start, operation);
            if (!next.ok()) return Result<void>::failure(next.error());
            auto current = validate_bitmap_entry(
                context, next.value() - 1, operation);
            if (!current.ok()) return current;
        }
        return Result<void>::success();
    }

    Result<void> observe_payload_entry(
        std::uint64_t payload_index,
        std::uint64_t raw,
        const DecodedPayloadEntry& entry,
        std::wstring_view operation) const {
        if (entry.file_offset != 0) {
            auto range = validate_payload_range(entry.file_offset, operation);
            if (!range.ok()) return range;
        }
        std::uint64_t file_end = entry.file_offset;
        if (entry.file_offset != 0 &&
            !checked_add(entry.file_offset, layout.block_size(), file_end)) {
            return reader_failure<void>(
                L"payload BAT range overflows",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        try {
            std::scoped_lock lock(integrity_mutex);
            if (integrity_corrupt) {
                return reader_failure<void>(
                    L"VHDX reader latched BAT integrity corruption",
                    PersistenceErrorCode::BackingDataCorrupt, 0, operation);
            }
            const auto existing = observed_payloads.find(payload_index);
            if (existing != observed_payloads.end()) {
                if (existing->second.raw != raw) {
                    integrity_corrupt = true;
                    return reader_failure<void>(
                        L"payload BAT entry changed after observation",
                        PersistenceErrorCode::BackingDataCorrupt, 0,
                        operation);
                }
                return Result<void>::success();
            }
            if (entry.file_offset != 0) {
                const auto next = observed_physical_ranges.lower_bound(
                    entry.file_offset);
                if ((next != observed_physical_ranges.end() &&
                     next->first < file_end) ||
                    (next != observed_physical_ranges.begin() &&
                     std::prev(next)->second > entry.file_offset)) {
                    integrity_corrupt = true;
                    return reader_failure<void>(
                        L"touched VHDX payload ranges overlap",
                        PersistenceErrorCode::BackingDataCorrupt, 0,
                        operation);
                }
            }
            if (observed_payloads.size() >=
                options.max_observed_payloads) {
                return reader_failure<void>(
                    L"observed BAT integrity state exceeds caller budget",
                    PersistenceErrorCode::IoError, 0, operation);
            }
            const auto inserted = observed_payloads.emplace(
                payload_index,
                ObservedPayload{raw, entry.file_offset, file_end});
            if (!inserted.second) {
                return reader_failure<void>(
                    L"observed payload insertion conflicted",
                    PersistenceErrorCode::FatalInternalInvariant, 0,
                    operation);
            }
            if (entry.file_offset != 0) {
                try {
                    const auto range_inserted =
                        observed_physical_ranges.emplace(
                            entry.file_offset, file_end);
                    if (!range_inserted.second) {
                        observed_payloads.erase(inserted.first);
                        integrity_corrupt = true;
                        return reader_failure<void>(
                            L"observed physical range insertion conflicted",
                            PersistenceErrorCode::BackingDataCorrupt, 0,
                            operation);
                    }
                } catch (...) {
                    observed_payloads.erase(inserted.first);
                    throw;
                }
            }
            return Result<void>::success();
        } catch (const std::bad_alloc&) {
            return reader_failure<void>(
                L"allocating bounded BAT integrity state failed",
                PersistenceErrorCode::IoError, 0, operation);
        }
    }

    Result<void> check_integrity_latch(
        std::wstring_view operation) const {
        std::scoped_lock lock(integrity_mutex);
        if (!integrity_corrupt) return Result<void>::success();
        return reader_failure<void>(
            L"VHDX reader latched BAT integrity corruption",
            PersistenceErrorCode::BackingDataCorrupt, 0, operation);
    }
};

VhdxReader::VhdxReader(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
VhdxReader::~VhdxReader() = default;
VhdxReader::VhdxReader(VhdxReader&&) noexcept = default;
VhdxReader& VhdxReader::operator=(VhdxReader&&) noexcept = default;

Result<std::unique_ptr<VhdxReader>> VhdxReader::open(
    const OperationContext& context,
    std::unique_ptr<ValidatedPersistenceFile>& file,
    const VhdxReaderOptions options,
    const VhdxParseRequirements requirements,
    VhdxParseEvidence* evidence) {
    constexpr auto operation = L"VhdxReader::open";
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::unique_ptr<VhdxReader>>::failure(live.error());
    }
    if (!file) {
        return reader_failure<std::unique_ptr<VhdxReader>>(
            L"validated VHDX file is missing",
            PersistenceErrorCode::InvalidFormat, 0, operation);
    }
    const auto identity = file->observed_identity();
    const std::shared_ptr<IPersistenceFile> parse_view(
        file.get(), [](IPersistenceFile*) noexcept {});
    auto layout = parse_vhdx_layout(
        context, parse_view, requirements, evidence);
    if (!layout.ok()) {
        return Result<std::unique_ptr<VhdxReader>>::failure(layout.error());
    }
    return create_parsed(
        context, std::unique_ptr<IPersistenceFile>(std::move(file)),
        identity, layout.move_value(), options);
}

Result<std::unique_ptr<VhdxReader>> VhdxReader::create(
    std::unique_ptr<ValidatedPersistenceFile> file,
    VhdxLayout layout,
    VhdxReaderOptions options) {
    return create(
        OperationContext{}, std::move(file), std::move(layout), options);
}

Result<std::unique_ptr<VhdxReader>> VhdxReader::create(
    const OperationContext& context,
    std::unique_ptr<ValidatedPersistenceFile> file,
    VhdxLayout layout,
    VhdxReaderOptions options) {
    constexpr auto operation = L"VhdxReader::create";
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::unique_ptr<VhdxReader>>::failure(live.error());
    }
    if (!file) {
        return reader_failure<std::unique_ptr<VhdxReader>>(
            L"validated VHDX file is missing",
            PersistenceErrorCode::InvalidFormat, 0, operation);
    }
    const auto identity = file->observed_identity();
    return create_owned(
        context,
        std::unique_ptr<IPersistenceFile>(std::move(file)),
        identity, std::move(layout), options);
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<std::unique_ptr<VhdxReader>> VhdxReader::create_for_testing(
    std::unique_ptr<IPersistenceFile> file,
    WindowsFileIdentity windows_identity,
    VhdxLayout layout,
    VhdxReaderOptions options) {
    return create_for_testing(
        OperationContext{}, std::move(file), std::move(windows_identity),
        std::move(layout), options);
}

Result<std::unique_ptr<VhdxReader>> VhdxReader::create_for_testing(
    const OperationContext& context,
    std::unique_ptr<IPersistenceFile> file,
    WindowsFileIdentity windows_identity,
    VhdxLayout layout,
    VhdxReaderOptions options) {
    return create_owned(
        context,
        std::move(file), std::move(windows_identity),
        std::move(layout), options);
}
#endif

Result<std::unique_ptr<VhdxReader>> VhdxReader::create_owned(
    const OperationContext& context,
    std::unique_ptr<IPersistenceFile> file,
    WindowsFileIdentity windows_identity,
    VhdxLayout layout,
    VhdxReaderOptions options) {
    constexpr auto operation = L"VhdxReader::create";
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::unique_ptr<VhdxReader>>::failure(live.error());
    }
    if (!file || options.bat_cache.max_entries == 0 ||
        options.bat_cache.max_bytes < kVhdxBatPageBytes ||
        options.bat_cache.max_entries >
            options.bat_cache.max_bytes / kVhdxBatPageBytes ||
        options.max_extents == 0 || options.max_observed_payloads == 0 ||
        windows_identity.size != layout.file_length()) {
        return reader_failure<std::unique_ptr<VhdxReader>>(
            L"VHDX reader options are invalid",
            PersistenceErrorCode::InvalidFormat, 0, operation);
    }
    try {
        const std::shared_ptr<IPersistenceFile> parse_view(
            file.get(), [](IPersistenceFile*) noexcept {});
        auto reparsed = parse_vhdx_layout(context, parse_view);
        if (!reparsed.ok()) {
            return Result<std::unique_ptr<VhdxReader>>::failure(
                reparsed.error());
        }
        if (!same_layout(reparsed.value(), layout)) {
            return reader_failure<std::unique_ptr<VhdxReader>>(
                L"VHDX file does not match the supplied immutable layout",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        return create_parsed(
            context, std::move(file), std::move(windows_identity),
            std::move(layout), options);
    } catch (const std::bad_alloc&) {
        return reader_failure<std::unique_ptr<VhdxReader>>(
            L"allocating VHDX reader failed",
            PersistenceErrorCode::IoError, 0, operation);
    }
}

Result<std::unique_ptr<VhdxReader>> VhdxReader::create_parsed(
    const OperationContext& context,
    std::unique_ptr<IPersistenceFile> file,
    WindowsFileIdentity windows_identity,
    VhdxLayout layout,
    const VhdxReaderOptions options) {
    constexpr auto operation = L"VhdxReader::create";
    if (!file || options.bat_cache.max_entries == 0 ||
        options.bat_cache.max_bytes < kVhdxBatPageBytes ||
        options.bat_cache.max_entries >
            options.bat_cache.max_bytes / kVhdxBatPageBytes ||
        options.max_extents == 0 || options.max_observed_payloads == 0 ||
        windows_identity.size != layout.file_length()) {
        return reader_failure<std::unique_ptr<VhdxReader>>(
            L"VHDX reader options are invalid",
            PersistenceErrorCode::InvalidFormat, 0, operation);
    }
    try {
        auto impl = std::make_unique<Impl>(
            std::move(file), std::move(windows_identity),
            std::move(layout), options);
        auto length = impl->verify_length(context, operation);
        if (!length.ok()) {
            return Result<std::unique_ptr<VhdxReader>>::failure(
                length.error());
        }
        return Result<std::unique_ptr<VhdxReader>>::success(
            std::unique_ptr<VhdxReader>(
                new VhdxReader(std::move(impl))));
    } catch (const std::bad_alloc&) {
        return reader_failure<std::unique_ptr<VhdxReader>>(
            L"allocating VHDX reader failed",
            PersistenceErrorCode::IoError, 0, operation);
    }
}

Result<BlockLoadResult> VhdxReader::read_block(
    const OperationContext& context,
    std::uint64_t block_index,
    std::span<std::byte> destination) const {
    constexpr auto operation = L"VhdxReader::read_block";
    if (!impl_) {
        return reader_failure<BlockLoadResult>(
            L"VHDX reader is moved from",
            PersistenceErrorCode::FatalInternalInvariant, 0, operation);
    }
    if (destination.size() != kBlockSizeBytes) {
        return reader_failure<BlockLoadResult>(
            L"destination must be exactly one M2 block",
            PersistenceErrorCode::InvalidFormat, 0, operation);
    }
    auto integrity = impl_->check_integrity_latch(operation);
    if (!integrity.ok()) {
        return Result<BlockLoadResult>::failure(integrity.error());
    }
    std::uint64_t logical_offset = 0;
    if (!checked_mul(block_index, kBlockSizeBytes, logical_offset) ||
        logical_offset >= impl_->layout.virtual_size()) {
        return reader_failure<BlockLoadResult>(
            L"block index exceeds VHDX virtual geometry",
            PersistenceErrorCode::InvalidFormat, 0, operation);
    }
    auto length = impl_->verify_length(context, operation);
    if (!length.ok()) return Result<BlockLoadResult>::failure(length.error());

    const auto payload_index = logical_offset / impl_->layout.block_size();
    const auto slice_offset = logical_offset % impl_->layout.block_size();
    auto bat_index = impl_->payload_bat_index(payload_index, operation);
    if (!bat_index.ok()) {
        return Result<BlockLoadResult>::failure(bat_index.error());
    }
    auto raw = impl_->read_bat_entry(context, bat_index.value(), operation);
    if (!raw.ok()) return Result<BlockLoadResult>::failure(raw.error());
    auto entry = impl_->decode_payload_entry(raw.value(), operation);
    if (!entry.ok()) return Result<BlockLoadResult>::failure(entry.error());
    auto bitmap = impl_->validate_relevant_bitmap_entries(
        context, payload_index, operation);
    if (!bitmap.ok()) {
        return Result<BlockLoadResult>::failure(bitmap.error());
    }
    auto observed = impl_->observe_payload_entry(
        payload_index, raw.value(), entry.value(), operation);
    if (!observed.ok()) {
        return Result<BlockLoadResult>::failure(observed.error());
    }

    if (entry.value().state != PayloadState::FullyPresent) {
        auto live = check_operation_context(context);
        if (!live.ok()) return Result<BlockLoadResult>::failure(live.error());
        return Result<BlockLoadResult>::success({BlockLoadKind::AllZero});
    }
    auto valid = impl_->validate_payload_range(
        entry.value().file_offset, operation);
    if (!valid.ok()) return Result<BlockLoadResult>::failure(valid.error());

    std::uint64_t read_offset = 0;
    if (!checked_add(entry.value().file_offset, slice_offset, read_offset)) {
        return reader_failure<BlockLoadResult>(
            L"payload slice offset overflows",
            PersistenceErrorCode::BackingDataCorrupt, 0, operation);
    }
    const auto logical_length = static_cast<std::size_t>((std::min)(
        kBlockSizeBytes, impl_->layout.virtual_size() - logical_offset));
    try {
        std::vector<std::byte> scratch(kBlockSizeBytes, std::byte{0});
        {
            std::scoped_lock lock(impl_->mutex);
            ++impl_->statistics.payload_read_calls;
        }
        auto read = impl_->file->read_exact(
            context, read_offset, std::span(scratch).first(logical_length));
        if (!read.ok()) {
            return translate_io_error<BlockLoadResult>(read.error(), operation);
        }
        auto live = check_operation_context(context);
        if (!live.ok()) return Result<BlockLoadResult>::failure(live.error());
        std::copy(scratch.begin(), scratch.end(), destination.begin());
        return Result<BlockLoadResult>::success({BlockLoadKind::Data});
    } catch (const std::bad_alloc&) {
        return reader_failure<BlockLoadResult>(
            L"allocating bounded payload scratch failed",
            PersistenceErrorCode::IoError, 0, operation);
    }
}

Result<std::optional<std::uint64_t>> VhdxReader::next_allocated_block(
    const OperationContext& context,
    const std::uint64_t first_block) const {
    constexpr auto operation = L"VhdxReader::next_allocated_block";
    if (!impl_) {
        return reader_failure<std::optional<std::uint64_t>>(
            L"VHDX reader is moved from",
            PersistenceErrorCode::FatalInternalInvariant, 0, operation);
    }
    auto integrity = impl_->check_integrity_latch(operation);
    if (!integrity.ok()) {
        return Result<std::optional<std::uint64_t>>::failure(
            integrity.error());
    }
    auto length = impl_->verify_length(context, operation);
    if (!length.ok()) {
        return Result<std::optional<std::uint64_t>>::failure(
            length.error());
    }
    if (impl_->layout.block_size() % kM2BlockSize != 0) {
        return reader_failure<std::optional<std::uint64_t>>(
            L"VHDX payload block is not aligned to the M2 block size",
            PersistenceErrorCode::BackingDataCorrupt, 0, operation);
    }
    const auto last_block =
        (impl_->layout.virtual_size() - 1) / kM2BlockSize;
    if (first_block > last_block) {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    const auto first_offset = first_block * kM2BlockSize;
    const auto first_payload =
        first_offset / impl_->layout.block_size();
    auto relevant_bitmaps = impl_->validate_relevant_bitmap_entries(
        context, first_payload, operation);
    if (!relevant_bitmaps.ok()) {
        return Result<std::optional<std::uint64_t>>::failure(
            relevant_bitmaps.error());
    }

    for (auto payload = first_payload;
         payload < impl_->layout.payload_block_count(); ++payload) {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                live.error());
        }
        auto bat_index = impl_->payload_bat_index(payload, operation);
        if (!bat_index.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                bat_index.error());
        }
        if (payload != 0 &&
            payload % impl_->layout.chunk_ratio() == 0) {
            auto bitmap = impl_->validate_bitmap_entry(
                context, bat_index.value() - 1, operation);
            if (!bitmap.ok()) {
                return Result<std::optional<std::uint64_t>>::failure(
                    bitmap.error());
            }
        }
        auto raw = impl_->read_bat_entry(
            context, bat_index.value(), operation);
        if (!raw.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                raw.error());
        }
        auto entry = impl_->decode_payload_entry(raw.value(), operation);
        if (!entry.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                entry.error());
        }
        auto observed = impl_->observe_payload_entry(
            payload, raw.value(), entry.value(), operation);
        if (!observed.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                observed.error());
        }
        live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                live.error());
        }
        if (entry.value().state != PayloadState::FullyPresent) continue;

        std::uint64_t payload_offset = 0;
        if (!checked_mul(payload, impl_->layout.block_size(),
                         payload_offset)) {
            return reader_failure<std::optional<std::uint64_t>>(
                L"VHDX payload virtual offset overflows",
                PersistenceErrorCode::BackingDataCorrupt, 0, operation);
        }
        const auto candidate_offset =
            (std::max)(first_offset, payload_offset);
        const auto candidate_block = candidate_offset / kM2BlockSize;
        if (candidate_block > last_block) break;
        return Result<std::optional<std::uint64_t>>::success(
            candidate_block);
    }
    return Result<std::optional<std::uint64_t>>::success(std::nullopt);
}

Result<std::vector<VhdxAllocatedExtent>>
VhdxReader::enumerate_allocated_extents(
    const OperationContext& context) const {
    constexpr auto operation = L"VhdxReader::enumerate_allocated_extents";
    if (!impl_) {
        return reader_failure<std::vector<VhdxAllocatedExtent>>(
            L"VHDX reader is moved from",
            PersistenceErrorCode::FatalInternalInvariant, 0, operation);
    }
    auto integrity = impl_->check_integrity_latch(operation);
    if (!integrity.ok()) {
        return Result<std::vector<VhdxAllocatedExtent>>::failure(
            integrity.error());
    }
    auto length = impl_->verify_length(context, operation);
    if (!length.ok()) {
        return Result<std::vector<VhdxAllocatedExtent>>::failure(
            length.error());
    }
    try {
        std::vector<VhdxAllocatedExtent> result;
        result.reserve(static_cast<std::size_t>((std::min)(
            impl_->options.max_extents,
            impl_->layout.payload_block_count())));
        std::map<std::uint64_t, std::uint64_t> physical_ranges;
        for (std::uint64_t payload = 0;
             payload < impl_->layout.payload_block_count(); ++payload) {
            auto live = check_operation_context(context);
            if (!live.ok()) {
                return Result<std::vector<VhdxAllocatedExtent>>::failure(
                    live.error());
            }
            auto bat_index = impl_->payload_bat_index(payload, operation);
            if (!bat_index.ok()) {
                return Result<std::vector<VhdxAllocatedExtent>>::failure(
                    bat_index.error());
            }
            if (payload != 0 &&
                payload % impl_->layout.chunk_ratio() == 0) {
                auto bitmap = impl_->validate_bitmap_entry(
                    context, bat_index.value() - 1, operation);
                if (!bitmap.ok()) {
                    return Result<std::vector<VhdxAllocatedExtent>>::failure(
                        bitmap.error());
                }
            }
            auto raw = impl_->read_bat_entry(
                context, bat_index.value(), operation);
            if (!raw.ok()) {
                return Result<std::vector<VhdxAllocatedExtent>>::failure(
                    raw.error());
            }
            auto entry = impl_->decode_payload_entry(raw.value(), operation);
            if (!entry.ok()) {
                return Result<std::vector<VhdxAllocatedExtent>>::failure(
                    entry.error());
            }
            auto observed = impl_->observe_payload_entry(
                payload, raw.value(), entry.value(), operation);
            if (!observed.ok()) {
                return Result<std::vector<VhdxAllocatedExtent>>::failure(
                    observed.error());
            }
            if (entry.value().state != PayloadState::FullyPresent) continue;
            if (result.size() >= impl_->options.max_extents) {
                return reader_failure<std::vector<VhdxAllocatedExtent>>(
                    L"allocated extent count exceeds caller budget",
                    PersistenceErrorCode::IoError, 0, operation);
            }
            std::uint64_t physical_end = 0;
            (void)checked_add(
                entry.value().file_offset,
                impl_->layout.block_size(), physical_end);
            const auto next = physical_ranges.lower_bound(
                entry.value().file_offset);
            if ((next != physical_ranges.end() &&
                 next->first < physical_end) ||
                (next != physical_ranges.begin() &&
                 std::prev(next)->second > entry.value().file_offset)) {
                return reader_failure<std::vector<VhdxAllocatedExtent>>(
                    L"allocated VHDX payload ranges overlap",
                    PersistenceErrorCode::BackingDataCorrupt, 0, operation);
            }
            physical_ranges.emplace(
                entry.value().file_offset, physical_end);
            std::uint64_t virtual_offset = 0;
            (void)checked_mul(
                payload, impl_->layout.block_size(), virtual_offset);
            result.push_back({
                virtual_offset,
                (std::min)(
                    static_cast<std::uint64_t>(impl_->layout.block_size()),
                    impl_->layout.virtual_size() - virtual_offset)});
        }
        return Result<std::vector<VhdxAllocatedExtent>>::success(
            std::move(result));
    } catch (const std::bad_alloc&) {
        return reader_failure<std::vector<VhdxAllocatedExtent>>(
            L"allocating bounded extent state failed",
            PersistenceErrorCode::IoError, 0, operation);
    }
}

VhdxReaderStats VhdxReader::stats() const {
    if (!impl_) return {};
    std::scoped_lock lock(impl_->mutex);
    return impl_->statistics;
}

const VhdxLayout& VhdxReader::layout() const noexcept {
    return impl_->layout;
}

WindowsFileIdentity VhdxReader::windows_identity() const noexcept {
    return impl_ ? impl_->windows_identity : WindowsFileIdentity{};
}

void VhdxReader::clear_cache() {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->cache.clear();
    impl_->lru.clear();
    ++impl_->cache_generation;
    impl_->statistics.current_cache_entries = 0;
    impl_->statistics.current_cache_bytes = 0;
}

}  // namespace myrd::persistence
