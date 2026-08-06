#include "src/m3/m3_evidence_publisher.h"
#include "src/m3/m3_report.h"

#include <filesystem>
#include <string>

int wmain(int argc, wchar_t** argv) {
    using namespace myrd::m3;
    if (argc != 2) return 2;
    M3Report report{};
    report.operation = ReportOperation::VerifyFull;
    report.status = ReportStatus::Passed;
    report.operation_id = "fixture-1";
    report.started_utc = "2026-08-06T00:00:00.000Z";
    report.finished_utc = "2026-08-06T00:00:01.000Z";
    report.build = {
        "0123456789abcdef0123456789abcdef01234567", "Debug",
        std::string(64, 'A')};
    report.bundle_path_hash = std::string(64, 'B');
    report.image = ImageEvidence{
        "00112233445566778899AABBCCDDEEFF", 67108864, 512, 4096};
    report.slots[0] = {"A", SlotStatus::Selected, 7, std::nullopt,
                       "passed", true};
    report.slots[1] = {"B", SlotStatus::Empty, std::nullopt,
                       "slot_decode", "missing", false};
    report.selected = SelectedGenerationEvidence{
        0, false, 7, 3, 4, "bwi", "bfi", "jwi", "jfi", 12288,
        {4096, 4096, 1},
        {8192, 4096, 2, std::string(64, 'C')}};
    report.verification = VerificationEvidence{
        VerificationDepth::Full, true, 0, 0, 0, 0,
        std::string(64, 'D')};
    report.artifacts.push_back({"manifest", std::string(64, 'E')});
    auto json = serialize_m3_report(report);
    if (!json.ok()) return 3;
    const auto published = publish_m3_evidence(
        myrd::OperationContext{}, std::filesystem::path(argv[1]),
        json.value(), {});
    return published.published ? 0 : 4;
}
