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

// ---- Progress and the three confirmations (T3-03, T3-04) --------------------

// What the window shows while an operation runs.  Derived from the operation's state and phase,
// never from the window's own guess about how long something should take: a progress line that
// advances on its own would disagree with the terminal state the service reports.
std::string progress_text(product::ServiceOperationState state, const std::string& phase);

// Terminal states end the progress line; a queued or running operation keeps it alive.
bool operation_finished(product::ServiceOperationState state);

// Which confirmation a destructive command needs, if any.
enum class ConfirmationKind {
    none,
    // `disk stop --force`: the checkpoint could not be confirmed, so the writes in memory are
    // about to be discarded.
    volatile_data_loss,
    // `disk forget`: the profile's identity is about to be forgotten, and the command requires the
    // identifier echoed back so that it cannot be run by accident or by a stale script.
    forget,
};

struct ConfirmationRequest final {
    ConfirmationKind kind{ConfirmationKind::none};
    // The sentence a user reads.  For `forget` it names the identifier the user has to confirm.
    std::string text;
};

// The confirmation a stop needs.  `checkpoint_confirmed` is the service's answer, not the window's
// assumption: a stop whose final checkpoint failed is the one that loses data.
ConfirmationRequest stop_confirmation(bool checkpoint_confirmed,
                                      const std::string& mount);

ConfirmationRequest forget_confirmation(const std::string& mount,
                                       const std::string& profile_id);

// **Why a command was refused, in the words a user needs.**  `authorization_denied` is the one that
// gets an instruction rather than a sentence: an unprivileged window can read everything and change
// nothing, and "access denied" alone does not tell a user what to do about it.
std::string explain_refusal(std::uint32_t native_code, const std::string& service_message);

bool needs_elevation(std::uint32_t native_code);
// One line of text for the tray tooltip: the count and whether anything needs attention.  A
// tray icon exists to say "look at me" only when there is something to look at.
std::string tray_summary(const std::vector<DiskRow>& rows);

} // namespace myrd::gui
