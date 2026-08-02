#include "m1_lifecycle_runner.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>
#include <thread>

namespace myrd {

namespace {

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::string error_domain(ErrorDomain domain) {
    switch (domain) {
    case ErrorDomain::usage: return "usage";
    case ErrorDomain::preflight: return "preflight";
    case ErrorDomain::protocol: return "protocol";
    case ErrorDomain::win32: return "win32";
    case ErrorDomain::ntstatus: return "ntstatus";
    case ErrorDomain::storage: return "storage";
    case ErrorDomain::cleanup: return "cleanup";
    case ErrorDomain::evidence: return "evidence";
    case ErrorDomain::internal: return "internal";
    }
    return "unknown";
}

M1ErrorEvidence evidence(const Error& error) {
    return {error_domain(error.domain), error.code, error.native_code,
            narrow(error.operation), narrow(error.message)};
}

bool same_ids(std::vector<aim::DeviceId> left,
              std::vector<aim::DeviceId> right) {
    const auto less = [](aim::DeviceId a, aim::DeviceId b) {
        return a.value < b.value;
    };
    std::sort(left.begin(), left.end(), less);
    std::sort(right.begin(), right.end(), less);
    return left == right;
}

bool same_numbers(std::vector<std::uint32_t> left,
                  std::vector<std::uint32_t> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

std::string state_name(proxy::SessionState state) {
    switch (state) {
    case proxy::SessionState::constructed: return "constructed";
    case proxy::SessionState::objects_created: return "objects_created";
    case proxy::SessionState::ready: return "ready";
    case proxy::SessionState::serving: return "serving";
    case proxy::SessionState::closing: return "closing";
    case proxy::SessionState::closed: return "closed";
    case proxy::SessionState::failed: return "failed";
    }
    return "unknown";
}

void capture_provider(M1Report& report,
                      const proxy::SessionSnapshot& snapshot,
                      std::wstring_view native_path) {
    const auto& counters = snapshot.requests;
    auto& target = report.provider;
    target.info_count = counters.info;
    target.read_count = counters.read;
    target.write_count = counters.write;
    target.unmap_count = counters.unmap;
    target.zero_count = counters.zero;
    target.close_count = counters.close;
    target.info_error_count = counters.info_errors;
    target.read_error_count = counters.read_errors;
    target.write_error_count = counters.write_errors;
    target.unmap_error_count = counters.unmap_errors;
    target.zero_error_count = counters.zero_errors;
    target.fatal_protocol_error_count = counters.fatal_protocol_errors;
    target.cancelled = counters.cancelled;
    target.timed_out = counters.timed_out;
    target.native_object_path = narrow(native_path);
    target.final_state = state_name(snapshot.state);
    if (snapshot.last_error) target.last_error = evidence(*snapshot.last_error);
    target.diagnostics.clear();
    for (const auto& diagnostic : snapshot.recent_diagnostics) {
        target.diagnostics.push_back({
            diagnostic.sequence,
            diagnostic.request.request_code,
            diagnostic.request.offset,
            diagnostic.request.length,
            narrow(diagnostic.error.operation),
            evidence(diagnostic.error)});
    }
}

Error injected(M1FailurePoint point) {
    return {ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"injected_failure",
            L"debug lifecycle failure injected after stage " +
                std::to_wstring(static_cast<unsigned>(point))};
}

} // namespace

M1Report run_m1_lifecycle(
    IM1ExecutionGuard& execution_guard,
    IM1ProviderFactory& provider_factory,
    IM1KernelAdapter& kernel,
    IM1DiskLocator& locator,
    IM1ProtocolVerifier& protocol,
    const M1RunConfig& config) {
    M1Report report;
    report.mode = "run";
    report.run_id = config.run_id;
    report.started_utc = config.started_utc;

    std::unique_ptr<IM1ProviderLifecycle> provider;
    std::optional<std::vector<aim::DeviceId>> aim_before;
    std::optional<std::vector<std::uint32_t>> drives_before;
    std::optional<aim::DeviceId> owned_id;
    bool valid_device_created = false;
    bool remove_attempted = false;

    const auto set_primary = [&](const Error& error) {
        if (!report.primary_error) report.primary_error = evidence(error);
    };
    const auto add_cleanup = [&](const Error& error) {
        report.cleanup_errors.push_back(evidence(error));
    };
    const auto fail_if_injected = [&](M1FailurePoint point) {
        if (config.failure_point != point) return false;
        set_primary(injected(point));
        return true;
    };

    auto cleanup = [&] {
        if (owned_id && !remove_attempted) {
            remove_attempted = true;
            report.cleanup.exact_remove_attempted = true;
            report.cleanup.removed_device_id = std::to_string(owned_id->value);
            auto removed = kernel.remove_device(*owned_id);
            if (!removed) {
                add_cleanup(removed.error());
            } else {
                report.cleanup.exact_remove_succeeded = true;
                auto absent = kernel.wait_until_absent(
                    *owned_id, config.cleanup_timeout);
                if (!absent) add_cleanup(absent.error());
                else report.cleanup.device_disappeared = true;
            }
        }

        if (provider) {
            auto stopped = provider->stop_and_release();
            capture_provider(report, provider->snapshot(),
                             provider->native_object_path());
            if (!stopped) add_cleanup(stopped.error());
            else report.cleanup.provider_stopped = true;

            const auto deadline = std::chrono::steady_clock::now() +
                config.cleanup_timeout;
            do {
                auto released = provider->named_objects_released();
                if (!released) {
                    add_cleanup(released.error());
                    break;
                }
                if (released.value()) {
                    report.cleanup.named_objects_released = true;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    add_cleanup({ErrorDomain::cleanup, WAIT_TIMEOUT,
                        L"wait_for_named_objects_released",
                        L"named objects remained open after timeout"});
                    break;
                }
                std::this_thread::sleep_for(config.poll_interval);
            } while (true);
        }

        if (aim_before) {
            auto after = kernel.list_devices();
            if (!after) add_cleanup(after.error());
            else {
                for (const auto id : after.value()) {
                    report.baseline.aim_device_ids_after.push_back(
                        std::to_string(id.value));
                }
                report.cleanup.aim_device_lists_match =
                    same_ids(*aim_before, after.value());
                if (!report.cleanup.aim_device_lists_match) {
                    add_cleanup({ErrorDomain::cleanup, 0,
                        L"compare_aim_baseline",
                        L"AIM device list did not return to baseline"});
                }
            }
        }

        if (drives_before) {
            const auto deadline = std::chrono::steady_clock::now() +
                config.cleanup_timeout;
            do {
                auto after = locator.snapshot_numbers();
                if (after) {
                    report.baseline.physical_drive_numbers_after.clear();
                    for (const auto number : after.value()) {
                        report.baseline.physical_drive_numbers_after.push_back(
                            std::to_string(number));
                    }
                    if (same_numbers(*drives_before, after.value())) {
                        report.cleanup.physical_drive_lists_match = true;
                        break;
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    add_cleanup({ErrorDomain::cleanup, WAIT_TIMEOUT,
                        L"compare_physical_drive_baseline",
                        L"PhysicalDrive list did not return to baseline"});
                    break;
                }
                std::this_thread::sleep_for(config.poll_interval);
            } while (true);
        }
    };

    try {
        auto guarded = execution_guard.require_elevated_and_acquire();
        if (!guarded) {
            set_primary(guarded.error());
            report.last_stage = "preflight_failed";
            report.exit_code = 3;
            return report;
        }
        report.last_stage = "preflight_passed";

        do {
            auto devices = kernel.list_devices();
            if (!devices) { set_primary(devices.error()); break; }
            aim_before = devices.value();
            for (const auto id : devices.value()) {
                report.baseline.aim_device_ids_before.push_back(
                    std::to_string(id.value));
            }
            auto drive_numbers = locator.snapshot_numbers();
            if (!drive_numbers) { set_primary(drive_numbers.error()); break; }
            drives_before = drive_numbers.value();
            for (const auto number : drive_numbers.value()) {
                report.baseline.physical_drive_numbers_before.push_back(
                    std::to_string(number));
            }
            report.last_stage = "baseline_captured";

            auto driver = kernel.query_driver();
            if (!driver) { set_primary(driver.error()); break; }
            report.driver = driver.value();
            report.last_stage = "driver_queried";

            auto created_provider = provider_factory.create();
            if (!created_provider) {
                set_primary(created_provider.error()); break;
            }
            provider = std::move(created_provider.value());
            auto started = provider->start();
            if (!started) { set_primary(started.error()); break; }
            report.provider.native_object_path = narrow(
                provider->native_object_path());
            report.last_stage = "provider_ready";
            if (fail_if_injected(M1FailurePoint::provider_ready)) break;

            const aim::ProxyDiskSpec disk_spec{
                aim::kM0DiskSize, aim::kM0SectorSize,
                provider->native_object_path()};
            auto created = kernel.create_proxy_disk(disk_spec);
            if (!created) { set_primary(created.error()); break; }
            if (created.value().value == aim::kAllDevices ||
                created.value().value == aim::kAutoDeviceNumber) {
                set_primary({ErrorDomain::evidence, 0,
                    L"create_proxy_disk(owned_id)",
                    L"driver returned an unsafe sentinel DeviceId"});
                break;
            }
            owned_id = created.value();
            valid_device_created = true;
            const auto address = aim::decode_device_address(*owned_id);
            report.created_device = M1CreatedDeviceEvidence{
                std::to_string(owned_id->value), address.path_id,
                address.target_id, address.lun, aim::kM0CreateFlags};
            report.last_stage = "create_returned";
            if (fail_if_injected(M1FailurePoint::create_returned)) break;

            auto rescanned = kernel.rescan_adapter();
            if (!rescanned) { set_primary(rescanned.error()); break; }
            auto queried = kernel.query_device(*owned_id);
            if (!queried) { set_primary(queried.error()); break; }
            if (queried.value().id != *owned_id ||
                queried.value().size_bytes != disk_spec.size_bytes ||
                queried.value().sector_size != disk_spec.sector_size ||
                queried.value().flags != aim::kM1QueryFlags ||
                queried.value().native_object_path != disk_spec.native_object_path) {
                std::wostringstream details;
                details << L"QUERY_DEVICE identity mismatch: expected id="
                        << owned_id->value << L" size=" << disk_spec.size_bytes
                        << L" sector=" << disk_spec.sector_size
                        << L" flags=" << aim::kM1QueryFlags << L" path='"
                        << disk_spec.native_object_path << L"'; actual id="
                        << queried.value().id.value << L" size="
                        << queried.value().size_bytes << L" sector="
                        << queried.value().sector_size << L" flags="
                        << queried.value().flags << L" path='"
                        << queried.value().native_object_path << L"'";
                set_primary({ErrorDomain::evidence, 0,
                    L"query_device(identity)",
                    details.str()});
                break;
            }
            report.created_device->confirmed_by_query_device = true;
            report.last_stage = "device_confirmed";
            if (fail_if_injected(M1FailurePoint::device_confirmed)) break;

            auto located = locator.wait_for(*owned_id, *drives_before);
            if (!located) { set_primary(located.error()); break; }
            report.created_device->physical_drive_number =
                located.value().physical_drive_number;
            report.created_device->physical_drive_path =
                narrow(located.value().path);
            report.created_device->disk_length = located.value().length_bytes;
            report.last_stage = "physical_drive_confirmed";
            if (fail_if_injected(M1FailurePoint::physical_drive_confirmed)) break;

            const auto before = provider->snapshot().requests;
            auto verified = protocol.verify(
                located.value(), before, [&] {
                    return provider->snapshot().requests;
                });
            if (!verified) { set_primary(verified.error()); break; }
            const auto& raw = verified.value();
            report.raw_protocol = M1RawProtocolEvidenceReport{
                raw.offset, raw.length, raw.write_sha256, raw.read_sha256,
                raw.zero_write_verified, raw.unmap_verified,
                raw.flush_succeeded};
            report.last_stage = "write_verified";
            if (fail_if_injected(M1FailurePoint::write_verified)) break;
            if (!raw.zero_write_verified) {
                set_primary({ErrorDomain::evidence, 0, L"zero_verified",
                    L"provider ZERO request was not verified"});
                break;
            }
            report.last_stage = "zero_verified";
            if (fail_if_injected(M1FailurePoint::zero_verified)) break;
            if (!raw.unmap_verified) {
                set_primary({ErrorDomain::evidence, 0, L"unmap_verified",
                    L"provider UNMAP request was not verified"});
                break;
            }
            report.last_stage = "unmap_verified";
            if (fail_if_injected(M1FailurePoint::unmap_verified)) break;

            remove_attempted = true;
            report.cleanup.exact_remove_attempted = true;
            report.cleanup.removed_device_id = std::to_string(owned_id->value);
            auto removed = kernel.remove_device(*owned_id);
            if (!removed) { set_primary(removed.error()); break; }
            report.cleanup.exact_remove_succeeded = true;
            auto absent = kernel.wait_until_absent(
                *owned_id, config.cleanup_timeout);
            if (!absent) { set_primary(absent.error()); break; }
            report.cleanup.device_disappeared = true;
            report.last_stage = "device_removed";
            if (fail_if_injected(M1FailurePoint::device_removed)) break;
        } while (false);

        cleanup();
    } catch (...) {
        set_primary({ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"run_m1_lifecycle", L"unexpected exception"});
        cleanup();
    }

    const bool counters_ok = report.provider.info_count >= 1 &&
        report.provider.read_count >= 1 && report.provider.write_count >= 1 &&
        report.provider.zero_count >= 1 && report.provider.unmap_count >= 1;
    const bool device_cleanup_ok = !valid_device_created ||
        (report.cleanup.exact_remove_attempted &&
         report.cleanup.exact_remove_succeeded &&
         report.cleanup.device_disappeared);
    report.success = !report.primary_error && report.cleanup_errors.empty() &&
        report.raw_protocol.has_value() && counters_ok && device_cleanup_ok &&
        report.cleanup.provider_stopped &&
        report.cleanup.named_objects_released &&
        report.cleanup.aim_device_lists_match &&
        report.cleanup.physical_drive_lists_match;
    report.exit_code = report.success ? 0 : 7;
    if (report.success) report.last_stage = "cleanup_verified";
    return report;
}

} // namespace myrd
