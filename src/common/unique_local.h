#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace myrd {

class UniqueLocal final {
public:
    UniqueLocal() noexcept = default;

    explicit UniqueLocal(void* ptr) noexcept : ptr_(ptr) {}

    ~UniqueLocal() noexcept { free(); }

    UniqueLocal(UniqueLocal&& other) noexcept
        : ptr_(other.release()) {}

    UniqueLocal& operator=(UniqueLocal&& other) noexcept {
        if (this != &other) {
            free();
            ptr_ = other.release();
        }
        return *this;
    }

    UniqueLocal(const UniqueLocal&) = delete;
    UniqueLocal& operator=(const UniqueLocal&) = delete;

    void* get() const noexcept { return ptr_; }

    void* release() noexcept {
        void* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

    void reset(void* replacement = nullptr) noexcept {
        free();
        ptr_ = replacement;
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    void free() noexcept {
        if (ptr_) {
            LocalFree(ptr_);
            ptr_ = nullptr;
        }
    }

    void* ptr_ = nullptr;
};

} // namespace myrd
