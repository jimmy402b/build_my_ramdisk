#include "lifecycle_runner.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
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

std::string session_id_from_native_path(std::wstring_view path) {
    constexpr std::wstring_view prefix =
        L"\\BaseNamedObjects\\Global\\myrd-m0-";
    if (!path.starts_with(prefix)) return {};
    const auto id = path.substr(prefix.size());
    if (id.size() != 32) return {};
    for (const auto ch : id) {
        if (!((ch >= L'0' && ch <= L'9') ||
              (ch >= L'a' && ch <= L'f'))) {
            return {};
        }
    }
    return w2s(id);
}

bool same_device_id_set(std::vector<myrd::aim::DeviceId> left,
                        std::vector<myrd::aim::DeviceId> right) {
    const auto less = [](const auto a, const auto b) {
        return a.value < b.value;
    };
    std::sort(left.begin(), left.end(), less);
    std::sort(right.begin(), right.end(), less);
    return left == right;
}

bool same_physical_drive_set(std::vector<std::uint32_t> left,
                             std::vector<std::uint32_t> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
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
    report.started_utc = config.started_utc;
    report.last_stage = stage_name(LifecycleStage::started);

    std::optional<aim::DeviceId> owned_id;
    std::optional<std::vector<aim::DeviceId>> devices_before_snapshot;
    std::optional<std::vector<std::uint32_t>>
        physical_drives_before_snapshot;
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

    auto capture_post_baseline = [&]() {
        if (devices_before_snapshot) {
            auto devices_after = adapter.list_devices();
            if (devices_after) {
                report.baseline.aim_device_ids_after.clear();
                for (const auto& device : devices_after.value()) {
                    report.baseline.aim_device_ids_after.push_back(
                        std::to_string(device.value));
                }
                report.cleanup.aim_device_lists_match =
                    same_device_id_set(
                        devices_after.value(), *devices_before_snapshot);
            } else {
                cleanup_errors.push_back(ErrorEvidence{
                    "cleanup",
                    static_cast<std::uint32_t>(devices_after.error().code),
                    "list_devices(after)",
                    "failed to capture post-cleanup AIM device list"});
            }
        }

        if (physical_drives_before_snapshot) {
            constexpr auto kSnapshotTimeout = std::chrono::seconds(15);
            constexpr auto kSnapshotInterval =
                std::chrono::milliseconds(250);
            const auto deadline =
                std::chrono::steady_clock::now() + kSnapshotTimeout;
            std::uint32_t last_error_code = WAIT_TIMEOUT;
            bool observed_complete_snapshot = false;

            do {
                auto physical_after = locator.snapshot_numbers();
                if (physical_after) {
                    observed_complete_snapshot = true;
                    report.baseline.physical_drive_numbers_after.clear();
                    for (const auto number : physical_after.value()) {
                        report.baseline.physical_drive_numbers_after.push_back(
                            std::to_string(number));
                    }
                    report.cleanup.physical_drive_lists_match =
                        same_physical_drive_set(
                            physical_after.value(),
                            *physical_drives_before_snapshot);
                    if (report.cleanup.physical_drive_lists_match) break;
                    last_error_code = WAIT_TIMEOUT;
                } else {
                    last_error_code =
                        static_cast<std::uint32_t>(
                            physical_after.error().code);
                }

                if (std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::sleep_for(kSnapshotInterval);
            } while (true);

            if (!report.cleanup.physical_drive_lists_match) {
                cleanup_errors.push_back(ErrorEvidence{
                    "cleanup",
                    last_error_code,
                    "wait_for_physical_drive_baseline",
                    observed_complete_snapshot
                        ? "PhysicalDrive list did not return to baseline before timeout"
                        : "could not capture a complete post-cleanup PhysicalDrive list before timeout"});
            }
        }
    };

    auto wait_for_named_objects_released = [&]() {
        constexpr auto kReleaseTimeout = std::chrono::seconds(15);
        constexpr auto kReleaseInterval = std::chrono::milliseconds(100);
        const auto deadline =
            std::chrono::steady_clock::now() + kReleaseTimeout;

        do {
            auto released = provider.named_objects_released();
            if (!released) {
                cleanup_errors.push_back(ErrorEvidence{
                    "cleanup",
                    static_cast<std::uint32_t>(released.error().code),
                    w2s(released.error().operation),
                    w2s(released.error().message)});
                return;
            }
            if (released.value()) {
                report.cleanup.named_objects_released = true;
                return;
            }

            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(kReleaseInterval);
        } while (true);

        cleanup_errors.push_back(ErrorEvidence{
            "cleanup",
            WAIT_TIMEOUT,
            "wait_for_named_objects_released",
            "provider named objects remained open after timeout"});
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
                auto absent = adapter.wait_until_absent(
                    *owned_id, std::chrono::seconds(15));
                if (absent.ok()) {
                    report.cleanup.device_disappeared = true;
                } else {
                    cleanup_errors.push_back(ErrorEvidence{
                        "cleanup",
                        static_cast<std::uint32_t>(absent.error().code),
                        "wait_until_absent",
                        "created device did not disappear"});
                }
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

        // 3. Wait for the driver to release its final object references.
        wait_for_named_objects_released();

        capture_post_baseline();

        stage = LifecycleStage::cleanup_verified;
        report.last_stage = stage_name(stage);
    };

    try {
        // Stage 1: Preflight
        stage = LifecycleStage::preflight_passed;

        // Stage 2: Baseline capture
        auto devices_before = adapter.list_devices();
        if (!devices_before) { set_error(devices_before.error()); cleanup(); goto done; }
        devices_before_snapshot = devices_before.value();
        for (auto& d : devices_before.value())
            report.baseline.aim_device_ids_before.push_back(
                std::to_string(d.value));

        auto pd_before = locator.snapshot_numbers();
        if (!pd_before) { set_error(pd_before.error()); cleanup(); goto done; }
        physical_drives_before_snapshot = pd_before.value();
        for (auto n : pd_before.value())
            report.baseline.physical_drive_numbers_before.push_back(
                std::to_string(n));

        stage = LifecycleStage::baseline_captured;

        // Stage 3: Start provider
        {
            auto start_result = provider.start();
            if (!start_result) {
                set_error(start_result.error());
                cleanup();
                goto done;
            }
            const auto native_path = provider.native_object_path();
            report.provider.native_object_path = w2s(native_path);
            report.provider.session_id =
                session_id_from_native_path(native_path);
            report.provider.mapping_size = proxy::kMappingSize;
            report.provider.data_area_size = proxy::kDataAreaSize;
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

        auto rescanned = adapter.rescan_adapter();
        if (!rescanned) { set_error(rescanned.error()); cleanup(); goto done; }

        // Stage 5: Device confirmed (separate QUERY_DEVICE)
        auto info = adapter.query_device(*owned_id);
        if (!info) { set_error(info.error()); cleanup(); goto done; }
        if (info.value().size_bytes != spec.size_bytes ||
            info.value().sector_size != spec.sector_size ||
            info.value().native_object_path != spec.native_object_path ||
            info.value().flags != aim::kM0QueryFlags) {
            set_error(Error{
                ErrorDomain::evidence, 0,
                L"query_device(identity)",
                L"QUERY_DEVICE evidence does not match the created proxy disk"});
            cleanup();
            goto done;
        }
        report.created_device->create_flags = info.value().flags;
        report.created_device->disk_length = info.value().size_bytes;
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
            auto absent = adapter.wait_until_absent(
                *owned_id, std::chrono::seconds(15));
            if (!absent) {
                set_error(absent.error());
                cleanup();
                goto done;
            }
            report.cleanup.device_disappeared = true;
            owned_id.reset();
            stage = LifecycleStage::device_removed;
        }

        // Stage 9: Stop provider
        auto provider_stop = provider.stop_and_release();
        if (provider_stop) {
            report.cleanup.provider_stopped = true;
        } else {
            cleanup_errors.push_back(ErrorEvidence{
                "cleanup",
                static_cast<std::uint32_t>(provider_stop.error().code),
                "stop_and_release",
                "provider stop failed"});
        }
        stage = LifecycleStage::provider_stopped;

        // Stage 10: Check cleanup
        wait_for_named_objects_released();

        capture_post_baseline();
        stage = LifecycleStage::cleanup_verified;

        report.success = !primary_error.has_value() && cleanup_errors.empty() &&
            report.cleanup.device_disappeared &&
            report.cleanup.provider_stopped &&
            report.cleanup.named_objects_released &&
            report.cleanup.aim_device_lists_match &&
            report.cleanup.physical_drive_lists_match;
        report.exit_code = report.success ? 0 : 7;

    } catch (...) {
        set_error(Error{ErrorDomain::internal, 0,
            L"run_lifecycle", L"unexpected exception"});
        cleanup();
    }

done:
    {
        const auto counters = provider.counters();
        report.provider.info_count = counters.info;
        report.provider.read_count = counters.read;
        report.provider.write_count = counters.write;
        report.provider.close_count = counters.close;
        report.provider.fatal_protocol_error_count =
            counters.fatal_protocol_errors;
    }
    report.last_stage = stage_name(stage);
    report.cleanup_errors = std::move(cleanup_errors);
    if (primary_error) report.primary_error = *primary_error;
    return report;
}

} // namespace myrd::poc
