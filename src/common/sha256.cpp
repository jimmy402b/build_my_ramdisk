#include "sha256.h"

#include "sha256_stream.h"

namespace myrd {

Result<Sha256Digest> sha256_digest(std::span<const std::byte> data) {
    Sha256Stream stream;
    auto update_result = stream.update(data);
    if (!update_result.ok()) {
        return Result<Sha256Digest>::failure(update_result.error());
    }
    return stream.finalize_digest();
}

Result<std::string> sha256_hex(std::span<const std::byte> data) {
    Sha256Stream stream;
    auto update_result = stream.update(data);
    if (!update_result.ok()) {
        return Result<std::string>::failure(update_result.error());
    }
    return stream.finalize();
}

}  // namespace myrd
