// The M5 execution backend against a real worker process.
//
// This is the suite that executes what increments 10-12 wrote: the launcher, the
// session provider, and the adapter that drives them.  Everything before it was
// reviewed-but-unexecuted code, and this is where that stops being true.
//
// Most of what is here is the service-to-worker hop: process launch, inherited
// startup channel, pipe ACL, handshake, binding, and the commands that carry a
// profile's lifecycle across it.  The runtime lifecycle below that hop is the
// M4/M3 suites' subject, and these cases deliberately leave it alone -- a launch
// that adopts a bundle stops at the bundle kind, and one that fails to reach the
// worker at all is distinguished from one that reached it by *which* failure
// comes back.
//
// `m5_backend_creates_a_bundle_and_mounts_it_end_to_end` is the exception, and
// it is the only case in this tree that drives the whole thing: a real
// generation-zero image, a real AIM proxy disk, a real provisioned and formatted
// volume, a real drive letter, mounted and released.  It claims a letter it has
// verified is free and releases it on every path, and it is slow enough that the
// suite's own timeout has to allow for a provisioning round-trip.

#include "tests/support/test_framework.h"

#include "src/common/operation_context.h"
#include "src/management/service_worker_control.h"
#include "src/management/windows_worker_sessions.h"
#include "src/profile/profile_types.h"
#include "src/product/uuid.h"

#include <chrono>
#include <cstdint>
#include <cstring>
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

// A budget for the one command that does real work.  The session refuses a
// deadline beyond what a worker may be asked to wait, and a create provisions a
// volume through PowerShell, so the default 30 seconds is a guess that would
// show up as a transport timeout rather than as the result it was waiting for.
OperationContext provisioning_context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(100)};
}

// A drive letter this machine is not using, verified rather than assumed.  A
// real lifecycle claims a real letter here, so a case that guessed one would
// take a drive someone else is using -- or fail for a reason that has nothing to
// do with what it is testing.  Scanned from the end of the alphabet downwards,
// because that is where a desktop is least likely to keep something.
wchar_t free_drive_letter() {
    const DWORD in_use = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'R'; --letter) {
        if ((in_use & (1u << (letter - L'A'))) == 0) return letter;
    }
    REQUIRE(false);
    return L'\0';
}

bool is_letter_assigned(const wchar_t letter) {
    return (GetLogicalDrives() & (1u << (letter - L'A'))) != 0;
}

// Waits for a letter to come free again.  The teardown is asynchronous once the
// channel closes -- the worker notices, runs cleanup, and releases the mount --
// so a bare check would be a race, and sleeping a fixed guess would be a test
// that passes for the wrong reason.
bool wait_for_letter_release(const wchar_t letter) {
    for (int attempt = 0; attempt != 150; ++attempt) {
        if (!is_letter_assigned(letter)) return true;
        Sleep(100);
    }
    return !is_letter_assigned(letter);
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
// The lifecycle is a parameter because it decides which lifecycle a launch
// runs, and that is not a detail of this fixture.  It defaults to `ready`
// deliberately: a `provisioning` profile makes the worker *create and provision*
// a real volume on this machine, and a case that only wants to prove the worker
// was reached must not do that as a side effect of the fixture it borrowed.
profile::ProfileRecord profile_record(const product::Uuid& profile_id,
                                      const product::Uuid& image_id,
                                      const std::wstring& bundle_path,
                                      const std::wstring& mount_target = L"R:",
                                      const std::uint64_t revision = 7,
                                      const profile::ProfileLifecycle lifecycle =
                                          profile::ProfileLifecycle::ready) {
    profile::Profile value{
        profile_id,
        L"S-1-5-21-100-200-300-1001",
        L"work",
        1'700'000'000'000ULL,
        lifecycle,
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

// What an end-to-end create asks for.  Built as the *intent*, not as a durable
// record, because that is what production has at this point: the profile is not
// published until after the bundle action the case below drives.
constexpr std::uint64_t kCapacityBytes = 64ull * 1024 * 1024;

LifecycleOperationIntent create_intent(const product::Uuid& image_id,
                                       const TempBundle& bundle,
                                       const std::wstring& mount_target) {
    return LifecycleOperationIntent{
        LifecycleOperationKind::create,
        uuid("11111111-2222-3333-4444-555555555555"),
        image_id,
        L"S-1-5-21-100-200-300-1001",
        L"work",
        bundle.root.wstring(),
        mount_target,
        kCapacityBytes,
        false,
        control::MemoryMode::automatic,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        PhysicalMemorySnapshot{},
        0};
}

// The launched image id is a product uuid and the worker reports the
// persistence layer's sixteen raw bytes.  The bytes are the same sixteen in the
// same order, so this is the comparison the product itself makes.
persistence::ImageUuid image_bytes(const product::Uuid& id) noexcept {
    persistence::ImageUuid value{};
    const auto bytes = id.bytes();
    std::memcpy(value.bytes.data(), bytes.data(), value.bytes.size());
    return value;
}

// The provider takes a launch request, not a profile: a launch no longer always
// starts from a durable record, so the seam's input is what both producers
// translate into.  Going through the translation rather than building a request
// by hand is deliberate -- a hand-built one would not be the thing production
// sends.
WorkerLaunchRequest request_for(const profile::ProfileRecord& profile) {
    auto translated = make_launch_request(profile);
    REQUIRE(translated.ok());
    return translated.value();
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
    auto session = provider.launch(request_for(valid_profile(bundle)), context());
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
        request_for(profile_record(
            uuid("11111111-2222-3333-4444-555555555555"),
            uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"), first.root.wstring())),
        context());
    auto right = provider.launch(
        request_for(profile_record(
            uuid("12121212-3434-5656-7878-909090909090"),
            uuid("abababab-cdcd-efef-0101-232323232323"), second.root.wstring())),
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
    const auto refused = missing.launch(request_for(valid_profile(bundle)), context());
    REQUIRE(!refused.ok());
}

// The mount-target rule used to be a case here, because the provider was where
// it lived.  It lives in the translation now -- one gate for both producers, and
// the profile path additionally has to name a drive-letter *mount kind*, which
// this case never checked -- so the case moved to `unit.m5.service_worker_control`
// rather than being kept as a second copy of one rule.

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
    // `start`.  `start` cannot succeed here -- an empty directory is not a
    // manifest bundle, so recovery refuses it before touching a volume -- but the
    // *failure* is what this case is about: it must be the runtime's failure,
    // arriving over an authenticated channel, and not `execution_not_bound`,
    // which is what a launch that never happened would produce.  The two are
    // indistinguishable from a bare `!ok()`.
    //
    // The profile is `ready`, so the launch adopts.  A `provisioning` one would
    // run the create lifecycle instead, which provisions a real volume and
    // claims a real drive letter on this machine -- a side effect this case has
    // no business causing, and one it would have caused silently.
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

TEST_CASE(m5_backend_creates_a_bundle_and_mounts_it_end_to_end) {
    // §1.25's case, brought back now that the operation it needed exists. Every
    // step below has been run before in isolation against a fake, and none of it
    // has ever run in order against the real thing: a real worker process, a real
    // generation-zero image, a real AIM proxy disk, a real provisioned and
    // formatted volume, a real drive letter.
    //
    // It is also the first case in this tree to touch any of that, so its side
    // effects are stated rather than discovered: it claims a letter it has
    // verified is free, and every exit path releases it -- `cleanup` removes the
    // drive letter and dismounts, and a worker whose channel is closed without
    // one runs the same teardown as it exits.
    TempBundle bundle(L"e2e");
    const wchar_t letter = free_drive_letter();
    REQUIRE(!is_letter_assigned(letter));
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("1f1f1f1f-2e2e-3d3d-4c4c-5b5b5b5b5b5b");

    auto launch_request =
        make_launch_request(create_intent(image_id, bundle, mount_target));
    REQUIRE(launch_request.ok());
    // A create asks for a new volume, which is the whole of what unit B added:
    // the same launch used to recover and fail on an empty bundle.
    REQUIRE(launch_request.value().volume_mode
            == m5::WorkerVolumeMode::provision_new);

    WindowsWorkerSessionProvider provider(worker_executable());
    auto launched = provider.launch(launch_request.value(), context());
    REQUIRE(launched.ok());
    auto& session = *launched.value();
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    // The command §1.26 designed: it creates the bundle, provisions the volume,
    // and answers with the identity the service records as its registration.
    // A failure here exits the process at the assertion, so the reason is not
    // visible from this run -- the framework has no message form.  Locating one
    // means a temporary `fprintf` of `created.error()`, which is exactly how the
    // two defects this case found on its first run were located: a zero RAM
    // budget and a missing provisioning resource.
    const auto created = session.create_bundle(provisioning_context(), operation);
    REQUIRE(created.ok());
    REQUIRE(created.value().bundle.has_value());
    REQUIRE(created.value().bundle->complete());
    // The image is the one the launch named -- not a random one -- because the
    // service derived it before the bundle existed and validates the recorded
    // evidence against it.
    REQUIRE(created.value().bundle->image_id == image_bytes(image_id));
    REQUIRE_EQ(created.value().bundle->virtual_bytes, kCapacityBytes);
    REQUIRE_EQ(created.value().bundle->logical_sector_size, 512u);
    REQUIRE_EQ(created.value().bundle->physical_sector_size, 4096u);

    // The runtime the create brought up is observable and holds a volume with a
    // complete persistent identity. This is what the plan's provisioning-family
    // steps read instead of performing their own.
    const auto observed = session.require_runtime_observation(context());
    REQUIRE(observed.ok());
    REQUIRE(observed.value().worker_alive);
    REQUIRE(observed.value().volume.has_value());
    REQUIRE(observed.value().volume->complete());
    REQUIRE(observed.value().image_id == image_bytes(image_id));

    // And the volume is really there: the letter this case claimed is assigned.
    REQUIRE(is_letter_assigned(letter));

    const auto cleaned = session.cleanup(context(), operation);
    REQUIRE(cleaned.ok());
    // Released, which is the half of the side effect that has to be stated.
    REQUIRE(!is_letter_assigned(letter));
}

TEST_CASE(m5_backend_adapter_registers_the_bundle_it_creates) {
    // The adapter's own path to a registration, which nothing exercised until
    // now: the case above drives `ServiceWorkerSession` directly, and the unit
    // suite drives `ServiceWorkerControl` with a provider that can never launch.
    // Between them, `register_bundle`'s success branch -- mapping the worker's
    // evidence, deciding whether to hold the session, converting the image id --
    // had never run.  It is the adapter the service actually calls.
    TempBundle bundle(L"adapter-create");
    const wchar_t letter = free_drive_letter();
    REQUIRE(!is_letter_assigned(letter));
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto image_id = uuid("2e2e2e2e-3d3d-4c4c-5b5b-5b5b5b5b5b5b");
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    {
        WindowsWorkerSessionProvider provider(worker_executable());
        ServiceWorkerControl control(provider);

        WorkerActionRequest request{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        const auto result = control.execute(request);
        REQUIRE(result.ok());
        REQUIRE(result.value().lifecycle.bundle.has_value());
        const auto& evidence = result.value().lifecycle.bundle.value();

        // Every field is either the worker's authenticated reading or a product
        // constant the service already holds.  The geometry and the uuid come
        // from the inspection that verified the image; the file identity names
        // the file object, not the path.
        REQUIRE(evidence.source == BundleEvidenceSource::created_and_verified);
        REQUIRE(evidence.image_id == image_id);
        REQUIRE_EQ(evidence.logical_capacity_bytes, kCapacityBytes);
        REQUIRE_EQ(evidence.logical_sector_bytes, kProductLogicalSectorBytes);
        REQUIRE_EQ(evidence.physical_sector_bytes, kProductPhysicalSectorBytes);
        REQUIRE_EQ(evidence.payload_block_bytes, kProductPayloadBlockBytes);
        REQUIRE(evidence.manifest_name == kProductManifestName);
        REQUIRE(evidence.file_identity.volume_serial != 0);

        // The create's session is held, so the plan's later
        // `create_runtime_and_worker` finds it instead of launching a second
        // worker for one bundle -- and the steps after that read this runtime.
        const auto observed = control.require_runtime_observation(profile);
        REQUIRE(observed.ok());
        REQUIRE(observed.value().worker_alive);
        REQUIRE(observed.value().committed_generation >= 1);

        // `record_volume_identity` is a second adapter path that had never run.
        // It reads the volume's persistent GPT identities out of the live
        // worker's status and turns them into the durable registration, so a
        // complete identity here is proof the worker really provisioned and
        // adopted a volume rather than merely answering a question.
        WorkerActionRequest record{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::record_volume_identity,
            std::nullopt,
            std::nullopt,
            std::nullopt};
        const auto recorded = control.execute(record);
        REQUIRE(recorded.ok());
        REQUIRE(recorded.value().lifecycle.volume.has_value());

        REQUIRE(is_letter_assigned(letter));
    }
    // Past this scope the adapter has dropped the session, which closes the
    // channel.  That is the path a service crash takes, and it has to release
    // the mount on its own: the worker notices the closed pipe, runs its ordered
    // teardown, and removes the drive letter.
    REQUIRE(wait_for_letter_release(letter));
}

// The profile a checkpoint cut is authorized against.  It is built from the
// *same* image id the create used, because the cut checks the live runtime's
// image against the durable record's -- a worker cutting a snapshot for another
// profile's image is exactly what that check refuses.
profile::ProfileRecord checkpoint_profile(const product::Uuid& profile_id,
                                          const product::Uuid& image_id,
                                          const std::wstring& bundle_path,
                                          const std::wstring& mount_target) {
    auto record = profile_record(profile_id, image_id, bundle_path, mount_target);
    record.profile.lifecycle = profile::ProfileLifecycle::ready;
    return record;
}

TEST_CASE(m5_backend_checkpoint_crosses_the_boundary_and_commits) {
    // The two-phase checkpoint boundary against a real worker.  This is the
    // largest block of the adapter that nothing had ever run:
    // `flush_exact_volume`, `cut_candidate`, `checkpoint_across_boundary` and
    // `abort_held_candidate` all go through here for the first time, and so does
    // the P4/P5 shape they carry.  The model suite drives the *decisions* with a
    // fake; only this can show that they survive a real snapshot and a real
    // commit.
    TempBundle bundle(L"boundary");
    const wchar_t letter = free_drive_letter();
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto image_id = uuid("3f3f3f3f-4e4e-5d5d-6c6c-7b7b7b7b7b7b");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    {
        WindowsWorkerSessionProvider provider(worker_executable());
        ServiceWorkerControl control(provider);

        WorkerActionRequest create{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        REQUIRE(control.execute(create).ok());

        auto durable = checkpoint_profile(profile, image_id, bundle.root.wstring(),
                                          mount_target);
        WorkerActionRequest flush{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::flush_exact_volume,
            std::nullopt,
            durable,
            std::nullopt};
        // The flush cuts the candidate a later commit publishes.  It reaches the
        // worker's two-phase prepare, which had never run outside the model.
        REQUIRE(control.execute(flush).ok());

        // The boundary is where the service decides, with the candidate already
        // frozen in the worker.  Here it continues.
        WorkerActionRequest boundary_request{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::checkpoint,
            std::nullopt,
            durable,
            std::nullopt};
        auto committed = control.execute_checkpoint(
            boundary_request,
            [&](const CheckpointCommitBoundaryEvidence& evidence) {
                // The evidence is the live worker's own report -- the frozen
                // candidate's slot and digest, and the operation this session
                // asked for.  A boundary handed a different identity would be
                // deciding about a checkpoint nobody cut.
                REQUIRE(evidence.product_operation_id == operation);
                REQUIRE(evidence.profile_id == profile);
                REQUIRE(evidence.candidate_generation
                        == evidence.prior_generation + 1);
                return Result<CheckpointCommitBoundaryDecision>::success(
                    CheckpointCommitBoundaryDecision{true, 0, 0});
            });
        REQUIRE(committed.ok());
        REQUIRE(committed.value().checkpoint.has_value());
        const auto& checkpoint = committed.value().checkpoint.value();
        REQUIRE(checkpoint.disposition == CheckpointCommitDisposition::committed);
        REQUIRE_EQ(checkpoint.generation,
                   checkpoint.correlation.candidate_generation);
        // A landed commit is what the provisioning start path acknowledges.
        REQUIRE(committed.value().lifecycle.first_checkpoint_acknowledged);

        // And the session is still usable afterwards: a landed commit releases
        // the candidate, so a later flush can cut a fresh one.  Asserted here
        // because the withdrawal path does *not* have this property -- see the
        // case below and manifest §1.33.
        REQUIRE(control.execute(flush).ok());
    }
    REQUIRE(wait_for_letter_release(letter));
}

TEST_CASE(m5_backend_checkpoint_boundary_withdrawal_publishes_nothing) {
    // The other half of the boundary: the service withdraws the commit after the
    // candidate is frozen.  Nothing may be published, and the candidate must be
    // resolved rather than left outstanding -- an abort the worker confirmed is
    // what makes "not committed" a fact instead of an assumption.
    TempBundle bundle(L"boundary-withdraw");
    const wchar_t letter = free_drive_letter();
    const std::wstring mount_target = std::wstring(1, letter) + L":";
    const auto profile = uuid("11111111-2222-3333-4444-555555555555");
    const auto image_id = uuid("4f4f4f4f-5e5e-6d6d-7c7c-8b8b8b8b8b8b");
    const auto operation = uuid("99999999-8888-7777-6666-555555555555");

    {
        WindowsWorkerSessionProvider provider(worker_executable());
        ServiceWorkerControl control(provider);

        WorkerActionRequest create{
            operation,
            operation,
            profile,
            ManagementCommand::create_and_start,
            ActionKind::create_and_verify_bundle,
            create_intent(image_id, bundle, mount_target),
            std::nullopt,
            std::nullopt};
        REQUIRE(control.execute(create).ok());

        auto durable = checkpoint_profile(profile, image_id, bundle.root.wstring(),
                                          mount_target);
        WorkerActionRequest flush{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::flush_exact_volume,
            std::nullopt,
            durable,
            std::nullopt};
        REQUIRE(control.execute(flush).ok());

        WorkerActionRequest boundary_request{
            operation,
            operation,
            profile,
            ManagementCommand::checkpoint,
            ActionKind::checkpoint,
            std::nullopt,
            durable,
            std::nullopt};
        auto withdrawn = control.execute_checkpoint(
            boundary_request,
            [](const CheckpointCommitBoundaryEvidence&) {
                return Result<CheckpointCommitBoundaryDecision>::success(
                    CheckpointCommitBoundaryDecision{false, 0, 0});
            });
        REQUIRE(withdrawn.ok());
        REQUIRE(withdrawn.value().checkpoint.has_value());
        // Reported as not committed, at the prior generation -- because nothing
        // was published, and the generation is the runtime's own number rather
        // than one derived from what the service expected.
        REQUIRE(withdrawn.value().checkpoint->disposition
                == CheckpointCommitDisposition::not_committed);
        REQUIRE_EQ(withdrawn.value().checkpoint->generation,
                   withdrawn.value().checkpoint->correlation.prior_generation);
        REQUIRE(!withdrawn.value().lifecycle.first_checkpoint_acknowledged);

        // Deliberately *not* asserted here: that a later flush can cut a fresh
        // candidate.  It cannot.  A withdrawn candidate leaves the runtime
        // unable to prepare another one -- `image_path.open_target` reports the
        // bundle file identity differs from the expected one -- while a
        // *committed* candidate leaves the session perfectly usable (asserted in
        // the case above, which is what makes the two paths distinguishable).
        // The finding and its minimal reproduction are manifest §1.33, and it is
        // not fixed here: the expectation lives inside M3's abort handling, and
        // the transfer manifest's mandatory gate forbids M5 from changing M3's
        // bundle, journal or manifest formats.
    }
    REQUIRE(wait_for_letter_release(letter));
}
