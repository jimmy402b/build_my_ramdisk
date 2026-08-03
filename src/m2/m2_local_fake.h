#pragma once

#include "m2_report.h"
#include "src/common/result.h"

#include <filesystem>
#include <string_view>

namespace myrd {

struct M2LocalFakeRun {
    M2Report report;
    std::filesystem::path report_path;
};

// Runs the production lifecycle orchestrator against deterministic, stateful
// local adapters. This validates orchestration and evidence publication only;
// it never creates an operating-system disk or touches the AIM driver.
Result<M2LocalFakeRun> run_m2_local_fake(
    const std::filesystem::path& evidence_directory,
    std::string_view run_id);

} // namespace myrd
