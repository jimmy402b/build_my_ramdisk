#pragma once

#include "m2_report.h"
#include "src/common/operation_context.h"

#include <cstdint>
#include <functional>

namespace myrd {

// The callback is a resource-ownership handoff. A workload must invoke it
// immediately after its first mount/volume mutation and before any later
// fallible operation. This lets the outer lifecycle register rollback even
// when setup_and_write() subsequently fails.
using M2MountsAcquired = std::function<void()>;

class IM2VolumeWorkload {
public:
    virtual ~IM2VolumeWorkload() = default;

    virtual Result<M2IoEvidence> setup_and_write(
        const OperationContext& context,
        M2DeviceIdentity& identity,
        const M2MountsAcquired& mounts_acquired) = 0;

    virtual Result<std::uint64_t> delete_and_retrim(
        const OperationContext& context,
        M2DeviceIdentity& identity) = 0;

    virtual Result<M2PerformanceEvidence> run_performance(
        const OperationContext& context,
        const M2DeviceIdentity& identity) = 0;

    virtual Result<void> remove_mounts(
        const OperationContext& context,
        const M2DeviceIdentity& identity) = 0;
};

} // namespace myrd
