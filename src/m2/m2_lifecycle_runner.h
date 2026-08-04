#pragma once

#include "m2_report.h"
#include "m2_volume_workload.h"
#include "src/common/operation_context.h"
#include "src/driver/aim_protocol.h"

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>

namespace myrd {

struct M2BaselineSnapshot {
    M2DriverServiceState driver_service;
    M2InventorySnapshot inventory;
    std::string fingerprint;
};

struct M2BaselineComparisonEvidence {
    M2DriverServiceState driver_service_after;
    M2FinalBaselineComparison comparison;
};

struct M2ProviderSample {
    DynamicBlockStoreStats store_stats;
    std::uint64_t provider_process_id{};
    std::uint64_t working_set_bytes{};
    std::uint64_t private_bytes{};
    std::uint64_t commit_bytes{};
};

struct M2OwnedDevice {
    aim::DeviceId id{};
    M2DeviceIdentity identity;
};

class IM2BaselineControl {
public:
    virtual ~IM2BaselineControl() = default;
    virtual Result<M2BaselineSnapshot> capture(
        const OperationContext& context) = 0;
    virtual Result<void> restore_service(
        const OperationContext& context,
        const M2BaselineSnapshot& baseline) = 0;
    virtual Result<M2BaselineComparisonEvidence> compare(
        const OperationContext& context,
        const M2BaselineSnapshot& baseline) = 0;
};

class IM2LifecycleProvider {
public:
    virtual ~IM2LifecycleProvider() = default;
    virtual Result<void> start(const OperationContext& context) = 0;
    virtual Result<void> stop(const OperationContext& context) = 0;
    virtual Result<M2ProviderSample> sample(
        const OperationContext& context,
        M2StoreSamplePoint point) const = 0;
    virtual std::string session_id() const = 0;
    virtual std::wstring native_object_path() const = 0;
};

class IM2LifecycleProviderFactory {
public:
    virtual ~IM2LifecycleProviderFactory() = default;

    // Failure transfers no provider ownership. Success must return a
    // non-null provider; the runner owns and stops it even if the phase
    // deadline expires immediately after this call returns.
    virtual Result<std::unique_ptr<IM2LifecycleProvider>> create(
        const OperationContext& context,
        const M2GeometryOptions& geometry,
        const M2BackingState& backing) = 0;
};

class IM2DeviceControl {
public:
    virtual ~IM2DeviceControl() = default;

    // Failure creates no device. Success must return the exact, removable
    // device id. AIM address 0 (Path 0, Target 0, LUN 0) is valid; only the
    // wildcard/auto sentinel is forbidden.
    virtual Result<M2OwnedDevice> create(
        const OperationContext& context,
        std::wstring_view native_object_path,
        const M2GeometryOptions& geometry) = 0;
    virtual Result<M2DeviceIdentity> wait_for_disk(
        const OperationContext& context,
        const M2OwnedDevice& owned) = 0;
    virtual Result<void> remove(
        const OperationContext& context,
        aim::DeviceId id) = 0;
};

class IM2ReportPublisher {
public:
    virtual ~IM2ReportPublisher() = default;

    // The runner calls this only with a semantically valid report. A
    // successful return is the publication commit point. Implementations
    // must validate cancellation/deadline immediately before their atomic
    // replace/rename and must return failure if that commit may not occur.
    // After success, the lifecycle runner will not mutate report state.
    virtual Result<void> publish(
        const OperationContext& context,
        const M2Report& report) = 0;
};

class IM2LifecycleClock {
public:
    virtual ~IM2LifecycleClock() = default;
    virtual std::chrono::steady_clock::time_point steady_now() = 0;
    virtual std::string utc_now() = 0;
};

class M2SystemLifecycleClock final : public IM2LifecycleClock {
public:
    std::chrono::steady_clock::time_point steady_now() override;
    std::string utc_now() override;
};

struct M2LifecycleConfig {
    M2Report report;
    std::chrono::milliseconds phase_timeout{30000};
    std::stop_token stop_token{};
    std::optional<M2PhaseId> injected_failure_after;
};

M2Report run_m2_lifecycle(
    IM2BaselineControl& baseline,
    IM2LifecycleProviderFactory& providers,
    IM2DeviceControl& devices,
    IM2VolumeWorkload& workload,
    IM2ReportPublisher& publisher,
    IM2LifecycleClock& clock,
    M2LifecycleConfig config);

} // namespace myrd
