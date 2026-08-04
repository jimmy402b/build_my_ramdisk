#include "sha256_stream.h"

#include <new>
#include <string>
#include <utility>

namespace myrd {
namespace {

constexpr std::uint32_t code(Sha256StreamErrorCode error) noexcept {
    return static_cast<std::uint32_t>(error);
}

Error make_error(ErrorDomain domain, std::uint32_t code,
                 const wchar_t* operation, const wchar_t* message,
                 NTSTATUS native_code = 0) {
    return {domain, code, operation, message,
            static_cast<std::uint32_t>(native_code)};
}

Result<void> property_error(const wchar_t* operation, NTSTATUS status) {
    return Result<void>::failure(make_error(
        ErrorDomain::ntstatus,
        code(Sha256StreamErrorCode::initialization_failed), operation,
        L"BCrypt property query failed", status));
}

class AlgorithmHandleGuard final {
public:
    explicit AlgorithmHandleGuard(BCRYPT_ALG_HANDLE handle) noexcept
        : handle_(handle) {}
    ~AlgorithmHandleGuard() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    BCRYPT_ALG_HANDLE release() noexcept {
        const auto handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    BCRYPT_ALG_HANDLE handle_{};
};

class HashHandleGuard final {
public:
    explicit HashHandleGuard(BCRYPT_HASH_HANDLE handle) noexcept
        : handle_(handle) {}
    ~HashHandleGuard() {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }
    BCRYPT_HASH_HANDLE release() noexcept {
        const auto handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    BCRYPT_HASH_HANDLE handle_{};
};

}  // namespace

Sha256Stream::Sha256Stream()
    : hash_update_error_(make_error(
          ErrorDomain::ntstatus,
          code(Sha256StreamErrorCode::hash_update_failed),
          L"BCryptHashData", L"failed to update SHA-256 hash")),
      hash_finalize_error_(make_error(
          ErrorDomain::ntstatus,
          code(Sha256StreamErrorCode::hash_finalize_failed),
          L"BCryptFinishHash", L"failed to finalize SHA-256 hash")) {
    auto result = initialize();
    if (!result.ok()) {
        state_.fail(result.error());
    }
}

Sha256Stream::~Sha256Stream() {
    if (hash_ != nullptr) {
        BCryptDestroyHash(hash_);
    }
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
}

Result<void> Sha256Stream::initialize() {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return Result<void>::failure(make_error(
            ErrorDomain::ntstatus,
            code(Sha256StreamErrorCode::initialization_failed),
            L"BCryptOpenAlgorithmProvider", L"failed to open SHA-256",
            status));
    }
    AlgorithmHandleGuard algorithm_guard(algorithm);

    ULONG object_length = 0;
    ULONG bytes_returned = 0;
    status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
        &bytes_returned, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return property_error(L"BCryptGetProperty(object length)", status);
    }
    if (bytes_returned != sizeof(object_length) || object_length == 0) {
        return Result<void>::failure(make_error(
            ErrorDomain::internal,
            code(Sha256StreamErrorCode::invalid_hash_length), L"Sha256Stream",
            L"BCrypt returned an invalid hash object length"));
    }

    ULONG hash_length = 0;
    bytes_returned = 0;
    status = BCryptGetProperty(
        algorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length),
        &bytes_returned, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return property_error(L"BCryptGetProperty(hash length)", status);
    }
    constexpr ULONG kSha256DigestBytes = 32;
    if (bytes_returned != sizeof(hash_length) ||
        hash_length != kSha256DigestBytes) {
        return Result<void>::failure(make_error(
            ErrorDomain::internal,
            code(Sha256StreamErrorCode::invalid_hash_length), L"Sha256Stream",
            L"BCrypt returned an invalid SHA-256 hash length"));
    }

    try {
        hash_object_.resize(object_length);
        digest_.resize(hash_length);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorDomain::internal,
            code(Sha256StreamErrorCode::allocation_failed),
            L"Sha256Stream", L"failed to allocate BCrypt hash buffers"));
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(
        algorithm, &hash, hash_object_.data(), object_length, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return Result<void>::failure(make_error(
            ErrorDomain::ntstatus,
            code(Sha256StreamErrorCode::initialization_failed),
            L"BCryptCreateHash",
            L"failed to create SHA-256 hash", status));
    }
    HashHandleGuard hash_guard(hash);
    algorithm_ = algorithm_guard.release();
    hash_ = hash_guard.release();
    return Result<void>::success();
}

Result<void> Sha256Stream::update(std::span<const std::byte> data) {
    return state_.update(data, 0, [this](std::span<const std::byte> chunk) {
        const auto status = BCryptHashData(
            hash_, reinterpret_cast<PUCHAR>(
                const_cast<std::byte*>(chunk.data())),
            static_cast<ULONG>(chunk.size()), 0);
        if (!BCRYPT_SUCCESS(status)) {
            hash_update_error_.native_code =
                static_cast<std::uint32_t>(status);
            return Result<void>::failure(std::move(hash_update_error_));
        }
        return Result<void>::success();
    });
}

Result<std::string> Sha256Stream::finalize() {
    return state_.finalize([this]() -> Result<std::string> {
        std::string output;
        try {
            output.resize(digest_.size() * 2U);
        } catch (const std::bad_alloc&) {
            return Result<std::string>::failure(make_error(
                ErrorDomain::internal,
                code(Sha256StreamErrorCode::allocation_failed),
                L"Sha256Stream::finalize",
                L"failed to allocate SHA-256 hexadecimal output"));
        }

        const auto status = BCryptFinishHash(
            hash_, digest_.data(), static_cast<ULONG>(digest_.size()), 0);
        if (!BCRYPT_SUCCESS(status)) {
            hash_finalize_error_.native_code =
                static_cast<std::uint32_t>(status);
            return Result<std::string>::failure(
                std::move(hash_finalize_error_));
        }

        constexpr char kHex[] = "0123456789ABCDEF";
        for (std::size_t index = 0; index < digest_.size(); ++index) {
            const auto byte = static_cast<unsigned>(digest_[index]);
            output[index * 2U] = kHex[(byte >> 4U) & 0x0FU];
            output[index * 2U + 1U] = kHex[byte & 0x0FU];
        }
        return Result<std::string>::success(std::move(output));
    });
}

}  // namespace myrd
