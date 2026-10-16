// The startup sweep's decisions, with no job object and no processes in them.
//
// T1-03's acceptance is "kill the worker, and the next start sweeps the leftover
// away without leaving a device or a drive letter".  The half of that which can
// be measured on every build is the half that decides *what* to sweep: which
// orphans are ours, which may be terminated, and which leftovers may be
// reclaimed.
//
// The reason to spend tests here rather than on the Win32 enumeration is that
// every dangerous failure of a sweep is a *decision* failure.  Cleaning up a
// resource that belongs to another profile, terminating a process that merely
// inherited a dead worker's pid, authorising a cleanup from a half-recorded
// identity -- none of those are bugs in reading a job object; they are bugs in
// what the sweep concluded.  So the conclusions are tested directly.

#include "tests/support/test_framework.h"

#include "src/management/worker_process_source.h"
#include "src/management/worker_sweep_policy.h"

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace myrd;
using namespace myrd::management;

namespace {

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

// A complete, well-formed observation: everything a cleanup is allowed to use.
ObservedResourceIdentity complete_identity() {
    ObservedResourceIdentity identity{};
    persistence::ImageUuid image{};
    image.bytes[0] = std::byte{0x11};
    identity.bundle_image_id = image;
    BundleFileIdentity file{};
    file.volume_serial = 0x1234;
    file.file_id[0] = std::byte{0x22};
    identity.bundle_file_identity = file;
    identity.volume_guid = "\\\\?\\Volume{5a5a5a5a-6b6b-7c7c-8d8d-9e9e9e9e9e9e}\\";
    identity.mount_target = L'R';
    identity.generation = 7;
    return identity;
}

// The job-wide verdict cases take both halves of the question: the processes the
// job reported, and the bindings the durable records hold.  The job cannot supply
// the second, which is the whole reason the verdict is a separate step.
OrphanedProcess observed(std::uint32_t pid,
                         std::optional<std::uint64_t> creation_time,
                         std::optional<product::Uuid> profile) {
    OrphanedProcess process{};
    process.process_id = pid;
    process.process_creation_time = creation_time;
    process.profile_id = std::move(profile);
    return process;
}

product::Uuid profile_a();
product::Uuid profile_b();

// `product::Uuid` has no default constructor, so an aggregate that holds one
// cannot be value-initialised -- the same constraint the other fixtures in this
// tree document.  It is built by assignment instead, with every field named.
ExpectedWorkerBinding expected_binding(const product::Uuid& profile,
                                       std::uint64_t creation_time,
                                       bool expects_running) {
    ExpectedWorkerBinding expected{profile, creation_time, expects_running};
    return expected;
}

product::Uuid profile_a() {
    return uuid("aaaaaaaa-1111-2222-3333-444444444444");
}

product::Uuid profile_b() {
    return uuid("bbbbbbbb-1111-2222-3333-444444444444");
}

} // namespace

// ---------------------------------------------------------------------------
// A pid is not an identity.
// ---------------------------------------------------------------------------

TEST_CASE(m5_sweep_matches_a_worker_on_pid_and_creation_time) {
    OrphanedWorker orphan{};
    orphan.process_id = 4242;
    orphan.process_creation_time = 111'222'333;
    orphan.profile_id = uuid("11111111-2222-3333-4444-555555555555");
    orphan.recorded_creation_time = 111'222'333;

    // Same pid, same creation time: the process the binding recorded.
    REQUIRE_EQ(classify_orphan(orphan), OrphanDisposition::ordered_teardown);
}

TEST_CASE(m5_sweep_refuses_a_recycled_pid) {
    // The load-bearing case.  Windows recycles pids, so a job enumeration that
    // matched on pid alone would eventually name a process that has nothing to do
    // with this service -- and the sweep would terminate it, or worse, adopt it
    // as a worker for a profile whose real worker is long dead.
    OrphanedWorker orphan{};
    orphan.process_id = 4242;
    orphan.process_creation_time = 999'999'999;  // the live process
    orphan.profile_id = uuid("11111111-2222-3333-4444-555555555555");
    orphan.recorded_creation_time = 111'222'333;  // what we recorded

    REQUIRE_EQ(classify_orphan(orphan), OrphanDisposition::pid_recycled);
}

TEST_CASE(m5_sweep_refuses_a_binding_with_no_creation_time) {
    // A binding carrying a pid and no creation time is not an identity; it is a
    // number that will be reused.  There is nothing to compare, so nothing may be
    // concluded -- and "I cannot tell" must not become "terminate it".
    OrphanedWorker orphan{};
    orphan.process_id = 4242;
    orphan.process_creation_time = 111'222'333;
    orphan.profile_id = uuid("11111111-2222-3333-4444-555555555555");
    orphan.recorded_creation_time = std::nullopt;

    REQUIRE_EQ(classify_orphan(orphan), OrphanDisposition::indeterminate);
}

TEST_CASE(m5_sweep_leaves_a_zero_pid_alone) {
    // A zero pid is a broken enumeration, not a process.  The sweep reports it
    // rather than acting: an enumeration that produced this cannot be trusted to
    // have produced the rest of the list either.
    OrphanedWorker orphan{};
    orphan.process_id = 0;
    orphan.process_id = 0;
    REQUIRE_EQ(classify_orphan(orphan), OrphanDisposition::indeterminate);
}

// ---------------------------------------------------------------------------
// Identity, never a name.
// ---------------------------------------------------------------------------

TEST_CASE(m5_sweep_terminates_a_worker_it_cannot_place) {
    // Inside our job, so this service started it -- `AssignProcessToJobObject` is
    // how a process gets there.  What is unknown is which profile it served, and
    // a worker whose profile cannot be named is one no operation can ever address
    // again.  It goes, and it goes *cooperatively* first so it can still run its
    // ordered teardown.
    OrphanedWorker orphan{};
    orphan.process_id = 777;
    orphan.process_creation_time = 555;
    orphan.profile_id = std::nullopt;
    orphan.recorded_creation_time = std::nullopt;

    REQUIRE_EQ(classify_orphan(orphan), OrphanDisposition::foreign);
}

// ---------------------------------------------------------------------------
// Nothing is reclaimed without a complete identity.
// ---------------------------------------------------------------------------

TEST_CASE(m5_sweep_reclaims_only_with_a_complete_identity) {
    const std::optional<ObservedResourceIdentity> identity = complete_identity();
    auto allowed = reclaimable_resources(identity);

    REQUIRE(allowed.has_value());
    // Reclaimed as the whole identity, not narrowed to the fields a caller
    // happened to need: a caller that could reclaim on a subset could reclaim on
    // whatever part was present, which is how a sweep removes somebody else's
    // volume.
    REQUIRE(*allowed == *identity);
}

TEST_CASE(m5_sweep_reclaims_nothing_when_nothing_was_observed) {
    // Not "there is nothing left behind" -- "I cannot tell".  Returning an empty
    // identity here would be the most dangerous shortcut available: it would
    // authorise a cleanup with nothing behind it.
    REQUIRE(!reclaimable_resources(std::nullopt).has_value());
}

TEST_CASE(m5_sweep_reclaims_nothing_from_a_partial_identity) {
    // Each of the three identity classes on its own is insufficient, and each is
    // a way a real record could be short: the worker died before it reported the
    // volume, or the bundle file identity was never read, or the mount was never
    // assigned.  Every one of them must refuse.
    auto missing_image = complete_identity();
    missing_image.bundle_image_id.reset();
    REQUIRE(!reclaimable_resources(missing_image).has_value());

    auto missing_file_identity = complete_identity();
    missing_file_identity.bundle_file_identity.reset();
    REQUIRE(!reclaimable_resources(missing_file_identity).has_value());

    auto missing_volume = complete_identity();
    missing_volume.volume_guid.clear();
    REQUIRE(!reclaimable_resources(missing_volume).has_value());

    auto missing_mount = complete_identity();
    missing_mount.mount_target = 0;
    REQUIRE(!reclaimable_resources(missing_mount).has_value());
}

TEST_CASE(m5_sweep_identity_completeness_is_exactly_the_rule) {
    // The predicate and the reclaim function must agree on what "complete" means,
    // or a caller could authorise by one rule and reclaim by another.
    const auto identity = complete_identity();
    REQUIRE(identity.complete());

    auto partial = identity;
    partial.volume_guid.clear();
    REQUIRE(!partial.complete());
    REQUIRE(!reclaimable_resources(partial).has_value());
    // A generation of zero is *not* incompleteness: generation zero is a real
    // generation (a freshly created bundle stands at it), and treating it as
    // absent would refuse to reclaim a brand-new profile's leftovers.
    auto generation_zero = identity;
    generation_zero.generation = 0;
    REQUIRE(generation_zero.complete());
    REQUIRE(reclaimable_resources(generation_zero).has_value());
}

// ---------------------------------------------------------------------------
// Assembling the identity from what is already durable.
// ---------------------------------------------------------------------------

namespace {

// A profile record as the registration store would hand it back: a volume that was
// assigned, mounted at R:, with a bundle image.
//
// Built as an aggregate and then filled in, because `product::Uuid` has no default
// constructor -- the same constraint that shaped `ExpectedWorkerBinding`'s helper
// above and that recurs everywhere these records are constructed in tests.  Only the
// fields this projection reads are set; the rest keep their declared defaults, so a
// case that starts needing another field has to say so.
profile::ProfileRecord durable_record() {
    profile::ProfileRecord record{
        4,
        profile::Profile{profile_a(),
                         L"S-1-5-18",
                         L"a",
                         0,
                         profile::ProfileLifecycle::ready,
                         L"C:\\bundles\\a",
                         {},
                         uuid("eeeeeeee-1111-2222-3333-444444444444")}};
    record.profile.gpt_disk_guid = uuid("11112222-3333-4444-5555-666677778888");
    record.profile.partition_guid = uuid("99998888-7777-6666-5555-444433332222");
    record.profile.volume_guid = uuid("aaaabbbb-cccc-dddd-eeee-ffff00001111");
    record.profile.mount_kind = profile::MountKind::drive_letter;
    record.profile.mount_target = L"R:";
    return record;
}

BundleFileIdentity file_identity() {
    BundleFileIdentity identity{};
    identity.volume_serial = 0xABCD;
    identity.file_id[0] = std::byte{0x42};
    return identity;
}

} // namespace

TEST_CASE(m5_identity_projects_a_complete_durable_record) {
    // The ordinary reclaimable case: everything the reclamation needs is either on
    // the profile record or supplied beside it, and the projection turns it into
    // something `reclaimable_resources` accepts.
    const auto record = durable_record();
    const auto identity =
        project_observed_identity(record, file_identity(), 9);

    REQUIRE(identity.complete());
    REQUIRE(identity.bundle_image_id.has_value());
    REQUIRE(identity.bundle_file_identity.has_value());
    REQUIRE_EQ(identity.volume_guid,
               record.profile.volume_guid->to_string());
    REQUIRE_EQ(identity.mount_target, L'R');
    REQUIRE_EQ(identity.generation, 9U);

    // And the decision accepts it, which is the whole reason to build it.
    REQUIRE(reclaimable_resources(identity).has_value());
}

TEST_CASE(m5_identity_projection_does_not_decide_by_itself) {
    // Building the identity and judging it are separate steps on purpose.  A
    // projection that returned "nothing" for an incomplete record would make "the
    // record is short" indistinguishable from "there is no record", and those lead
    // to different reports -- so an incomplete projection is *returned*, and it is
    // `reclaimable_resources` that refuses it.
    auto record = durable_record();
    record.profile.volume_guid.reset();

    const auto identity =
        project_observed_identity(record, file_identity(), 1);
    REQUIRE(!identity.complete());
    REQUIRE(!reclaimable_resources(identity).has_value());

    // The projection still produced what it could, which is what lets a caller say
    // *which* part is missing rather than only that something is.
    REQUIRE(identity.bundle_image_id.has_value());
    REQUIRE(identity.bundle_file_identity.has_value());
    REQUIRE(identity.volume_guid.empty());
}

TEST_CASE(m5_identity_refuses_to_invent_a_letter_from_a_directory_mount) {
    // The shape trap, and the reason this case exists.
    //
    // `ObservedResourceIdentity::mount_target` is one `wchar_t` -- the reclamation
    // it authorises is "clear this drive letter".  A profile record's mount target
    // is a *string* and can legitimately be a directory, so taking its first
    // character would work by accident for `R:` and would invent a letter for
    // anything else.  A directory mount must therefore project to *no* mount, which
    // makes the identity incomplete and the reclamation refuse.
    auto record = durable_record();
    record.profile.mount_kind = profile::MountKind::directory;
    record.profile.mount_target = L"C:\\mounts\\disk-a";

    const auto identity =
        project_observed_identity(record, file_identity(), 1);
    REQUIRE_EQ(identity.mount_target, L'\0');
    REQUIRE(!identity.complete());
    REQUIRE(!reclaimable_resources(identity).has_value());
}

TEST_CASE(m5_identity_normalizes_a_lowercase_letter) {
    // Windows accepts `r:` and the volume is the same volume.  Two spellings of one
    // identity must not become two identities, or a sweep could fail to match the
    // very resource it recorded.
    auto record = durable_record();
    record.profile.mount_target = L"r:";
    const auto identity =
        project_observed_identity(record, file_identity(), 1);
    REQUIRE_EQ(identity.mount_target, L'R');
}

TEST_CASE(m5_identity_rejects_a_malformed_mount_target) {
    // A drive-letter mount that is not exactly `X:` is not something to guess at.
    auto record = durable_record();
    record.profile.mount_target = L"R";
    REQUIRE_EQ(project_observed_identity(record, file_identity(), 1).mount_target,
               L'\0');

    record.profile.mount_target = L"CD:";
    REQUIRE_EQ(project_observed_identity(record, file_identity(), 1).mount_target,
               L'\0');

    record.profile.mount_target = L"1:";
    REQUIRE_EQ(project_observed_identity(record, file_identity(), 1).mount_target,
               L'\0');
}

TEST_CASE(m5_identity_without_a_bundle_file_identity_is_incomplete) {
    // The bundle's directory identity arrives with the worker's evidence and can be
    // genuinely absent -- a worker that died before reporting it.  Absent must make
    // the identity incomplete rather than being ignored, because reclaiming against
    // a bundle whose identity was never established is reclaiming on a guess.
    const auto record = durable_record();
    const auto identity =
        project_observed_identity(record, std::nullopt, 3);
    REQUIRE(!identity.complete());
    REQUIRE(!reclaimable_resources(identity).has_value());
    // The volume half is still there, which is what lets the report say what *is*
    // known.
    REQUIRE(!identity.volume_guid.empty());
}

TEST_CASE(m5_identity_generation_zero_is_a_real_generation) {
    // Consistent with `ObservedResourceIdentity::complete` and with the rest of the
    // sweep: generation zero is a freshly created bundle, not an absent value.
    const auto record = durable_record();
    const auto identity =
        project_observed_identity(record, file_identity(), 0);
    REQUIRE(identity.complete());
    REQUIRE_EQ(identity.generation, 0U);
    REQUIRE(reclaimable_resources(identity).has_value());
}

// ---------------------------------------------------------------------------
// The job-wide verdicts: what to do with each process found in the worker job.
//
// The job object can say which processes are in it; it cannot say which profile
// each served.  That lives in the durable records, so the verdicts take both and
// decide -- which is why the dangerous cases are testable without a job.
// ---------------------------------------------------------------------------

TEST_CASE(m5_sweep_verdicts_adopts_a_worker_whose_disk_should_be_serving) {
    // The one case that is not a termination.  A profile the operator expects to
    // be serving has a worker that is supposed to be alive; terminating it would
    // stop a disk somebody believes is running, and only that profile's own
    // start/stop path may decide that.
    const std::vector<OrphanedProcess> processes{
        observed(100, 5'000, profile_a())};
    const std::vector<ExpectedWorkerBinding> expected{
        expected_binding(profile_a(), 5'000, true)};

    const auto verdicts = sweep_verdicts(processes, expected);
    REQUIRE_EQ(verdicts.size(), 1U);
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::adopt);
    REQUIRE_EQ(verdicts[0].process_id, 100U);
}

TEST_CASE(m5_sweep_verdicts_asks_a_stale_worker_for_ordered_teardown) {
    // Our worker, its profile is not expected running, so it owes the ordered
    // teardown: remove the drive letter, dismount, stop AIM.  *Asked* first, not
    // killed -- which is why the job does not set KILL_ON_JOB_CLOSE.
    const std::vector<OrphanedProcess> processes{
        observed(100, 5'000, profile_a())};
    const std::vector<ExpectedWorkerBinding> expected{
        expected_binding(profile_a(), 5'000, false)};

    const auto verdicts = sweep_verdicts(processes, expected);
    REQUIRE_EQ(verdicts.size(), 1U);
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::terminate_cooperatively);
}

TEST_CASE(m5_sweep_verdicts_forces_down_a_process_it_cannot_place) {
    // Inside our job, so this service started it; no binding, so no operation can
    // ever address it again.  Forced, because there is no profile whose ordered
    // teardown it could run.
    const std::vector<OrphanedProcess> processes{
        observed(100, 5'000, std::nullopt)};

    const auto verdicts = sweep_verdicts(processes, {});
    REQUIRE_EQ(verdicts.size(), 1U);
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::terminate_forced);
}

TEST_CASE(m5_sweep_verdicts_forces_down_a_worker_for_an_unknown_profile) {
    // A binding naming a profile this service has no record for.  Treated exactly
    // like no binding at all: the service cannot say which disk the process
    // served, so it can neither adopt it nor wait for it.
    const std::vector<OrphanedProcess> processes{
        observed(100, 5'000, profile_b())};
    const std::vector<ExpectedWorkerBinding> expected{
        expected_binding(profile_a(), 5'000, false)};

    const auto verdicts = sweep_verdicts(processes, expected);
    REQUIRE_EQ(verdicts.size(), 1U);
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::terminate_forced);
}

TEST_CASE(m5_sweep_verdicts_never_touches_a_recycled_pid) {
    // The stranger-killing case, one layer up.  Same pid, same profile record,
    // different creation time: a different process is wearing the dead worker's
    // number, and neither killing it nor adopting it is defensible.
    const std::vector<OrphanedProcess> processes{
        observed(100, 9'999, profile_a())};
    const std::vector<ExpectedWorkerBinding> expected{
        expected_binding(profile_a(), 5'000, false)};

    const auto verdicts = sweep_verdicts(processes, expected);
    REQUIRE_EQ(verdicts.size(), 1U);
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::leave_alone);
}

TEST_CASE(m5_sweep_verdicts_leaves_alone_what_it_cannot_compare) {
    // A binding with no recorded creation time is a pid and nothing more; a
    // process whose creation time could not be read is unknown.  Both are "I
    // cannot tell", and that is not a licence.
    ExpectedWorkerBinding no_recorded_time = expected_binding(profile_a(), 0, false);
    no_recorded_time.recorded_creation_time.reset();
    auto verdicts = sweep_verdicts({observed(100, 5'000, profile_a())},
                                  {no_recorded_time});
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::leave_alone);

    verdicts = sweep_verdicts({observed(100, std::nullopt, profile_a())},
                              {expected_binding(profile_a(), 5'000, false)});
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::leave_alone);
}

TEST_CASE(m5_sweep_verdicts_ignores_a_zero_pid) {
    const auto verdicts =
        sweep_verdicts({observed(0, 5'000, std::nullopt)}, {});
    REQUIRE_EQ(verdicts.size(), 1U);
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::leave_alone);
}

TEST_CASE(m5_sweep_verdicts_reports_every_process_in_order) {
    // One verdict per process, in the order observed, so a caller can pair them
    // with the handles it holds.  A sweep that dropped or reordered entries would
    // leave a process with no verdict -- and the safe reading of "no verdict" is
    // not obvious enough to leave to chance.
    //
    // Four different verdicts in one list, because the interesting failure is not
    // any single one of them but a sweep that gets the *distinction* wrong:
    //
    //   10  ours, creation time agrees, disk not expected running -> ask it down
    //   20  no binding at all                                     -> force it down
    //   30  ours, but the binding records a different process      -> leave alone
    //   40  our binding, no creation time readable for the process -> leave alone
    //
    // The profile_b binding deliberately records a creation time that the live
    // process does not have.  That is a recycled pid -- the case that must never
    // be terminated -- and it is here rather than in a list of its own so that a
    // sweep which ignored creation times could not pass this case by accident.
    const std::vector<OrphanedProcess> processes{
        observed(10, 1, profile_a()), observed(20, 2, std::nullopt),
        observed(30, 3, profile_b()), observed(40, std::nullopt, profile_a())};
    const std::vector<ExpectedWorkerBinding> expected{
        expected_binding(profile_a(), 1, false),
        expected_binding(profile_b(), 999, false)};

    const auto verdicts = sweep_verdicts(processes, expected);
    REQUIRE_EQ(verdicts.size(), 4U);
    REQUIRE_EQ(verdicts[0].process_id, 10U);
    REQUIRE_EQ(verdicts[0].verdict, SweepVerdict::terminate_cooperatively);
    REQUIRE_EQ(verdicts[1].process_id, 20U);
    REQUIRE_EQ(verdicts[1].verdict, SweepVerdict::terminate_forced);
    REQUIRE_EQ(verdicts[2].process_id, 30U);
    REQUIRE_EQ(verdicts[2].verdict, SweepVerdict::leave_alone);
    REQUIRE_EQ(verdicts[3].process_id, 40U);
    REQUIRE_EQ(verdicts[3].verdict, SweepVerdict::leave_alone);
    // And every entry names the pid it is about, so a caller cannot act on a
    // verdict belonging to a different process.
    for (std::size_t index = 0; index < verdicts.size(); ++index) {
        REQUIRE_EQ(verdicts[index].process_id, processes[index].process_id);
    }
}

// ---------------------------------------------------------------------------
// The design rule that has no runtime symptom until two disks are running.
// ---------------------------------------------------------------------------

// G4 says one disk's failure must not stop another.  The way a sweep breaks that
// is not a bad decision about a process -- it is a *job-wide* termination, which
// ends every worker at once.  `TerminateJobObject` is exactly that, and its
// absence from the interface is the enforcement: a caller cannot reach what does
// not exist.
//
// Asserted as a compile-time property rather than a comment, because a comment
// does not fail the build when somebody later adds the convenient method.  The
// check is written so that *adding* a job-wide terminate makes this test stop
// compiling, which is a louder and earlier failure than a case that fails only
// when two disks are running.
namespace {

// Detects a member callable as `terminate_job`.  If someone adds one, the
// static assertion below fires and names this file.
template <typename T, typename = void>
struct has_job_wide_terminate : std::false_type {};

template <typename T>
struct has_job_wide_terminate<
    T, std::void_t<decltype(std::declval<const T&>().terminate_job())>>
    : std::true_type {};

} // namespace

TEST_CASE(m5_worker_process_source_has_no_job_wide_terminate) {
    static_assert(!has_job_wide_terminate<IWorkerProcessSource>::value,
                  "Terminating the worker job would end every disk's worker at "
                  "once, which is the G4 isolation rule's one unforgivable "
                  "shortcut.  Per-process termination only: see design spec "
                  "section 5.2 and worker_process_source.h.");
    // The assertion above is the test.  This case exists so the rule is visible
    // in the test output and in `ctest -R m5.worker_sweep_policy`, not only in a
    // header comment nobody re-reads.
    REQUIRE(true);
}

