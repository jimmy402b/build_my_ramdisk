#pragma once

// **The GUI's view model, with no Windows in it.**
//
// T3-02 says the window shows disks and memory, and that `aggregate` is the *only* verdict the
// user is shown.  Both of those are decisions worth testing without a message pump: a verdict
// computed from `runtime` and `health` in a hurry would show a blocked disk as fine, and the
// failure would only appear in front of a user.
//
// So the formatting lives here, pure functions over the client view types, and `gui_main.cpp`
// does nothing but put them in a list.

#include "src/product/service_view.h"

#include <string>
#include <vector>

namespace myrd::gui {

// One row of the disk list.  Kept as named fields rather than a formatted string so the list can
// size its columns and a test can assert a field without parsing text.
struct DiskRow final {
    std::string profile_id;
    std::string mount;
    // **The verdict, and only the verdict.**  From `ServiceDiskView::aggregate`; the runtime and
    // health columns are shown beside it as facts, never composed into a second opinion.
    std::string verdict;
    std::string runtime;
    std::string persistence;
    std::string generation;
    std::string memory;
    bool needs_attention{};
};

DiskRow make_disk_row(const product::ServiceDiskView& disk);

std::vector<DiskRow> make_disk_rows(const std::vector<product::ServiceDiskView>& disks);

// The memory column: resident against budget, in the units a user thinks in.  `resident` is RAM
// the runtime holds now; `budget` is what the disk was opened with.
std::string format_memory(std::uint64_t resident_bytes, std::uint64_t budget_bytes);

// One line of text for the tray tooltip: the count and whether anything needs attention.  A
// tray icon exists to say "look at me" only when there is something to look at.
std::string tray_summary(const std::vector<DiskRow>& rows);

} // namespace myrd::gui
