// The GUI's view model (T3-02): what a row says, and which field is allowed to be the verdict.
//
// These are the two claims worth testing without a desktop.  A list that composes its own status
// from runtime and health will show a blocked disk as fine -- the service says "blocked/needs
// attention" in `aggregate` while the runtime is still running, because a disk whose writes are
// no longer durable is still a running disk.  And a memory column that prints "0.0 / 0.0 MiB" for
// a stopped disk reads as a measurement rather than as an absence.

#include "tests/support/test_framework.h"

#include "src/gui/gui_view_model.h"

#include <string>
#include <vector>

namespace {

myrd::product::ServiceDiskView disk(const char* mount,
                                    const myrd::product::ServiceAggregate aggregate,
                                    const myrd::product::ServiceRuntime runtime,
                                    const myrd::product::ServiceHealth health,
                                    const std::uint64_t generation,
                                    const std::uint64_t resident,
                                    const std::uint64_t budget) {
    auto id = myrd::product::Uuid::parse("11111111-1111-4111-8111-111111111111");
    REQUIRE(id.ok());
    myrd::product::ServiceDiskView view{};
    view.profile_id = id.value();
    view.mount_target = std::string(mount);
    view.aggregate = aggregate;
    view.runtime = runtime;
    view.health = health;
    view.confirmed_generation = generation;
    view.resident_bytes = resident;
    view.budget_bytes = budget;
    return view;
}

} // namespace

TEST_CASE(m5_gui_the_verdict_is_the_aggregate_and_nothing_else) {
    // Runtime and health disagree with the verdict on purpose: this is exactly the shape a
    // disk with blocked persistence has.
    const auto row = myrd::gui::make_disk_row(disk(
        "R:", myrd::product::ServiceAggregate::blocked_needs_attention,
        myrd::product::ServiceRuntime::running, myrd::product::ServiceHealth::healthy, 7,
        1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL));

    REQUIRE_EQ(row.verdict, std::string("blocked/needs-attention"));
    REQUIRE_EQ(row.runtime, std::string("Running"));
    REQUIRE_EQ(row.needs_attention, true);
}

TEST_CASE(m5_gui_a_healthy_running_disk_needs_no_attention) {
    const auto row = myrd::gui::make_disk_row(disk(
        "R:", myrd::product::ServiceAggregate::running,
        myrd::product::ServiceRuntime::running, myrd::product::ServiceHealth::healthy, 3,
        2ULL * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL));
    REQUIRE_EQ(row.verdict, std::string("running"));
    REQUIRE_EQ(row.needs_attention, false);
}

TEST_CASE(m5_gui_a_failed_disk_still_shows_its_verdict) {
    // `failed` is a verdict, not an exception: the list shows it rather than dropping the row.
    const auto row = myrd::gui::make_disk_row(disk(
        "R:", myrd::product::ServiceAggregate::failed,
        myrd::product::ServiceRuntime::stopped, myrd::product::ServiceHealth::fatal, 1, 0, 0));
    REQUIRE_EQ(row.verdict, std::string("failed"));
    REQUIRE_EQ(row.needs_attention, true);
}

TEST_CASE(m5_gui_memory_reads_in_mebibytes_and_absence_is_not_zero) {
    REQUIRE_EQ(myrd::gui::format_memory(19'660'800ULL, 1'073'741'824ULL),
               std::string("18.7 MiB / 1024.0 MiB"));
    REQUIRE_EQ(myrd::gui::format_memory(0, 0), std::string("not running"));
    REQUIRE_EQ(myrd::gui::format_memory(1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL),
               std::string("1.0 MiB / 64.0 MiB"));
}

TEST_CASE(m5_gui_the_tray_says_when_to_look) {
    const auto calm = myrd::gui::make_disk_row(disk(
        "R:", myrd::product::ServiceAggregate::running,
        myrd::product::ServiceRuntime::running, myrd::product::ServiceHealth::healthy, 2, 0, 0));
    REQUIRE_EQ(myrd::gui::tray_summary({calm}), std::string("MyRamDisk: 1 disk"));

    const auto alarmed = myrd::gui::make_disk_row(disk(
        "S:", myrd::product::ServiceAggregate::blocked_needs_attention,
        myrd::product::ServiceRuntime::running, myrd::product::ServiceHealth::healthy, 2, 0, 0));
    REQUIRE_EQ(myrd::gui::tray_summary({calm, alarmed}),
               std::string("MyRamDisk: 2 disks, 1 needs attention"));

    REQUIRE_EQ(myrd::gui::tray_summary({}), std::string("MyRamDisk: no disks"));
}
