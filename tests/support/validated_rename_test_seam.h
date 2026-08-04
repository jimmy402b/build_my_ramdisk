#pragma once

#include "src/persistence/image_paths.h"
#include "src/persistence/native_rename_backend.h"
#include "src/persistence/writer_lock.h"

#include <string_view>

namespace myrd::persistence::test {

using BeforeNativeRenameHook = void (*)(void* context);

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

} // namespace myrd::persistence::test
