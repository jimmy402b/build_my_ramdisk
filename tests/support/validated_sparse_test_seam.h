#pragma once

#if !defined(MYRD_ENABLE_TEST_SEAMS)
#error "validated sparse test seams require MYRD_ENABLE_TEST_SEAMS"
#endif

#include "src/persistence/image_paths.h"
#include "src/persistence/native_sparse_backend.h"
#include "src/persistence/writer_lock.h"

#include <cstddef>

namespace myrd::persistence::test {

[[nodiscard]] Result<void> mark_validated_bundle_file_sparse_with_backend(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    detail::INativeSparseBackend& backend);

[[nodiscard]] std::size_t release_quarantined_native_sparse_for_test();

}  // namespace myrd::persistence::test
