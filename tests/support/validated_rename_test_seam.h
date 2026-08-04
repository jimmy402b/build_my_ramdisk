#pragma once

#include "src/persistence/image_paths.h"
#include "src/persistence/native_rename_backend.h"
#include "src/persistence/writer_lock.h"

#include <cstddef>
#include <string_view>

namespace myrd::persistence::test {

using BeforeNativeRenameHook = void (*)(void* context);
using AfterWriterStateSnapshotHook = void (*)(void* context);

[[nodiscard]] Result<void> verify_bundle_writer_lock_with_snapshot_hook(
    BundleWriterLock& writer,
    AfterWriterStateSnapshotHook hook,
    void* hook_context);

[[nodiscard]] Result<void> rename_validated_bundle_file_with_hook(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::string_view new_relative_name,
    BeforeNativeRenameHook hook,
    void* hook_context);

[[nodiscard]] Result<void> rename_validated_bundle_file_with_backend(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::string_view new_relative_name,
    detail::INativeRenameBackend& backend);

[[nodiscard]] std::size_t native_rename_slots_in_use() noexcept;
[[nodiscard]] std::size_t maximum_native_rename_slots() noexcept;
[[nodiscard]] bool quarantine_empty_native_rename_for_test();
[[nodiscard]] std::size_t release_quarantined_native_renames_for_test();

} // namespace myrd::persistence::test
