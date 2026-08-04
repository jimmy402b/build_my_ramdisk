#pragma once

#include "src/persistence/image_paths.h"
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

} // namespace myrd::persistence::test
