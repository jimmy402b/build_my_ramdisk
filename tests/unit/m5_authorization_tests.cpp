// unit.m5.authorization -- the public-command rules and the visibility they produce (R-11).
//
// **Pure model test.**  It builds CallerIdentity values by hand: no service, no pipe, no device.
// The process-level non-elevated e2e could not be made to run on this guest (see
// docs/validation/m5-stab-09-nonadmin.md), so the rule table is pinned here, where the decision
// actually lives, and the service-side owner comparison is exercised by its own code path.

#include "test_framework.h"

#include "src/control/authorization.h"
#include "src/control/ipc_codec.h"

#include <string>

namespace {

myrd::ipc::CallerIdentity administrator() {
    return myrd::ipc::CallerIdentity{L"S-1-5-21-1-2-3-500", true, false, false,
                                     true, true, false, false};
}

myrd::ipc::CallerIdentity filtered_administrator() {
    // The same account after UAC filtering: it is an administrator, but not elevated.
    return myrd::ipc::CallerIdentity{L"S-1-5-21-1-2-3-500", true, false, false,
                                     true, false, false, false};
}

myrd::ipc::CallerIdentity standard_user() {
    return myrd::ipc::CallerIdentity{L"S-1-5-21-1-2-3-1001", true, false, false,
                                     false, false, false, false};
}

std::uint32_t id_of(const myrd::ipc::CommandId id) {
    return static_cast<std::uint32_t>(id);
}

} // namespace

TEST_CASE(m5_authorization_disk_show_is_owner_filtered) {
    // R-11: a non-elevated caller may read a profile it owns.  The rule is "caller filtered",
    // and the service compares the profile's owner because the rule cannot see the resource.
    using myrd::ipc::CommandId;
    const auto rule = myrd::ipc::public_command_authorization_rule(id_of(CommandId::disk_show));
    REQUIRE(rule.has_value());
    REQUIRE(*rule == myrd::ipc::AuthorizationRule::caller_filtered);

    const auto as_user = myrd::ipc::authorize_public_command(
        id_of(CommandId::disk_show), standard_user(),
        std::optional<std::wstring_view>(L"S-1-5-21-1-2-3-1001"));
    REQUIRE(as_user.allowed);
    REQUIRE(as_user.visibility == myrd::ipc::AuthorizationVisibility::caller_only);

    // The same caller asking about someone else's profile is still "allowed, filtered": the
    // owner comparison is the service's job and is what turns it into a refusal.
    const auto other = myrd::ipc::authorize_public_command(
        id_of(CommandId::disk_show), standard_user(),
        std::optional<std::wstring_view>(L"S-1-5-21-1-2-3-500"));
    REQUIRE(other.allowed);
    REQUIRE(other.visibility == myrd::ipc::AuthorizationVisibility::caller_only);
}

TEST_CASE(m5_authorization_mutations_stay_administrator_only) {
    using myrd::ipc::CommandId;
    for (const auto id : {CommandId::disk_create, CommandId::disk_import,
                          CommandId::disk_forget, CommandId::disk_export,
                          CommandId::disk_repair_profile, CommandId::disk_rebind}) {
        const auto rule = myrd::ipc::public_command_authorization_rule(id_of(id));
        REQUIRE(rule.has_value());
        REQUIRE(*rule == myrd::ipc::AuthorizationRule::administrator_only);

        const auto as_user = myrd::ipc::authorize_public_command(id_of(id), standard_user(),
                                                                 std::nullopt);
        REQUIRE(!as_user.allowed);
        const auto as_filtered = myrd::ipc::authorize_public_command(
            id_of(id), filtered_administrator(), std::nullopt);
        REQUIRE(!as_filtered.allowed);
        const auto as_admin = myrd::ipc::authorize_public_command(
            id_of(id), administrator(), std::nullopt);
        REQUIRE(as_admin.allowed);
        REQUIRE(as_admin.visibility == myrd::ipc::AuthorizationVisibility::all);
    }
}

TEST_CASE(m5_authorization_lifecycle_commands_are_owner_or_administrator) {
    // start/stop/checkpoint/compact are the owner's own operations: an owner may run them on
    // its profile without elevation, and anyone else is refused.  This is the rule that makes
    // a user's own disk usable without elevation (R-11's read path plus the lifecycle).
    using myrd::ipc::CommandId;
    for (const auto id : {CommandId::disk_start, CommandId::disk_stop,
                          CommandId::disk_checkpoint, CommandId::disk_compact}) {
        const auto rule = myrd::ipc::public_command_authorization_rule(id_of(id));
        REQUIRE(rule.has_value());
        REQUIRE(*rule == myrd::ipc::AuthorizationRule::owner_or_administrator);

        const auto as_owner = myrd::ipc::authorize_public_command(
            id_of(id), standard_user(), std::optional<std::wstring_view>(L"S-1-5-21-1-2-3-1001"));
        REQUIRE(as_owner.allowed);
        REQUIRE(as_owner.visibility == myrd::ipc::AuthorizationVisibility::caller_only);

        const auto as_outsider = myrd::ipc::authorize_public_command(
            id_of(id), standard_user(), std::optional<std::wstring_view>(L"S-1-5-21-1-2-3-500"));
        REQUIRE(!as_outsider.allowed);
    }
}

TEST_CASE(m5_authorization_list_and_show_share_the_caller_filtered_rule) {
    using myrd::ipc::CommandId;
    const auto list_rule = myrd::ipc::public_command_authorization_rule(id_of(CommandId::disk_list));
    const auto show_rule = myrd::ipc::public_command_authorization_rule(id_of(CommandId::disk_show));
    REQUIRE(list_rule.has_value());
    REQUIRE(show_rule.has_value());
    REQUIRE(*list_rule == *show_rule);
    REQUIRE(*list_rule == myrd::ipc::AuthorizationRule::caller_filtered);

    // An unauthenticated caller is refused before the rule is even consulted.
    auto anonymous = standard_user();
    anonymous.authenticated = false;
    const auto decision = myrd::ipc::authorize_public_command(
        id_of(CommandId::disk_list), anonymous, std::nullopt);
    REQUIRE(!decision.allowed);
    REQUIRE(decision.visibility == myrd::ipc::AuthorizationVisibility::none);
}

TEST_CASE(m5_authorization_network_logons_are_refused) {
    using myrd::ipc::CommandId;
    auto network = standard_user();
    network.network = true;
    const auto decision = myrd::ipc::authorize_public_command(
        id_of(CommandId::disk_list), network, std::nullopt);
    REQUIRE(!decision.allowed);
}
