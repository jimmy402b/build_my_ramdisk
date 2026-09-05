#include "tests/support/m4_fake_worker.h"

#include <algorithm>
#include <utility>

namespace myrd::tests {

bool FakeWorkerControl::begins_noncancellable(
    management::ActionKind action) const noexcept {
    return std::find(noncancellable_.begin(), noncancellable_.end(), action)
        != noncancellable_.end();
}

Result<management::WorkerActionResult> FakeWorkerControl::execute(
    const management::WorkerActionRequest& request) {
    calls.push_back(request);
    if (after_execute_) after_execute_(calls.size());
    if (failure_.has_value()) {
        return Result<management::WorkerActionResult>::failure(
            failure_.value());
    }
    return Result<management::WorkerActionResult>::success(result_);
}

void FakeWorkerControl::set_result(
    management::WorkerActionResult result) {
    result_ = std::move(result);
    failure_.reset();
}

void FakeWorkerControl::set_failure(Error error) {
    failure_ = std::move(error);
}

void FakeWorkerControl::add_noncancellable_boundary(
    management::ActionKind action) {
    noncancellable_.push_back(action);
}

void FakeWorkerControl::set_after_execute(
    std::function<void(std::size_t)> callback) {
    after_execute_ = std::move(callback);
}

} // namespace myrd::tests
