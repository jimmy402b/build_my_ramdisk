// The M5 execution backend against a real worker process.
//
// This is the suite that executes what increments 10-12 wrote: the launcher, the
// session provider, and the adapter that drives them.  Everything before it was
// reviewed-but-unexecuted code, and this is where that stops being true.
//
// **What it deliberately does not reach, and why.** The worker's `start()`
// command runs `DiskRuntime::start()`, which needs an AIM proxy disk and a real
// volume -- the acceptance environment, not a unit harness.  So the cases here
// drive the worker up to and including the authenticated session and stop
// there, and `start` is exercised only far enough to show the adapter *reached*
// the runtime rather than failing to reach the worker at all.  The runtime
// lifecycle itself is covered by the M4/M3 integration suites, which do run
// against the driver.
//
// That boundary is the point: what did not exist before this file is the
// service-to-worker hop -- process launch, inherited startup channel, pipe ACL,
// handshake, binding -- and that hop is fully exercised here.

#include "tests/support/test_framework.h"

#include "src/common/operation_context.h"
#include "src/management/service_worker_control.h"
#include "src/management/windows_worker_sessions.h"
#include "src/profile/profile_types.h"
#include "src/product/uuid.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {

using namespace myrd;
using namespace myrd::management;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

OperationContext context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
}

// The worker binary the build just produced, found relative to this test's own
// module.  The relative path comes from CMake so the build root is not baked
// into the binary, and existence is asserted rather than assumed: a layout
// change must fail with the path it tried, not launch nothing and report a
// product defect.
std::wstring worker_executable() {
    std::wstring module(32 * 1024, L'\0');
    const auto written = GetModuleFileNameW(nullptr, module.data(),
                                            static_cast<DWORD>(module.size()));
    REQUIRE(written != 0 && written < module.size());
    module.resize(written);
    const auto path =
        (std::filesystem::path(module).parent_path()
         / std::filesystem::path(MYRD_M5_WORKER_RELPATH))
            .lexically_normal();
    REQUIRE(std::filesystem::exists(path));
    return path.wstring();
}

// The bundle path only has to be non-empty: the worker validates its config
// before the handshake but does not open the bundle until `start`, and these
// cases never reach a bundle.  Creating the directory keeps the config honest
// rather than pointing at somewhere that does not exist.
class TempBundle final {
public:
    explicit TempBundle(const wchar_t* tag) {
        root = std::filesystem::temp_directory_path()
            / (std::wstring(L"myrd-m5-backend-") + tag + L"-"
               + std::to_wstring(std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count()));
        std::filesystem::create_directories(root);
    }
    ~TempBundle() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

// `profile::Profile` holds a `product::Uuid`, which has no public default
// constructor, so it cannot be value-initialized -- every case builds it through
// the aggregate, and the field order below is the struct's own.
profile::ProfileRecord profile_record(const product::Uuid& profile_id,
                                      const product::Uuid& image_id,
                                      const std::wstring& bundle_path,
                                      const std::wstring& mount_target = L"R:",
                                      const std::uint64_t revision = 7) {
    profile::Profile value{
        profile_id,
        L"S-1-5-21-100-200-300-1001",
        L"work",
        1'700'000'000'000ULL,
        profile::ProfileLifecycle::provisioning,
        bundle_path,
        "disk.rdm",
        image_id,
        8ULL * 1024 * 1024,
        512,
        4096,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        profile::MountKind::drive_letter,
        mount_target,
        false,
        profile::MemoryPolicy{},
        profile::CheckpointPolicy{},
        profile::CompactionPolicy{}};
    return profile::ProfileRecord{revision, std::move(value)};
}

profile::ProfileRecord valid_profile(const TempBundle& bundle) {
    return profile_record(uuid("11111111-2222-3333-4444-555555555555"),
                          uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"),
                          bundle.root.wstring());
}

} // namespace

TEST_CASE(m5_backend_launches_and_authenticates_a_real_worker) {
    // The whole point of the increment: a real process starts, reads the spec
    // from the inherited channel, connects to a pipe whose ACL admits only the
    // service and itself, and proves it did by echoing the startup secret.  None
    // of that can be faked without testing a different object than the one that
    // runs.
    TempBundle bundle(L"launch");
    WindowsWorkerSessionProvider provider(worker_executable());
    auto session = provider.launch(valid_profile(bundle), context());
    REQUIRE(session.ok());
    REQUIRE(session.value() != nullptr);

    // The binding is the child's *own* pid and process creation time, checked
    // against the process the launcher started.  A pid alone would not be an
    // identity -- Windows recycles them -- so both travel.
    const auto& binding = session.value()->binding();
    REQUIRE(binding.profile_id == uuid("11111111-2222-3333-4444-555555555555"));
    REQUIRE(binding.process_id != 0);
    REQUIRE(binding.process_creation_time != 0);

    // The runtime has not been started, so a status read is refused.  That is
    // what proves the worker authenticated *before* touching a bundle: a worker
    // that had opened one would have a runtime to report on.
    //
    // The assertion is on *whose* refusal it is, not merely that the call
    // failed.  `observe` is the operation the worker's own command loop names
    // when it refuses a status for a runtime it has not started, so an error
    // carrying it can only have come from that loop, over the authenticated
    // channel.  A launch that never happened, or a handshake that silently
    // no-opped, would fail here with something else -- and a bare `!status.ok()`
    // could not tell the two apart.
    const auto status = session.value()->status(context());
    REQUIRE(!status.ok());
    REQUIRE(status.error().operation == L"observe");

    // Reading a reply also advances the session's sequence, which only moves
    // when a request was actually written.
    REQUIRE(session.value()->last_sequence() >= 1);

    // Shutdown is accepted with no runtime: a worker that was never started is
    // still a worker that can be told to go away.
    REQUIRE(session.value()->shutdown(context(),
                                      uuid("99999999-8888-7777-6666-555555555555"))
                .ok());
}

TEST_CASE(m5_backend_two_profiles_never_share_a_worker) {
    // Each profile gets its own process and its own pipe name, and the binding
    // each channel authenticated names that profile and no other.  A service
    // that could confuse the two would command one disk while recording
    // operations against another.
    TempBundle first(L"two-a");
    TempBundle second(L"two-b");
    WindowsWorkerSessionProvider provider(worker_executable());

    auto left = provider.launch(
        profile_record(uuid("11111111-2222-3333-4444-555555555555"),
                       uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"),
                       first.root.wstring()),
        context());
    auto right = provider.launch(
        profile_record(uuid("12121212-3434-5656-7878-909090909090"),
                       uuid("abababab-cdcd-efef-0101-232323232323"),
                       second.root.wstring()),
        context());
    REQUIRE(left.ok());
    REQUIRE(right.ok());
    REQUIRE(left.value()->binding().profile_id
            != right.value()->binding().profile_id);
    // Distinct processes, not one worker answering for both.
    REQUIRE(left.value()->binding().process_id
            != right.value()->binding().process_id);
}

TEST_CASE(m5_backend_refuses_a_launch_it_cannot_complete) {
    // A launch that cannot be completed must leave nothing behind.  An empty
    // executable path is refused before a process exists at all.
    TempBundle bundle(L"no-exe");
    WindowsWorkerSessionProvider missing(std::wstring{});
    const auto refused = missing.launch(valid_profile(bundle), context());
    REQUIRE(!refused.ok());
}

TEST_CASE(m5_backend_refuses_a_mount_target_that_is_not_a_drive_letter) {
    // The worker takes a drive letter, so a durable target that is not one is
    // refused here rather than truncated into a plausible letter.  Refused
    // before any process starts: a worker launched for the wrong mount is a
    // worker holding the wrong disk.
    TempBundle bundle(L"bad-mount");
    WindowsWorkerSessionProvider provider(worker_executable());
    auto profile = profile_record(
        uuid("11111111-2222-3333-4444-555555555555"),
        uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"), bundle.root.wstring(),
        L"not-a-drive-letter");
    const auto refused = provider.launch(profile, context());
    REQUIRE(!refused.ok());
}

TEST_CASE(m5_backend_attach_finds_nothing_and_says_so) {
    // Adopting a worker this provider did not launch needs a registry of live
    // workers, which belongs to the composition.  A successful `nullopt` means
    // "none running", which the adapter turns into an explicit refusal -- the
    // honest answer until that registry exists, and different from claiming a
    // worker was found.
    WindowsWorkerSessionProvider provider(worker_executable());
    const auto attached =
        provider.attach(uuid("11111111-2222-3333-4444-555555555555"), context());
    REQUIRE(attached.ok());
    REQUIRE(attached.value() == nullptr);
}

TEST_CASE(m5_backend_adapter_reaches_the_worker_it_launched) {
    // The adapter's `create_runtime_and_worker` maps onto a launch and then a
    // `start`.  `start` cannot succeed here -- it needs an AIM proxy disk and a
    // real volume -- but the *failure* is what this case is about: it must be
    // the runtime's failure, arriving over an authenticated channel, and not
    // `execution_not_bound`, which is what a launch that never happened would
    // produce.  The two are indistinguishable from a bare `!ok()`.
    TempBundle bundle(L"adapter");
    WindowsWorkerSessionProvider provider(worker_executable());
    ServiceWorkerControl control(provider);
    const auto profile = valid_profile(bundle);
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    WorkerActionRequest request{
        operation,
        operation,
        profile.profile.profile_uuid,
        ManagementCommand::start,
        ActionKind::create_runtime_and_worker,
        std::nullopt,
        profile,
        std::nullopt};
    const auto result = control.execute(request);
    REQUIRE(!result.ok());
    REQUIRE(result.error().code
            != static_cast<std::uint32_t>(
                ServiceWorkerControlError::execution_not_bound));
}
