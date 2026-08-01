#include "sha256.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace myrd {

Result<std::string> sha256_hex(std::span<const std::byte> data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
        nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) {
        return Result<std::string>::failure(
            {ErrorDomain::internal, static_cast<std::uint32_t>(st),
             L"BCryptOpenAlgorithmProvider", L"failed to open SHA-256"});
    }

    DWORD objLen = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbData, 0);
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&cbData), sizeof(cbData), &cbData, 0);

    std::vector<UCHAR> hashObj(objLen);
    std::vector<UCHAR> hash(cbData);

    BCRYPT_HASH_HANDLE hHash = nullptr;
    st = BCryptCreateHash(hAlg, &hHash, hashObj.data(),
        static_cast<ULONG>(hashObj.size()), nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(st)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return Result<std::string>::failure(
            {ErrorDomain::internal, static_cast<std::uint32_t>(st),
             L"BCryptCreateHash", L"failed"});
    }

    st = BCryptHashData(hHash,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())),
        static_cast<ULONG>(data.size()), 0);

    if (BCRYPT_SUCCESS(st)) {
        BCryptFinishHash(hHash, hash.data(), static_cast<ULONG>(hash.size()), 0);
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!BCRYPT_SUCCESS(st)) {
        return Result<std::string>::failure(
            {ErrorDomain::internal, static_cast<std::uint32_t>(st),
             L"BCryptHashData", L"hash failed"});
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (auto b : hash) {
        oss << std::setw(2) << static_cast<unsigned>(b);
    }
    return Result<std::string>::success(oss.str());
}

} // namespace myrd
