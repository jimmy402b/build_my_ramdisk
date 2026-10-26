#pragma once

// **The GUI's only door to the service.**
//
// It speaks the same wire format the CLI does -- both link `myrd_product_client` -- and it links
// neither the service nor the storage stack (T3-01's closure check enforces that).  Everything
// here returns client view types: no snapshot, no bundle path, no journal.
//
// The class is deliberately thin.  It answers one question ("what does the service say about its
// disks?") and returns either the answer or a sentence a user can read; the window decides how to
// show it.

#include "src/product/service_view.h"

#include <chrono>
#include <string>
#include <vector>

namespace myrd::gui {

struct DiskSnapshot final {
    std::vector<product::ServiceDiskView> disks;
    // Present exactly when the service could not be asked.  A GUI that showed an empty list for
    // "the service is not running" would be telling the user they have no disks.
    std::string unavailable_reason;
};

// Ask the service for its disks.  Never throws; a service that is not there is a normal
// condition for a tray application that starts before the service does.
DiskSnapshot load_disks(std::chrono::milliseconds budget =
                            std::chrono::milliseconds{3000});

} // namespace myrd::gui
