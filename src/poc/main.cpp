#include "cli.h"
#include "lifecycle_runner.h"
#include "m0_provider.h"
#include "poc_report.h"
#include "src/common/authenticode.h"
#include "src/common/process_snapshot.h"
#include "src/common/sha256.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/physical_drive_locator.h"
#include "src/driver/windows_srb_transport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <securitybaseapi.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>

namespace {

void publish_report(const std::string& json, const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "ERROR: failed to write report to %s\n",
                     path.c_str());
        return;
    }
    f.write(json.data(), static_cast<std::streamsize>(json.size()));
    f.close();
    std::printf("Report written to %s\n", path.c_str());
}

std::string current_utc() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

} // anonymous namespace

int wmain(int argc, wchar_t* argv[]) {
    myrd::poc::PocReport report;

    try {
        report.started_utc = current_utc();

        // Parse arguments
        std::vector<std::wstring_view> av;
        for (int i = 1; i < argc; ++i)
            av.push_back(std::wstring_view(argv[i]));

#ifdef _DEBUG
        bool debug = true;
#else
        bool debug = false;
#endif

        auto parsed = myrd::poc::parse_command_line(av, debug);
        if (!parsed) {
            report.success = false;
            report.exit_code = 2;
            report.last_stage = "parse_error";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            std::fprintf(stderr, "Usage: myrd-m0-poc query-driver --report <path>\n");
            std::fprintf(stderr, "       myrd-m0-poc run --report <path>\n");
            return 2;
        }

        auto cmd = parsed.value();
        report.mode = (cmd.mode == myrd::poc::CommandMode::query_driver)
                          ? "query-driver" : "run";
        report.run_id = current_utc(); // simplified — will be replaced with CSPRNG

        // ── query-driver mode ─────────────────────────────────
        if (cmd.mode == myrd::poc::CommandMode::query_driver) {
            report.last_stage = "query_started";

            auto adapter = myrd::aim::discover_aim_adapter();
            if (!adapter) {
                report.exit_code = 5;
                report.success = false;
                report.finished_utc = current_utc();
                auto json = myrd::poc::serialize_report(report);
                publish_report(json, cmd.report_path.string());
                return 5;
            }

            myrd::aim::WindowsSrbTransport transport(
                adapter.value().handle.get());
            myrd::aim::DirectAimAdapter aim(transport);

            auto ver = aim.query_version();
            if (ver)
                report.driver.query_version_raw =
                    std::to_string(ver.value().raw);

            auto devices = aim.list_devices();
            if (devices) {
                for (auto& d : devices.value())
                    report.baseline.aim_device_ids_before.push_back(
                        std::to_string(d.value));
            }

            auto pd = myrd::aim::snapshot_physical_drive_numbers();
            if (pd) {
                for (auto n : pd.value())
                    report.baseline.physical_drive_numbers_before.push_back(
                        std::to_string(n));
            }

            // Post-snapshot (same as before for query mode)
            report.baseline.aim_device_ids_after =
                report.baseline.aim_device_ids_before;
            report.baseline.physical_drive_numbers_after =
                report.baseline.physical_drive_numbers_before;

            report.success = true;
            report.exit_code = 0;
            report.last_stage = "report_written";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            publish_report(json, cmd.report_path.string());
            return 0;
        }

        // ── run mode ──────────────────────────────────────────
        // Check admin elevation
        BOOL is_admin = FALSE;
        SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
        PSID admin_group = nullptr;
        if (AllocateAndInitializeSid(&nt_auth, 2,
                SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                0, 0, 0, 0, 0, 0, &admin_group)) {
            CheckTokenMembership(nullptr, admin_group, &is_admin);
            FreeSid(admin_group);
        }
        if (!is_admin) {
            report.exit_code = 3;
            report.success = false;
            report.last_stage = "preflight_failed";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            publish_report(json, cmd.report_path.string());
            return 3;
        }

        // Global serial mutex
        myrd::UniqueHandle run_mutex(
            CreateMutexW(nullptr, FALSE, L"Global\\myrd-m0-run-mutex"));
        if (!run_mutex ||
            WaitForSingleObject(run_mutex.get(), 0) != WAIT_OBJECT_0) {
            report.exit_code = 3;
            report.success = false;
            report.last_stage = "preflight_failed";
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            publish_report(json, cmd.report_path.string());
            ReleaseMutex(run_mutex.get());
            return 3;
        }

        // Build concrete components
        auto adapter = myrd::aim::discover_aim_adapter();
        if (!adapter) {
            report.exit_code = 5;
            report.success = false;
            ReleaseMutex(run_mutex.get());
            report.finished_utc = current_utc();
            auto json = myrd::poc::serialize_report(report);
            publish_report(json, cmd.report_path.string());
            return 5;
        }

        myrd::aim::WindowsSrbTransport transport(
            adapter.value().handle.get());
        myrd::aim::DirectAimAdapter aim(transport);

        // Kernel adapter wrapper (for lifecycle runner)
        struct KernelAdapter : myrd::poc::IKernelDiskAdapter {
            myrd::aim::DirectAimAdapter& aim;
            myrd::aim::WindowsSrbTransport& transport;
            KernelAdapter(myrd::aim::DirectAimAdapter& a,
                          myrd::aim::WindowsSrbTransport& t)
                : aim(a), transport(t) {}
            myrd::Result<std::vector<myrd::aim::DeviceId>> list_devices() override { return aim.list_devices(); }
            myrd::Result<myrd::aim::DeviceId> create_proxy_disk(const myrd::aim::ProxyDiskSpec& s) override { return aim.create_proxy_disk(s); }
            myrd::Result<myrd::aim::DiskInfo> query_device(myrd::aim::DeviceId id) override { return aim.query_device(id); }
            myrd::Result<void> remove_device(myrd::aim::DeviceId id) override { return aim.remove_device(id); }
            myrd::Result<void> wait_until_absent(myrd::aim::DeviceId id, std::chrono::milliseconds to) override {
                auto deadline = std::chrono::steady_clock::now() + to;
                while (std::chrono::steady_clock::now() < deadline) {
                    auto list = aim.list_devices();
                    if (!list) return myrd::Result<void>::failure(list.error());
                    bool found = false;
                    for (auto& d : list.value())
                        if (d.value == id.value) { found = true; break; }
                    if (!found) return myrd::Result<void>::success();
                    Sleep(250);
                }
                return myrd::Result<void>::success();
            }
        };

        // Disk locator wrapper
        struct DiskLocator : myrd::poc::IDiskLocator {
            myrd::Result<std::vector<std::uint32_t>> snapshot_numbers() override { return myrd::aim::snapshot_physical_drive_numbers(); }
            myrd::Result<myrd::aim::LocatedDisk> wait_for(myrd::aim::DeviceId id, std::span<const std::uint32_t> bl) override { return myrd::aim::wait_for_created_physical_drive(id, bl, std::chrono::seconds(15)); }
        };

        KernelAdapter kAdapter(aim, transport);
        DiskLocator kLocator;

        // M0Provider for lifecycle
        myrd::poc::M0Provider provider;
        // Fake round-trip for now (real implementation needs IRawDiskIo)
        struct NoOpRT : myrd::poc::IRoundTrip {
            myrd::Result<myrd::poc::RawRoundTripEvidence> run(const myrd::aim::LocatedDisk&) override {
                return myrd::Result<myrd::poc::RawRoundTripEvidence>::failure({myrd::ErrorDomain::internal,0,L"",L""});
            }
        };
        NoOpRT noopRt;

        myrd::poc::RunConfig config;
        config.run_id = current_utc();
        config.debug_build = debug;
        config.injected_failure = cmd.injected_failure;
        config.report_path = cmd.report_path.string();

        auto result = myrd::poc::run_lifecycle(
            provider, kAdapter, kLocator, noopRt, config);

        result.finished_utc = current_utc();
        auto json = myrd::poc::serialize_report(result);
        publish_report(json, cmd.report_path.string());

        ReleaseMutex(run_mutex.get());
        return result.exit_code;

    } catch (const std::exception& e) {
        report.exit_code = 12;
        report.success = false;
        report.last_stage = "exception";
        report.finished_utc = current_utc();
        auto json = myrd::poc::serialize_report(report);
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 12;
    } catch (...) {
        report.exit_code = 12;
        report.success = false;
        report.last_stage = "unknown_exception";
        report.finished_utc = current_utc();
        auto json = myrd::poc::serialize_report(report);
        std::fprintf(stderr, "FATAL: unknown exception\n");
        return 12;
    }
}
