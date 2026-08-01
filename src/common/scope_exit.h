#pragma once

#include <type_traits>
#include <utility>

namespace myrd {

template <class F>
class ScopeExit final {
public:
    explicit ScopeExit(F&& func) noexcept(std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(func)), active_(true) {}

    ScopeExit(ScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : func_(std::move(other.func_)), active_(other.active_) {
        other.release();
    }

    ScopeExit& operator=(ScopeExit&&) = delete;
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ~ScopeExit() noexcept(noexcept(std::declval<F>()())) {
        if (active_) {
            func_();
        }
    }

    void release() noexcept { active_ = false; }

private:
    F func_;
    bool active_;
};

template <class F>
ScopeExit(F) -> ScopeExit<F>;

} // namespace myrd
