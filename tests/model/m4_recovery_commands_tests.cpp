// The recovery command family: repair, rebind and forget.
//
// These are the commands a broken profile is recovered with, and they carry the
// hardest constraints in the product.  `disk forget` may only happen when the
// service can *prove* the writer lock, the matching device and the retained
// bundle identity are safe; when it cannot, the transfer manifest's mandatory
// gate requires it to refuse, retain the registration and the bundle, and never
// report success.  Nothing tested any of the three: the model suite covered
// checkpoint and stop, and `tests/model/m4_recovery_commands_tests.cpp` -- named
// by Task 6's file list -- did not exist.
//
// The fixture is the checkpoint model's, included for its fakes alone.  The
// recovery commands share its profile store, its operation store and its worker
// control, and re-implementing them here would test a different object than the
// one that runs.

#include "tests/support/test_framework.h"

#define MYRD_CHECKPOINT_FIXTURE_ONLY
#include "m4_checkpoint_operation_tests.cpp"

using namespace myrd;
using namespace myrd::management;

namespace {

product::Uuid work_profile() {
    return uuid("11111111-1111-4111-8111-111111111111");
}

// A profile a recovery command can be admitted for: ready, stopped, idle and
// clean.  The checkpoint fixture's profile is running, and the transition
// contract refuses repair, rebind and forget on a disk that is up.
void make_stopped(Fixture& fixture) {
    auto snapshot = fixture.profiles.snapshot();
    snapshot.state = registered_profile_state(
        profile::ProfileLifecycle::ready,
        product::DiskState{product::RuntimeState::stopped,
                           product::ActivityState::idle,
                           product::PersistenceState::clean,
                           product::HealthState::healthy});
    fixture.profiles.replace(std::move(snapshot));
}

// The recovery commands are administrator-only by design (`disk.forget`,
// `disk.repair-profile` and `disk.rebind` all take `administrator_only` in
// `authorization.cpp`), and the checkpoint fixture's caller is an authenticated
// *owner*.  A case that used it would be refused by the authorization gate and
// would never reach the property it meant to test -- which is what the first
// version of these cases did.
ipc::CallerIdentity administrator(const std::wstring& sid) {
    return {sid, true, false, false, true, true, false, false};
}

ServiceCommandRequest recovery_request(const Fixture& fixture,
                                       control::Command command,
                                       std::string_view request_id) {
    return {uuid(request_id), std::move(command),
            administrator(fixture.profiles.snapshot().record.profile.owner_sid),
            true, false, std::nullopt, ClientWaitState::active};
}

} // namespace

TEST_CASE(recovery_forget_is_refused_when_safety_cannot_be_proven) {
    // The mandatory gate, stated as a test.  A forget that cannot prove the
    // writer lock, the matching device and the retained bundle identity are
    // safe must be refused -- and refused in the way that matters: the
    // registration stays, the bundle stays, and nothing reports success.  A
    // forget that "succeeded" without the proof would be a disk destroyed on the
    // strength of an assumption.
    Fixture fixture(product::PersistenceState::clean);
    make_stopped(fixture);
    fixture.worker.refuse_forget_proof = true;
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    // The forget is admitted -- the *request* is well formed and the state
    // allows it -- and it is execution that refuses, because execution is where
    // the proof is taken.  Asserting only on `dispatch` would have tested the
    // request validator and called it the safety property.
    const auto queued = core.dispatch(recovery_request(
        fixture,
        control::Command(control::DiskForgetCommand{
            work_profile(), work_profile()}),
        "88888888-8888-4888-8888-888888888881"));
    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());
    const auto terminal = core.execute_operation(
        queued.value().operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE(terminal.value().state != OperationState::succeeded);

    // The service asked for the proof rather than deciding on its own.
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 1U);
    // A tombstone in `reserved` is the durable record of *intent* -- it is what
    // lets a crash mid-forget be resolved -- and it is not a claim that anything
    // was removed.  `retained_bundle_identity` is part of that intent: it names
    // the bundle that must be kept *because* the proof did not come.  The
    // assertion is therefore on the phase alone, and it was narrower on the
    // first attempt only because I had guessed at the rest.
    REQUIRE(fixture.profiles.forget_tombstone.has_value());
    REQUIRE(fixture.profiles.forget_tombstone->phase
            == ForgetTombstonePhase::reserved);
    // And the profile is still there, which is the half of the requirement that
    // a caller cannot see from the error alone.
    REQUIRE(fixture.profiles.snapshot().record.profile.profile_uuid
            == work_profile());
}

TEST_CASE(recovery_forget_requires_the_confirmation_to_match) {
    // The confirmation is the user's own statement of what they mean to destroy.
    // A mismatch is refused before the worker is asked anything: there is no
    // proof worth taking for a request that does not name the profile it would
    // remove.
    Fixture fixture(product::PersistenceState::clean);
    make_stopped(fixture);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    const auto refused = core.dispatch(recovery_request(
        fixture,
        control::Command(control::DiskForgetCommand{
            work_profile(),
            uuid("99999999-9999-4999-8999-999999999999")}),
        "88888888-8888-4888-8888-888888888882"));
    REQUIRE(!refused.ok());
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 0U);
    REQUIRE(!fixture.profiles.forget_tombstone.has_value());
}

TEST_CASE(recovery_forget_is_refused_for_a_profile_that_is_not_stopped) {
    // Forget removes the registration and the bundle.  On a disk that is up,
    // that is a data-loss command, so the transition contract refuses it on the
    // state rather than on the proof -- and it must refuse before the worker is
    // asked, or a refusal would depend on what a live worker happened to answer.
    Fixture fixture(product::PersistenceState::clean); // running
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    const auto refused = core.dispatch(recovery_request(
        fixture,
        control::Command(control::DiskForgetCommand{
            work_profile(), work_profile()}),
        "88888888-8888-4888-8888-888888888883"));
    REQUIRE(!refused.ok());
    REQUIRE_EQ(fixture.worker.forget_proof_calls, 0U);
    REQUIRE(!fixture.profiles.forget_tombstone.has_value());
}

TEST_CASE(recovery_repair_restores_the_previous_durable_truth) {
    // Repair is the way back from a profile whose durable state is needs-attention
    // -- a record the service can no longer act on.  It restores the state the
    // profile held before the operation that broke it, which is the only
    // defensible answer: the current record is the one that is wrong.
    Fixture fixture(product::PersistenceState::clean);
    make_stopped(fixture);
    auto attention = fixture.profiles.snapshot();
    attention.state.disk.health = product::HealthState::needs_attention;
    fixture.profiles.replace(attention);
    // The store will only repair back to the state it was shown, which is the
    // compare-and-swap that stops a repair from ratifying a record nobody
    // expected.
    fixture.profiles.repair_candidate = attention;

    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);
    const auto queued = core.dispatch(recovery_request(
        fixture,
        control::Command(control::DiskRepairProfileCommand{work_profile(), true}),
        "88888888-8888-4888-8888-888888888884"));
    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());
    const auto terminal = core.execute_operation(
        queued.value().operation_id.value());
    REQUIRE(terminal.ok());
    REQUIRE_EQ(terminal.value().state, OperationState::succeeded);
    REQUIRE_EQ(fixture.profiles.repair_calls, 1U);
}

TEST_CASE(recovery_repair_is_refused_for_a_profile_that_is_healthy) {
    // The mirror.  A healthy profile has nothing to repair, and "repairing" it
    // would rewrite a record that is already correct -- so the contract refuses
    // it on health rather than letting a caller reset durable truth it merely
    // dislikes.
    Fixture fixture(product::PersistenceState::clean);
    make_stopped(fixture);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    const auto refused = core.dispatch(recovery_request(
        fixture,
        control::Command(control::DiskRepairProfileCommand{work_profile(), true}),
        "88888888-8888-4888-8888-888888888885"));
    REQUIRE(!refused.ok());
    REQUIRE_EQ(fixture.profiles.repair_calls, 0U);
}

TEST_CASE(recovery_rebind_refuses_an_image_the_profile_does_not_hold) {
    // Rebind points a profile at a bundle, and the caller has to name the image
    // they expect to find there.  Naming one the profile does not hold is
    // refused at the request: a rebind is a statement about *this* profile's
    // image, and accepting a different one would let the durable record be
    // redirected by whoever asked.
    Fixture fixture(product::PersistenceState::clean);
    make_stopped(fixture);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    const auto refused = core.dispatch(recovery_request(
        fixture,
        control::Command(control::DiskRebindCommand{
            work_profile(),
            L"D:\\Images\\work.myrd",
            uuid("77777777-7777-4777-8777-777777777777")}),
        "88888888-8888-4888-8888-888888888886"));
    REQUIRE(!refused.ok());
}

TEST_CASE(recovery_rebind_admits_the_image_the_profile_holds) {
    // The mirror, and the reason the case above is not vacuous: naming the
    // profile's own image is admitted, so what was refused really was the
    // mismatch rather than rebind failing for every input.
    Fixture fixture(product::PersistenceState::clean);
    make_stopped(fixture);
    ServiceCore core(
        fixture.clock, fixture.profiles, fixture.operations, fixture.worker,
        fixture.logger, fixture.ids, fixture.memory);

    const auto queued = core.dispatch(recovery_request(
        fixture,
        control::Command(control::DiskRebindCommand{
            work_profile(),
            L"D:\\Images\\work.myrd",
            fixture.profiles.snapshot().record.profile.image_uuid}),
        "88888888-8888-4888-8888-888888888887"));
    REQUIRE(queued.ok());
    REQUIRE(queued.value().operation_id.has_value());
    const auto terminal = core.execute_operation(
        queued.value().operation_id.value());
    REQUIRE(terminal.ok());
}
