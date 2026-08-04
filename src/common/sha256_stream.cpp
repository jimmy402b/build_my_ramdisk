/// @file sha256_stream.cpp
/// @brief Streaming SHA-256 — accumulates then delegates to one-shot sha256_hex().

#include "sha256_stream.h"
#include "sha256.h"

namespace myrd {

Result<void> Sha256Stream::update(std::span<const std::byte> data) {
    if (finalized_) {
        return Result<void>::failure(
            {ErrorDomain::internal, 0xE001,
             L"Sha256Stream", L"update after finalize"});
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    return Result<void>::success();
}

Result<std::string> Sha256Stream::finalize() {
    if (finalized_) {
        return Result<std::string>::failure(
            {ErrorDomain::internal, 0xE003,
             L"Sha256Stream", L"already finalized"});
    }
    finalized_ = true;
    return sha256_hex(buffer_);
}

}  // namespace myrd
