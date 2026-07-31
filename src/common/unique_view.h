#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace myrd {

class UniqueView final {
public:
    UniqueView() noexcept = default;

    explicit UniqueView(void* view) noexcept : view_(view) {}

    ~UniqueView() noexcept { unmap(); }

    UniqueView(UniqueView&& other) noexcept
        : view_(other.release()) {}

    UniqueView& operator=(UniqueView&& other) noexcept {
        if (this != &other) {
            unmap();
            view_ = other.release();
        }
        return *this;
    }

    UniqueView(const UniqueView&) = delete;
    UniqueView& operator=(const UniqueView&) = delete;

    void* get() const noexcept { return view_; }

    void* release() noexcept {
        void* v = view_;
        view_ = nullptr;
        return v;
    }

    void reset(void* replacement = nullptr) noexcept {
        unmap();
        view_ = replacement;
    }

    explicit operator bool() const noexcept { return view_ != nullptr; }

private:
    void unmap() noexcept {
        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
    }

    void* view_ = nullptr;
};

} // namespace myrd
