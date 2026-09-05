#pragma once

#include "src/management/service_core.h"

#include <functional>
#include <optional>
#include <vector>

namespace myrd::tests {

class FakeWorkerControl final : public management::IWorkerControl {
public:
    bool begins_noncancellable(
        management::ActionKind action) const noexcept override;
    Result<management::WorkerActionResult> execute(
        const management::WorkerActionRequest& request) override;

    void set_result(management::WorkerActionResult result);
    void set_failure(Error error);
    void add_noncancellable_boundary(management::ActionKind action);
    void set_after_execute(std::function<void(std::size_t)> callback);

    std::vector<management::WorkerActionRequest> calls;

private:
    management::WorkerActionResult result_{};
    std::optional<Error> failure_;
    std::vector<management::ActionKind> noncancellable_;
    std::function<void(std::size_t)> after_execute_;
};

} // namespace myrd::tests
