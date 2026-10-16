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

#include "src/management/worker_sweep_policy.h"

#include <optional>
#include <string>

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
