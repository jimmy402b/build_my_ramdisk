#include "src/persistence/journal_reader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

template <typename T>
Result<T> reader_failure(
    std::wstring_view message,
    const PersistenceErrorCode code =
        PersistenceErrorCode::BackingDataCorrupt,
    const std::uint32_t native_code = 0,
    const std::wstring_view operation = L"JournalReader::read_block") {
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

PersistenceErrorCode io_error_code(const Error& error) noexcept {
    return error.native_code == ERROR_ACCESS_DENIED
        ? PersistenceErrorCode::AccessDenied
        : PersistenceErrorCode::IoError;
}

std::uint64_t expected_record_length(
    const JournalRecordType type) noexcept {
    return type == JournalRecordType::Data
        ? kDataRecordBytes : kFixedRecordBytes;
}

}  // namespace

struct JournalReader::Impl final {
    Impl(JournalReaderOptions source_options,
         std::unique_ptr<JournalIndexReader> source_index)
        : options(std::move(source_options)),
          index(std::move(source_index)) {}

    JournalReaderOptions options{};
    std::unique_ptr<JournalIndexReader> index{};
};

JournalReader::JournalReader(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
JournalReader::~JournalReader() = default;
JournalReader::JournalReader(JournalReader&&) noexcept = default;
JournalReader& JournalReader::operator=(JournalReader&&) noexcept = default;

Result<std::unique_ptr<JournalReader>> JournalReader::create(
    JournalReaderOptions options) {
    if (validate_geometry(options.geometry) != GeometryStatus::Ok ||
        !options.view.file) {
        return reader_failure<std::unique_ptr<JournalReader>>(
            L"reader options are invalid",
            PersistenceErrorCode::InvalidFormat, 0,
            L"JournalReader::create");
    }
    try {
        auto index = JournalIndexReader::create(options.index_cache);
        if (!index.ok()) {
            return Result<std::unique_ptr<JournalReader>>::failure(
                index.error());
        }
        auto impl = std::make_unique<Impl>(
            std::move(options), index.move_value());
        return Result<std::unique_ptr<JournalReader>>::success(
            std::unique_ptr<JournalReader>(
                new JournalReader(std::move(impl))));
    } catch (const std::bad_alloc&) {
        return reader_failure<std::unique_ptr<JournalReader>>(
            L"allocating journal reader failed",
            PersistenceErrorCode::IoError, 0,
            L"JournalReader::create");
    }
}

Result<std::optional<BlockLoadResult>> JournalReader::read_block(
    const OperationContext& context,
    const std::uint64_t block_index,
    const std::span<std::byte> destination) const {
    if (!impl_) {
        return reader_failure<std::optional<BlockLoadResult>>(
            L"reader is moved from",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::optional<BlockLoadResult>>::failure(
            live.error());
    }
    auto logical = journal_block_logical_length(
        impl_->options.geometry, block_index);
    if (!logical.ok()) {
        return reader_failure<std::optional<BlockLoadResult>>(
            L"block index is outside image geometry",
            PersistenceErrorCode::InvalidFormat);
    }
    if (destination.size() != kM2BlockSize) {
        return reader_failure<std::optional<BlockLoadResult>>(
            L"destination must be exactly one M2 block",
            PersistenceErrorCode::InvalidFormat);
    }

    auto lookup = impl_->index->lookup(
        context, impl_->options.view, block_index);
    if (!lookup.ok()) {
        if (context_error(lookup.error()) ||
            lookup.error().domain == ErrorDomain::persistence ||
            lookup.error().domain == ErrorDomain::internal) {
            return Result<std::optional<BlockLoadResult>>::failure(
                lookup.error());
        }
        if (lookup.error().native_code == ERROR_HANDLE_EOF) {
            return reader_failure<std::optional<BlockLoadResult>>(
                L"committed journal index cannot be read",
                PersistenceErrorCode::JournalCorrupt,
                lookup.error().native_code);
        }
        return reader_failure<std::optional<BlockLoadResult>>(
            L"journal index I/O failed",
            io_error_code(lookup.error()),
            lookup.error().native_code);
    }
    if (lookup.value().kind == JournalIndexLookupKind::Absent) {
        return Result<std::optional<BlockLoadResult>>::success(
            std::nullopt);
    }

    const auto& descriptor = lookup.value().descriptor;
    const auto expected_length = expected_record_length(
        descriptor.record_type);
    if (descriptor.block_index != block_index ||
        descriptor.record_length != expected_length ||
        descriptor.record_offset > impl_->options.view.committed_length ||
        descriptor.record_length >
            impl_->options.view.committed_length - descriptor.record_offset) {
        return reader_failure<std::optional<BlockLoadResult>>(
            L"journal descriptor is outside the committed view");
    }

    std::vector<std::byte> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(descriptor.record_length));
    } catch (const std::bad_alloc&) {
        return reader_failure<std::optional<BlockLoadResult>>(
            L"allocating bounded record buffer failed",
            PersistenceErrorCode::IoError);
    }
    auto read = impl_->options.view.file->read_exact(
        context, descriptor.record_offset, bytes);
    if (!read.ok()) {
        if (context_error(read.error())) {
            return Result<std::optional<BlockLoadResult>>::failure(
                read.error());
        }
        if (read.error().domain == ErrorDomain::internal ||
            read.error().domain == ErrorDomain::persistence) {
            return Result<std::optional<BlockLoadResult>>::failure(
                read.error());
        }
        if (read.error().native_code != ERROR_HANDLE_EOF) {
            return reader_failure<std::optional<BlockLoadResult>>(
                L"journal record I/O failed",
                io_error_code(read.error()),
                read.error().native_code);
        }
        return reader_failure<std::optional<BlockLoadResult>>(
            L"referenced journal record cannot be read",
            PersistenceErrorCode::BackingDataCorrupt,
            read.error().native_code);
    }
    live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::optional<BlockLoadResult>>::failure(
            live.error());
    }
    auto decoded = decode_block_record(
        bytes, descriptor.record_offset,
        impl_->options.view.committed_length,
        impl_->options.geometry);
    if (!decoded.ok()) {
        return reader_failure<std::optional<BlockLoadResult>>(
            L"referenced journal record is corrupt",
            PersistenceErrorCode::BackingDataCorrupt,
            decoded.error().native_code);
    }
    live = check_operation_context(context);
    if (!live.ok()) {
        return Result<std::optional<BlockLoadResult>>::failure(
            live.error());
    }
    if (decoded.value().metadata.block_index != block_index ||
        decoded.value().metadata.common.type != descriptor.record_type ||
        decoded.value().metadata.common.checkpoint_generation !=
            descriptor.record_generation ||
        decoded.value().payload_crc32c != descriptor.payload_crc32c) {
        return reader_failure<std::optional<BlockLoadResult>>(
            L"journal record does not match its index descriptor");
    }

    if (descriptor.record_type == JournalRecordType::Data) {
        if (decoded.value().payload.size() != destination.size()) {
            return reader_failure<std::optional<BlockLoadResult>>(
                L"DATA payload does not fill one M2 block");
        }
        std::copy(decoded.value().payload.begin(),
                  decoded.value().payload.end(), destination.begin());
        return Result<std::optional<BlockLoadResult>>::success(
            BlockLoadResult{BlockLoadKind::Data});
    }
    return Result<std::optional<BlockLoadResult>>::success(
        BlockLoadResult{BlockLoadKind::AllZero});
}

}  // namespace myrd::persistence
