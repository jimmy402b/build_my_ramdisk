#include "lifecycle_runner.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <thread>

namespace {

std::string w2s(std::wstring_view ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
        static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(),
        static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
    return s;
}

} // anonymous namespace

namespace myrd::poc {

namespace {

std::string stage_name(LifecycleStage s) {
    switch (s) {
    case LifecycleStage::started: return "started";
    case LifecycleStage::preflight_passed: return "preflight_passed";
    case LifecycleStage::baseline_captured: return "baseline_captured";
    case LifecycleStage::provider_ready: return "provider_ready";
    case LifecycleStage::create_returned: return "create_returned";
    case LifecycleStage::device_confirmed: return "device_confirmed";
    case LifecycleStage::physical_drive_confirmed: return "physical_drive_confirmed";
    case LifecycleStage::round_trip_verified: return "round_trip_verified";
    case LifecycleStage::device_removed: return "device_removed";
    case LifecycleStage::provider_stopped: return "provider_stopped";
    case LifecycleStage::cleanup_verified: return "cleanup_verified";
    case LifecycleStage::report_written: return "report_written";
    }
    return "unknown";
}

} // anonymous namespace

PocReport run_lifecycle(
    IProviderLifecycle& provider,
    IKernelDiskAdapter& adapter,
    IDiskLocator& locator,
    IRoundTrip& round_trip,
    const RunConfig& config) {

    PocReport report;
    report.mode = "run";
    report.run_id = config.run_id;
    report.started_utc = "now"; // placeholder — caller sets real time
    report.last_stage = stage_name(LifecycleStage::started);

    std::optional<aim::DeviceId> owned_id;
    std::optional<ErrorEvidence> primary_error;
    std::vector<ErrorEvidence> cleanup_errors;
    LifecycleStage stage = LifecycleStage::started;

    auto set_error = [&](const Error& e) {
        if (!primary_error) {
            primary_error = ErrorEvidence{
                std::to_string(static_cast<int>(e.domain)),
                e.code,
                w2s(e.operation),
                w2s(e.message)};
        }
    };

    // Scope guard for cleanup
    auto cleanup = [&]() {
        // 1. If owned DeviceId exists, try remove
        if (owned_id) {
            report.cleanup.exact_remove_attempted = true;
            report.cleanup.removed_device_id = std::to_string(owned_id->value);
            auto rm = adapter.remove_device(*owned_id);
            if (rm.ok()) {
                report.cleanup.exact_remove_succeeded = true;
                adapter.wait_until_absent(*owned_id, std::chrono::seconds(15));
            } else {
                cleanup_errors.push_back(ErrorEvidence{
                    "cleanup", static_cast<std::uint32_t>(rm.error().code),
                    "remove_device", "cleanup remove failed"});
            }
        }

        // 2. Stop provider
        auto ps = provider.stop_and_release();
        if (ps.ok()) {
            report.cleanup.provider_stopped = true;
        } else {
            cleanup_errors.push_back(ErrorEvidence{
                "cleanup", 0, "stop_and_release", "provider stop failed"});
        }

        // 3. Check named objects released
        auto rel = provider.named_objects_released();
        report.cleanup.named_objects_released = rel.ok() && rel.value();

        stage = LifecycleStage::cleanup_verified;
        report.last_stage = stage_name(stage);
    };

    try {
        // Stage 1: Preflight
        stage = LifecycleStage::preflight_passed;

        // Stage 2: Baseline capture
        auto devices_before = adapter.list_devices();
        if (!devices_before) { set_error(devices_before.error()); cleanup(); goto done; }
        for (auto& d : devices_before.value())
            report.baseline.aim_device_ids_before.push_back(
                std::to_string(d.value));

        auto pd_before = locator.snapshot_numbers();
        if (!pd_before) { set_error(pd_before.error()); cleanup(); goto done; }
        for (auto n : pd_before.value())
            report.baseline.physical_drive_numbers_before.push_back(
                std::to_string(n));

        stage = LifecycleStage::baseline_captured;

        // Stage 3: Provider ready
        if (config.injected_failure == FailurePoint::none) {
            // Provider already started by caller
            stage = LifecycleStage::provider_ready;
        }

        // Debug failure injection points
        auto inject = [&](FailurePoint fp) {
            if (config.debug_build && config.injected_failure == fp) {
                set_error(Error{ErrorDomain::internal, 0,
                    L"injected_failure", L"debug failure injection"});
                cleanup();
                return true;
            }
            return false;
        };

        // Stage 4: CREATE
        aim::ProxyDiskSpec spec{aim::kM0DiskSize, aim::kM0SectorSize,
                                 provider.native_object_path()};
        auto created = adapter.create_proxy_disk(spec);
        if (!created) { set_error(created.error()); cleanup(); goto done; }
        owned_id = created.value();
        report.created_device = CreatedDeviceEvidence{};
        report.created_device->device_id_raw = std::to_string(created.value().value);
        auto addr = aim::decode_device_address(created.value());
        report.created_device->path_id = addr.path_id;
        report.created_device->target_id = addr.target_id;
        report.created_device->lun = addr.lun;
        stage = LifecycleStage::create_returned;

        if (inject(FailurePoint::create_returned)) goto done;

        // Stage 5: Device confirmed (separate QUERY_DEVICE)
        auto info = adapter.query_device(*owned_id);
        if (!info) { set_error(info.error()); cleanup(); goto done; }
        report.created_device->create_flags = info.value().flags;
        report.created_device->confirmed_by_query_device = true;
        stage = LifecycleStage::device_confirmed;

        if (inject(FailurePoint::device_confirmed)) goto done;

        // Stage 6: PhysicalDrive confirmed
        auto located = locator.wait_for(*owned_id, pd_before.value());
        if (!located) { set_error(located.error()); cleanup(); goto done; }
        report.created_device->physical_drive_number =
            static_cast<int>(located.value().physical_drive_number);
        report.created_device->physical_drive_path =
            w2s(located.value().path);
        report.created_device->disk_length = located.value().length_bytes;
        stage = LifecycleStage::physical_drive_confirmed;

        if (inject(FailurePoint::physical_drive_confirmed)) goto done;

        // Stage 7: Raw round trip
        auto rt = round_trip.run(located.value());
        if (!rt) { set_error(rt.error()); cleanup(); goto done; }
        report.raw_round_trip = rt.value();
        stage = LifecycleStage::round_trip_verified;

        if (inject(FailurePoint::round_trip_verified)) goto done;

        // Stage 8: Remove device
        {
            auto rm = adapter.remove_device(*owned_id);
            if (!rm) { set_error(rm.error()); cleanup(); goto done; }
            report.cleanup.exact_remove_attempted = true;
            report.cleanup.exact_remove_succeeded = true;
            report.cleanup.removed_device_id = std::to_string(owned_id->value);
            adapter.wait_until_absent(*owned_id, std::chrono::seconds(15));
            owned_id.reset();
            stage = LifecycleStage::device_removed;
        }

        // Stage 9: Stop provider
        provider.stop_and_release();
        report.cleanup.provider_stopped = true;
        stage = LifecycleStage::provider_stopped;

        // Stage 10: Check cleanup
        auto rel = provider.named_objects_released();
        report.cleanup.named_objects_released = rel.ok() && rel.value();

        // Post-baseline
        auto devices_after = adapter.list_devices();
        if (devices_after) {
            for (auto& d : devices_after.value())
                report.baseline.aim_device_ids_after.push_back(
                    std::to_string(d.value));
            report.cleanup.aim_device_lists_match =
                (devices_before.value().size() == devices_after.value().size());
        }
        auto pd_after = locator.snapshot_numbers();
        if (pd_after) {
            for (auto n : pd_after.value())
                report.baseline.physical_drive_numbers_after.push_back(
                    std::to_string(n));
            report.cleanup.physical_drive_lists_match =
                (pd_before.value().size() == pd_after.value().size());
        }
        stage = LifecycleStage::cleanup_verified;

        report.success = !primary_error.has_value();
        report.exit_code = report.success ? 0 : 7;

    } catch (...) {
        set_error(Error{ErrorDomain::internal, 0,
            L"run_lifecycle", L"unexpected exception"});
        cleanup();
    }

done:
    report.last_stage = stage_name(stage);
    report.cleanup_errors = std::move(cleanup_errors);
    if (primary_error) report.primary_error = *primary_error;
    report.finished_utc = "now";
    return report;
}

} // namespace myrd::poc
