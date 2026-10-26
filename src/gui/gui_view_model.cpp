#include "src/gui/gui_view_model.h"

#include "src/product/service_status.h"

#include <cstdint>
#include <string>

namespace myrd::gui {
namespace {

// Binary megabytes with one decimal: a ramdisk's resident set is read at that scale, and a byte
// count is a number nobody converts in their head.
std::string format_mebibytes(const std::uint64_t bytes) {
    constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;
    const std::uint64_t whole = bytes / kMebibyte;
    const std::uint64_t tenths = ((bytes % kMebibyte) * 10ULL) / kMebibyte;
    return std::to_string(whole) + "." + std::to_string(tenths) + " MiB";
}

} // namespace

std::string format_memory(const std::uint64_t resident_bytes,
                          const std::uint64_t budget_bytes) {
    if (budget_bytes == 0) {
        // A stopped disk has no budget to compare against; saying "0.0 / 0.0 MiB" would read as
        // a measurement rather than as an absence.
        return "not running";
    }
    return format_mebibytes(resident_bytes) + " / " + format_mebibytes(budget_bytes);
}

DiskRow make_disk_row(const product::ServiceDiskView& disk) {
    DiskRow row{};
    row.profile_id = disk.profile_id.to_string();
    row.mount = disk.mount_target.has_value() ? disk.mount_target.value() : std::string("(no mount)");
    row.verdict = std::string(product::to_string(disk.aggregate));
    row.runtime = std::string(product::to_string(disk.runtime));
    row.persistence = std::string(product::to_string(disk.persistence));
    row.generation = "generation " + std::to_string(disk.confirmed_generation);
    row.memory = format_memory(disk.resident_bytes, disk.budget_bytes);
    // Attention is the verdict's own answer.  `blocked_needs_attention` and `failed` are the two
    // verdicts that mean a human is needed; `degraded`-style states are not invented here because
    // the service does not have them.
    row.needs_attention = disk.aggregate == product::ServiceAggregate::blocked_needs_attention
        || disk.aggregate == product::ServiceAggregate::failed;
    return row;
}

std::vector<DiskRow> make_disk_rows(const std::vector<product::ServiceDiskView>& disks) {
    std::vector<DiskRow> rows;
    rows.reserve(disks.size());
    for (const auto& disk : disks) {
        rows.push_back(make_disk_row(disk));
    }
    return rows;
}

std::string progress_text(const product::ServiceOperationState state,
                          const std::string& phase) {
    if (operation_finished(state)) {
        return "finished: " + std::string(product::to_string(state));
    }
    if (phase.empty()) {
        return std::string(product::to_string(state)) + "...";
    }
    return std::string(product::to_string(state)) + ": " + phase;
}

bool operation_finished(const product::ServiceOperationState state) {
    return state == product::ServiceOperationState::succeeded
        || state == product::ServiceOperationState::failed
        || state == product::ServiceOperationState::cancelled;
}

ConfirmationRequest stop_confirmation(const bool checkpoint_confirmed,
                                      const std::string& mount) {
    ConfirmationRequest request{};
    if (checkpoint_confirmed) {
        return request;
    }
    request.kind = ConfirmationKind::volatile_data_loss;
    request.text = "The checkpoint for " + mount +
        " could not be confirmed, so stopping it will discard the writes that are still in "
        "memory. Stop anyway?";
    return request;
}

ConfirmationRequest forget_confirmation(const std::string& mount,
                                        const std::string& profile_id) {
    ConfirmationRequest request{};
    request.kind = ConfirmationKind::forget;
    request.text = "Forgetting " + mount + " removes this profile from the service. The identifier "
        "below has to match, so that a stale script cannot do it by accident:\n\n" + profile_id;
    return request;
}

bool needs_elevation(const std::uint32_t native_code) {
    return native_code
        == static_cast<std::uint32_t>(product::ServiceCoreError::authorization_denied);
}

std::string explain_refusal(const std::uint32_t native_code,
                            const std::string& service_message) {
    if (needs_elevation(native_code)) {
        return "This window cannot change anything without administrator rights. "
               "Restart it as administrator to make changes; reading works without.";
    }
    if (service_message.empty()) {
        return "The service refused the command.";
    }
    return service_message;
}
std::string tray_summary(const std::vector<DiskRow>& rows) {
    std::size_t attention = 0;
    for (const auto& row : rows) {
        if (row.needs_attention) ++attention;
    }
    const std::string disks = rows.size() == 1 ? "1 disk" : std::to_string(rows.size()) + " disks";
    if (rows.empty()) return "MyRamDisk: no disks";
    if (attention == 0) return "MyRamDisk: " + disks;
    return "MyRamDisk: " + disks + ", " + std::to_string(attention) + " needs attention";
}

} // namespace myrd::gui
