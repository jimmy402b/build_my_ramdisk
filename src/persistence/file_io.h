#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace myrd::persistence {

enum class FileOpenMode : std::uint8_t {
    ReadOnlyExisting,
    ReadWriteExisting,
    CreateNewReadWrite,
};

// Platform-sized, positioned transfers. Implementations may complete fewer
// bytes than requested. Exactness is owned by IPersistenceFile's production
// wrapper, not by test doubles or future codecs.
class IFileIoBackend {
public:
    virtual ~IFileIoBackend() = default;

    virtual Result<std::size_t> read_some(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<std::byte> destination) = 0;
    virtual Result<std::size_t> write_some(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<const std::byte> source) = 0;
    virtual Result<void> flush(const OperationContext& context) = 0;
    virtual Result<std::uint64_t> length(
        const OperationContext& context) const = 0;
    virtual Result<void> truncate(
        const OperationContext& context,
        std::uint64_t new_length) = 0;
    virtual Result<void> close() = 0;
};

class IPersistenceFile {
public:
    virtual ~IPersistenceFile() = default;

    virtual Result<void> read_exact(
        std::uint64_t offset,
        std::span<std::byte> destination) = 0;
    virtual Result<void> write_exact(
        std::uint64_t offset,
        std::span<const std::byte> source) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<std::uint64_t> length() const = 0;
    virtual Result<void> truncate(std::uint64_t new_length) = 0;

    // Context-aware overloads let long exact transfers stop between partial
    // completions. The no-context methods above preserve the frozen Task 11
    // narrow interface and use an unlimited live context.
    virtual Result<void> read_exact(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<std::byte> destination) = 0;
    virtual Result<void> write_exact(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<const std::byte> source) = 0;
    virtual Result<void> flush(const OperationContext& context) = 0;
    virtual Result<std::uint64_t> length(
        const OperationContext& context) const = 0;
    virtual Result<void> truncate(
        const OperationContext& context,
        std::uint64_t new_length) = 0;

    // Explicit close is required when a caller must observe close failure
    // before rename/publication. Destruction still performs best-effort close.
    virtual Result<void> close() = 0;
};

[[nodiscard]] std::unique_ptr<IPersistenceFile>
make_exact_persistence_file(std::unique_ptr<IFileIoBackend> backend);

[[nodiscard]] Result<std::unique_ptr<IPersistenceFile>>
open_windows_persistence_file(
    const std::filesystem::path& path,
    FileOpenMode mode);

} // namespace myrd::persistence
