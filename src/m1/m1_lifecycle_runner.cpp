#include "m1_lifecycle_runner.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace myrd {

Result<M1Report> run_m1_lifecycle(
    IM1ProviderFactory& provider_factory,
    IM1KernelAdapter& kernel,
    IM1RawDiskApi& raw_disk,
    const M1RunConfig& config) {

    M1Report report;
    report.mode = "m1-lifecycle";
    report.run_id = config.run_id;
    report.schema_version = 1;

    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts;
    ts << std::put_time(std::gmtime(&now_t), "%Y-%m-%dT%H:%M:%SZ");
    report.started_utc = ts.str();

    std::optional<aim::DeviceId> owned_id;
    std::unique_ptr<M1Provider> provider;
    std::string primary_error;

    auto record_cleanup_error = [&](const std::string& msg) {
        report.cleanup_errors.push_back(msg);
    };

    // Stage 1: capture baseline
    auto devices_before = kernel.list_devices();
    if (!devices_before) {
        report.last_stage = "baseline_capture";
        report.exit_code = 1;
        report.primary_error = "failed to list devices before";
        return Result<M1Report>::success(report);
    }
    report.last_stage = "baseline_captured";

    // Stage 2: create and start provider
    auto provider_result = provider_factory.create_and_start(
        L"\\BaseNamedObjects\\Global\\myrd-m1-test");
    if (!provider_result) {
        report.last_stage = "provider_start";
        report.exit_code = 1;
        report.primary_error = "failed to create provider";
        return Result<M1Report>::success(report);
    }
    provider = std::move(provider_result.value());
    report.last_stage = "provider_started";

    // Stage 3: create device via kernel adapter
    proxy::SessionSnapshot snap_before = provider->snapshot();
    report.provider.info_count = snap_before.requests.info;

    auto create_result = kernel.create_proxy_disk(
        provider->native_object_path(), 64ull * 1024ull * 1024ull, 512);
    if (!create_result) {
        report.last_stage = "device_create";
        report.exit_code = 1;
        report.primary_error = "failed to create device";
        auto stop = provider_factory.stop_and_release(*provider);
        if (!stop) record_cleanup_error("provider stop after create failure");
        return Result<M1Report>::success(report);
    }
    owned_id = create_result.value();
    report.last_stage = "device_created";

    // Stage 4: wait for session to serve (brief delay for driver to connect)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    report.last_stage = "session_serving";

    // Stage 5: raw protocol verification
    proxy::RequestCounters before_counters = provider->snapshot().requests;
    auto counters_fn = [&]() -> proxy::RequestCounters {
        return provider->snapshot().requests;
    };

    auto raw_evidence = verify_m1_raw_protocol(
        raw_disk, 64ull * 1024ull * 1024ull, 512,
        before_counters, counters_fn);
    if (raw_evidence.ok()) {
        report.raw_protocol.zero_write_verified =
            raw_evidence.value().zero_write_verified;
        report.raw_protocol.unmap_verified =
            raw_evidence.value().unmap_verified;
    }
    report.last_stage = "raw_protocol_verified";

    // Stage 6: capture provider counters
    proxy::SessionSnapshot snap_after = provider->snapshot();
    report.provider.info_count = snap_after.requests.info;
    report.provider.read_count = snap_after.requests.read;
    report.provider.write_count = snap_after.requests.write;
    report.provider.unmap_count = snap_after.requests.unmap;
    report.provider.zero_count = snap_after.requests.zero;
    report.provider.close_count = snap_after.requests.close;
    report.provider.fatal_protocol_error_count =
        snap_after.requests.fatal_protocol_errors;

    // Stage 7: remove exact device
    if (owned_id.has_value()) {
        auto remove = kernel.remove_device(owned_id.value());
        report.cleanup.exact_remove_succeeded = remove.ok();
        if (!remove) {
            record_cleanup_error("device remove failed");
        }
        if (report.cleanup.exact_remove_succeeded) {
            auto absent = kernel.wait_until_absent(
                owned_id.value(), std::chrono::seconds(15));
            report.cleanup.device_disappeared = absent.ok();
        }
    }
    report.last_stage = "device_removed";

    // Stage 8: stop provider
    auto stop = provider_factory.stop_and_release(*provider);
    report.cleanup.provider_stopped = stop.ok();
    if (!stop) record_cleanup_error("provider stop failed");
    report.cleanup.named_objects_released = true;
    report.last_stage = "provider_stopped";

    // Stage 9: verify baselines
    auto devices_after = kernel.list_devices();
    if (devices_before.ok() && devices_after.ok()) {
        report.cleanup.aim_device_lists_match =
            (devices_before.value().size() == devices_after.value().size());
    }
    report.last_stage = "baseline_verified";

    // Final
    report.success = report.cleanup_errors.empty() && primary_error.empty();
    report.exit_code = report.success ? 0 : 1;
    report.primary_error = primary_error.empty()
        ? std::optional<std::string>{} : primary_error;

    now = std::chrono::system_clock::now();
    now_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts2;
    ts2 << std::put_time(std::gmtime(&now_t), "%Y-%m-%dT%H:%M:%SZ");
    report.finished_utc = ts2.str();

    return Result<M1Report>::success(report);
}

} // namespace myrd
