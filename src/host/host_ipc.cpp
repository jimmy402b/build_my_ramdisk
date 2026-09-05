#include "src/host/host_ipc.h"

#include "src/common/sha256.h"
#include "src/control/authorization.h"
#include "src/profile/profile_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <AclAPI.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace myrd::host {
namespace {

inline constexpr std::array<std::byte, 8> kStartupMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'S'}, std::byte{'E'}, std::byte{'C'}, std::byte{0}};
inline constexpr std::uint16_t kStartupSchemaMajor = 1;
inline constexpr std::uint16_t kStartupSchemaMinor = 0;
inline constexpr std::size_t kCanonicalUuidBytes = 36;
inline constexpr std::size_t kStartupPrefixBytes = 160;
inline constexpr std::size_t kStartupRecordBytes =
    kStartupPrefixBytes + kAuthenticationTagBytes;

inline constexpr std::array<std::byte, 8> kLeaseMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'L'}, std::byte{'E'}, std::byte{'A'}, std::byte{'S'}};
inline constexpr std::array<std::byte, 8> kReattachmentMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'R'}, std::byte{'T'}, std::byte{'A'}, std::byte{0}};
inline constexpr std::uint16_t kLeaseSchemaMajor = 1;
inline constexpr std::uint16_t kLeaseSchemaMinor = 0;
inline constexpr std::size_t kBindingBytes = 112;
inline constexpr std::size_t kLeaseHeaderBytes = 140;
inline constexpr std::size_t kReattachmentPlaintextBytes = 164;
inline constexpr std::wstring_view kDpapiDescription =
    L"MyRamDisk runtime reattachment v1";

template <class T>
Result<T> ipc_error(
    HostProtocolError code,
    const wchar_t* operation,
    const wchar_t* message,
    std::uint32_t native_code = 0) {
    return Result<T>::failure(Error{
        native_code == 0 ? ErrorDomain::protocol : ErrorDomain::win32,
        static_cast<std::uint32_t>(code),
        operation,
        message,
        native_code});
}

void secure_zero(std::span<std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        static_cast<void>(SecureZeroMemory(bytes.data(), bytes.size()));
    }
}

class HandleOwner final {
public:
    explicit HandleOwner(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~HandleOwner() noexcept { reset(); }
    HandleOwner(HandleOwner&& other) noexcept : handle_(other.release()) {}
    HandleOwner& operator=(HandleOwner&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;

    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept {
        const auto value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_{};
};

void append_u16(
    std::array<std::byte, kStartupRecordBytes>& output,
    std::size_t& offset,
    std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset++] = static_cast<std::byte>(value & 0xffU);
        value >>= 8U;
    }
}

void append_u32(
    std::array<std::byte, kStartupRecordBytes>& output,
    std::size_t& offset,
    std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset++] = static_cast<std::byte>(value & 0xffU);
        value >>= 8U;
    }
}

void append_u64(
    std::array<std::byte, kStartupRecordBytes>& output,
    std::size_t& offset,
    std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset++] = static_cast<std::byte>(value & 0xffU);
        value >>= 8U;
    }
}

template <class T>
T read_le(
    const std::array<std::byte, kStartupRecordBytes>& input,
    std::size_t& offset) noexcept {
    static_assert(std::is_unsigned_v<T>);
    T value{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned char>(
            input[offset++])) << (index * 8U);
    }
    return value;
}

bool append_uuid_text(
    std::array<std::byte, kStartupRecordBytes>& output,
    std::size_t& offset,
    const product::Uuid& id) {
    const auto text = id.to_string();
    if (text.size() != kCanonicalUuidBytes) return false;
    for (const auto value : text) {
        output[offset++] = static_cast<std::byte>(
            static_cast<unsigned char>(value));
    }
    return true;
}

Result<product::Uuid> read_uuid_text(
    const std::array<std::byte, kStartupRecordBytes>& input,
    std::size_t& offset) {
    std::array<char, kCanonicalUuidBytes> text{};
    for (auto& value : text) {
        value = static_cast<char>(std::to_integer<unsigned char>(
            input[offset++]));
    }
    return product::Uuid::parse(
        std::string_view(text.data(), text.size()));
}

Result<std::array<std::byte, kStartupRecordBytes>> encode_startup_record(
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    if (!is_valid(binding)) {
        return ipc_error<std::array<std::byte, kStartupRecordBytes>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_encode",
            L"Startup secret binding is invalid");
    }
    std::array<std::byte, kStartupRecordBytes> record{};
    std::size_t offset = 0;
    std::copy(kStartupMagic.begin(), kStartupMagic.end(), record.begin());
    offset += kStartupMagic.size();
    append_u16(record, offset, kStartupSchemaMajor);
    append_u16(record, offset, kStartupSchemaMinor);
    append_u32(
        record, offset, static_cast<std::uint32_t>(record.size()));
    try {
        if (!append_uuid_text(record, offset, binding.profile_id)
            || !append_uuid_text(record, offset, binding.image_id)) {
            secure_zero(record);
            return ipc_error<std::array<std::byte, kStartupRecordBytes>>(
                HostProtocolError::invalid_argument,
                L"host_ipc.startup_encode",
                L"Startup UUID encoding is invalid");
        }
    } catch (...) {
        secure_zero(record);
        return ipc_error<std::array<std::byte, kStartupRecordBytes>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_encode",
            L"Failed to encode bounded startup UUIDs");
    }
    append_u32(record, offset, binding.worker_pid);
    append_u64(record, offset, binding.process_creation_time);
    append_u64(record, offset, binding.session_generation);
    append_u64(record, offset, binding.profile_revision);
    append_u32(record, offset, binding.product_major);
    append_u32(record, offset, binding.product_minor);
    append_u32(record, offset, binding.product_patch);
    const auto secret_view = secret.bytes();
    std::copy(secret_view.begin(), secret_view.end(), record.begin() + offset);
    offset += secret_view.size();
    if (offset != kStartupPrefixBytes) {
        secure_zero(record);
        return ipc_error<std::array<std::byte, kStartupRecordBytes>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_encode",
            L"Startup record layout invariant failed");
    }
    auto digest = sha256_digest(std::span(record).first(offset));
    if (!digest.ok()) {
        secure_zero(record);
        return Result<std::array<std::byte, kStartupRecordBytes>>::failure(
            digest.error());
    }
    std::copy(digest.value().begin(), digest.value().end(),
              record.begin() + offset);
    secure_zero(digest.value());
    auto wrapped =
        Result<std::array<std::byte, kStartupRecordBytes>>::success(record);
    secure_zero(record);
    return wrapped;
}

bool digest_equal(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) return false;
    std::byte difference{};
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == std::byte{};
}

template <class T>
void append_vector_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffU));
        value >>= 8U;
    }
}

template <class T>
Result<T> read_span_le(
    std::span<const std::byte> input,
    std::size_t& offset,
    const wchar_t* operation) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || input.size() - offset < sizeof(T)) {
        return ipc_error<T>(
            HostProtocolError::lease_malformed,
            operation,
            L"Runtime lease integer is truncated");
    }
    T value{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned char>(
            input[offset++])) << (index * 8U);
    }
    return Result<T>::success(value);
}

Result<void> append_binding_vector(
    std::vector<std::byte>& output,
    const WorkerBinding& binding,
    const wchar_t* operation) {
    try {
        const auto profile = binding.profile_id.to_string();
        const auto image = binding.image_id.to_string();
        if (profile.size() != kCanonicalUuidBytes
            || image.size() != kCanonicalUuidBytes) {
            return ipc_error<void>(
                HostProtocolError::lease_malformed,
                operation,
                L"Runtime lease UUID encoding is invalid");
        }
        for (const auto value : profile) {
            output.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(value)));
        }
        for (const auto value : image) {
            output.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(value)));
        }
        append_vector_le(output, binding.worker_pid);
        append_vector_le(output, binding.process_creation_time);
        append_vector_le(output, binding.session_generation);
        append_vector_le(output, binding.profile_revision);
        append_vector_le(output, binding.product_major);
        append_vector_le(output, binding.product_minor);
        append_vector_le(output, binding.product_patch);
        return Result<void>::success();
    } catch (...) {
        return ipc_error<void>(
            HostProtocolError::lease_malformed,
            operation,
            L"Failed to allocate bounded runtime lease binding");
    }
}

Result<WorkerBinding> read_binding_vector(
    std::span<const std::byte> input,
    std::size_t& offset,
    const wchar_t* operation) {
    if (offset > input.size()
        || input.size() - offset < kBindingBytes) {
        return ipc_error<WorkerBinding>(
            HostProtocolError::lease_malformed,
            operation,
            L"Runtime lease binding is truncated");
    }
    std::array<char, kCanonicalUuidBytes> profile_text{};
    std::array<char, kCanonicalUuidBytes> image_text{};
    for (auto& value : profile_text) {
        value = static_cast<char>(std::to_integer<unsigned char>(
            input[offset++]));
    }
    for (auto& value : image_text) {
        value = static_cast<char>(std::to_integer<unsigned char>(
            input[offset++]));
    }
    auto profile = product::Uuid::parse(
        std::string_view(profile_text.data(), profile_text.size()));
    auto image = product::Uuid::parse(
        std::string_view(image_text.data(), image_text.size()));
    if (!profile.ok() || !image.ok()) {
        return ipc_error<WorkerBinding>(
            HostProtocolError::lease_malformed,
            operation,
            L"Runtime lease UUID binding is malformed");
    }
    auto pid = read_span_le<std::uint32_t>(input, offset, operation);
    auto creation = read_span_le<std::uint64_t>(input, offset, operation);
    auto generation = read_span_le<std::uint64_t>(input, offset, operation);
    auto revision = read_span_le<std::uint64_t>(input, offset, operation);
    auto major = read_span_le<std::uint32_t>(input, offset, operation);
    auto minor = read_span_le<std::uint32_t>(input, offset, operation);
    auto patch = read_span_le<std::uint32_t>(input, offset, operation);
    if (!pid.ok() || !creation.ok() || !generation.ok() || !revision.ok()
        || !major.ok() || !minor.ok() || !patch.ok()) {
        return ipc_error<WorkerBinding>(
            HostProtocolError::lease_malformed,
            operation,
            L"Runtime lease binding fields are truncated");
    }
    WorkerBinding result{
        profile.move_value(), image.move_value(), pid.value(),
        creation.value(), generation.value(), revision.value(),
        major.value(), minor.value(), patch.value()};
    if (!is_valid(result)) {
        return ipc_error<WorkerBinding>(
            HostProtocolError::lease_binding_mismatch,
            operation,
            L"Runtime lease binding is invalid or version-mismatched");
    }
    return Result<WorkerBinding>::success(std::move(result));
}

Result<Sha256Digest> lease_entropy(const WorkerBinding& binding) {
    std::vector<std::byte> entropy_input;
    try {
        constexpr std::string_view domain =
            "MYRD-RUNTIME-LEASE-ENTROPY-v1";
        entropy_input.reserve(domain.size() + 1 + 4 + kBindingBytes);
        for (const auto value : domain) {
            entropy_input.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(value)));
        }
        entropy_input.push_back(std::byte{});
        append_vector_le(
            entropy_input, static_cast<std::uint16_t>(kHostProtocolMajor));
        append_vector_le(
            entropy_input, static_cast<std::uint16_t>(kHostProtocolMinor));
        auto appended = append_binding_vector(
            entropy_input, binding, L"host_ipc.lease_entropy");
        if (!appended.ok()) {
            return Result<Sha256Digest>::failure(appended.error());
        }
    } catch (...) {
        return ipc_error<Sha256Digest>(
            HostProtocolError::lease_protection_failed,
            L"host_ipc.lease_entropy",
            L"Failed to allocate bounded runtime lease entropy");
    }
    return sha256_digest(entropy_input);
}

Result<std::vector<std::byte>> make_reattachment_plaintext(
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    std::vector<std::byte> plaintext;
    try {
        plaintext.reserve(kReattachmentPlaintextBytes);
        plaintext.insert(
            plaintext.end(), kReattachmentMagic.begin(),
            kReattachmentMagic.end());
        append_vector_le(plaintext, kLeaseSchemaMajor);
        append_vector_le(plaintext, kLeaseSchemaMinor);
        append_vector_le(
            plaintext,
            static_cast<std::uint32_t>(kReattachmentPlaintextBytes));
        append_vector_le(
            plaintext, static_cast<std::uint16_t>(kHostProtocolMajor));
        append_vector_le(
            plaintext, static_cast<std::uint16_t>(kHostProtocolMinor));
        auto binding_result = append_binding_vector(
            plaintext, binding, L"host_ipc.lease_plaintext");
        if (!binding_result.ok()) {
            secure_zero(plaintext);
            return Result<std::vector<std::byte>>::failure(
                binding_result.error());
        }
        plaintext.insert(
            plaintext.end(), secret.bytes().begin(), secret.bytes().end());
    } catch (...) {
        secure_zero(plaintext);
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_protection_failed,
            L"host_ipc.lease_plaintext",
            L"Failed to allocate bounded reattachment plaintext");
    }
    if (plaintext.size() != kReattachmentPlaintextBytes) {
        secure_zero(plaintext);
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_protection_failed,
            L"host_ipc.lease_plaintext",
            L"Reattachment plaintext layout invariant failed");
    }
    return Result<std::vector<std::byte>>::success(std::move(plaintext));
}

Result<std::vector<std::byte>> protect_reattachment(
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    auto plaintext = make_reattachment_plaintext(binding, secret);
    if (!plaintext.ok()) return plaintext;
    auto entropy = lease_entropy(binding);
    if (!entropy.ok()) {
        secure_zero(plaintext.value());
        return Result<std::vector<std::byte>>::failure(entropy.error());
    }
    DATA_BLOB input{
        static_cast<DWORD>(plaintext.value().size()),
        reinterpret_cast<BYTE*>(plaintext.value().data())};
    DATA_BLOB entropy_blob{
        static_cast<DWORD>(entropy.value().size()),
        reinterpret_cast<BYTE*>(entropy.value().data())};
    DATA_BLOB output{};
    const BOOL protected_ok = CryptProtectData(
        &input,
        kDpapiDescription.data(),
        &entropy_blob,
        nullptr,
        nullptr,
        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
        &output);
    const auto error = protected_ok ? ERROR_SUCCESS : GetLastError();
    secure_zero(plaintext.value());
    secure_zero(entropy.value());
    if (!protected_ok || output.pbData == nullptr || output.cbData == 0) {
        if (output.pbData != nullptr) static_cast<void>(LocalFree(output.pbData));
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_protection_failed,
            L"host_ipc.lease_protect",
            L"Failed to DPAPI-protect runtime reattachment material",
            error);
    }
    std::vector<std::byte> protected_bytes;
    try {
        protected_bytes.assign(
            reinterpret_cast<std::byte*>(output.pbData),
            reinterpret_cast<std::byte*>(output.pbData) + output.cbData);
    } catch (...) {
        secure_zero(std::span(
            reinterpret_cast<std::byte*>(output.pbData), output.cbData));
        static_cast<void>(LocalFree(output.pbData));
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_protection_failed,
            L"host_ipc.lease_protect",
            L"Failed to allocate bounded DPAPI ciphertext");
    }
    secure_zero(std::span(
        reinterpret_cast<std::byte*>(output.pbData), output.cbData));
    static_cast<void>(LocalFree(output.pbData));
    return Result<std::vector<std::byte>>::success(
        std::move(protected_bytes));
}

Result<SessionSecret> unprotect_reattachment(
    std::span<const std::byte> protected_bytes,
    const WorkerBinding& expected) {
    if (protected_bytes.empty()
        || protected_bytes.size() > kMaximumRuntimeLeaseBytes
        || protected_bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        return ipc_error<SessionSecret>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_unprotect",
            L"Runtime lease ciphertext is outside the fixed bound");
    }
    auto entropy = lease_entropy(expected);
    if (!entropy.ok()) return Result<SessionSecret>::failure(entropy.error());
    DATA_BLOB input{
        static_cast<DWORD>(protected_bytes.size()),
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(
            protected_bytes.data()))};
    DATA_BLOB entropy_blob{
        static_cast<DWORD>(entropy.value().size()),
        reinterpret_cast<BYTE*>(entropy.value().data())};
    DATA_BLOB output{};
    LPWSTR description = nullptr;
    const BOOL unprotected = CryptUnprotectData(
        &input,
        &description,
        &entropy_blob,
        nullptr,
        nullptr,
        CRYPTPROTECT_UI_FORBIDDEN,
        &output);
    const auto error = unprotected ? ERROR_SUCCESS : GetLastError();
    secure_zero(entropy.value());
    const bool description_ok = description != nullptr
        && std::wstring_view(description) == kDpapiDescription;
    if (description != nullptr) static_cast<void>(LocalFree(description));
    if (!unprotected || !description_ok || output.pbData == nullptr
        || output.cbData != kReattachmentPlaintextBytes) {
        if (output.pbData != nullptr) {
            secure_zero(std::span(
                reinterpret_cast<std::byte*>(output.pbData), output.cbData));
            static_cast<void>(LocalFree(output.pbData));
        }
        return ipc_error<SessionSecret>(
            HostProtocolError::lease_protection_failed,
            L"host_ipc.lease_unprotect",
            L"Runtime reattachment material failed DPAPI validation",
            error);
    }
    std::span<const std::byte> plaintext(
        reinterpret_cast<const std::byte*>(output.pbData), output.cbData);
    std::size_t offset = 0;
    bool valid = std::equal(
        kReattachmentMagic.begin(), kReattachmentMagic.end(),
        plaintext.begin());
    offset += kReattachmentMagic.size();
    auto schema_major = read_span_le<std::uint16_t>(
        plaintext, offset, L"host_ipc.lease_unprotect");
    auto schema_minor = read_span_le<std::uint16_t>(
        plaintext, offset, L"host_ipc.lease_unprotect");
    auto total = read_span_le<std::uint32_t>(
        plaintext, offset, L"host_ipc.lease_unprotect");
    auto protocol_major = read_span_le<std::uint16_t>(
        plaintext, offset, L"host_ipc.lease_unprotect");
    auto protocol_minor = read_span_le<std::uint16_t>(
        plaintext, offset, L"host_ipc.lease_unprotect");
    auto binding = read_binding_vector(
        plaintext, offset, L"host_ipc.lease_unprotect");
    valid = valid && schema_major.ok() && schema_minor.ok() && total.ok()
        && protocol_major.ok() && protocol_minor.ok() && binding.ok()
        && schema_major.value() == kLeaseSchemaMajor
        && schema_minor.value() == kLeaseSchemaMinor
        && total.value() == kReattachmentPlaintextBytes
        && protocol_major.value() == kHostProtocolMajor
        && protocol_minor.value() == kHostProtocolMinor
        && binding.value() == expected
        && offset + kSessionSecretBytes == plaintext.size();
    SecretBytes secret_bytes_value{};
    if (valid) {
        std::copy_n(
            plaintext.begin() + offset,
            secret_bytes_value.size(),
            secret_bytes_value.begin());
    }
    secure_zero(std::span(
        reinterpret_cast<std::byte*>(output.pbData), output.cbData));
    static_cast<void>(LocalFree(output.pbData));
    if (!valid) {
        secure_zero(secret_bytes_value);
        return ipc_error<SessionSecret>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.lease_unprotect",
            L"DPAPI plaintext does not match expected worker identity");
    }
    auto secret = SessionSecret::from_bytes(secret_bytes_value);
    secure_zero(secret_bytes_value);
    return secret;
}

Result<StartupSecretMaterial> decode_startup_material_record(
    std::array<std::byte, kStartupRecordBytes>& record) {
    auto clear_record = [&record]() noexcept { secure_zero(record); };
    auto digest = sha256_digest(
        std::span<const std::byte>(record).first(kStartupPrefixBytes));
    if (!digest.ok()) {
        clear_record();
        return Result<StartupSecretMaterial>::failure(digest.error());
    }
    const bool digest_ok = digest_equal(
        digest.value(),
        std::span<const std::byte>(record).subspan(
            kStartupPrefixBytes, kAuthenticationTagBytes));
    secure_zero(digest.value());
    if (!digest_ok) {
        clear_record();
        return ipc_error<StartupSecretMaterial>(
            HostProtocolError::authentication_failed,
            L"host_ipc.startup_decode",
            L"Startup secret record integrity check failed");
    }

    std::size_t offset = 0;
    if (!std::equal(
            kStartupMagic.begin(), kStartupMagic.end(), record.begin())) {
        clear_record();
        return ipc_error<StartupSecretMaterial>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_decode",
            L"Startup secret magic is invalid");
    }
    offset += kStartupMagic.size();
    const auto schema_major = read_le<std::uint16_t>(record, offset);
    const auto schema_minor = read_le<std::uint16_t>(record, offset);
    const auto total_bytes = read_le<std::uint32_t>(record, offset);
    if (schema_major != kStartupSchemaMajor
        || schema_minor != kStartupSchemaMinor
        || total_bytes != record.size()) {
        clear_record();
        return ipc_error<StartupSecretMaterial>(
            HostProtocolError::version_mismatch,
            L"host_ipc.startup_decode",
            L"Startup secret schema or length is invalid");
    }
    auto profile = read_uuid_text(record, offset);
    auto image = read_uuid_text(record, offset);
    if (!profile.ok() || !image.ok()) {
        clear_record();
        return ipc_error<StartupSecretMaterial>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_decode",
            L"Startup secret UUID binding is malformed");
    }
    WorkerBinding actual{
        profile.move_value(),
        image.move_value(),
        read_le<std::uint32_t>(record, offset),
        read_le<std::uint64_t>(record, offset),
        read_le<std::uint64_t>(record, offset),
        read_le<std::uint64_t>(record, offset),
        read_le<std::uint32_t>(record, offset),
        read_le<std::uint32_t>(record, offset),
        read_le<std::uint32_t>(record, offset)};
    if (!is_valid(actual)) {
        clear_record();
        return ipc_error<StartupSecretMaterial>(
            HostProtocolError::binding_mismatch,
            L"host_ipc.startup_decode",
            L"Startup secret contains an invalid worker binding");
    }
    SecretBytes bytes{};
    std::copy_n(record.begin() + offset, bytes.size(), bytes.begin());
    offset += bytes.size();
    if (offset != kStartupPrefixBytes) {
        secure_zero(bytes);
        clear_record();
        return ipc_error<StartupSecretMaterial>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_decode",
            L"Startup secret layout is invalid");
    }
    auto secret = SessionSecret::from_bytes(bytes);
    secure_zero(bytes);
    clear_record();
    if (!secret.ok()) {
        return Result<StartupSecretMaterial>::failure(secret.error());
    }
    return Result<StartupSecretMaterial>::success(
        StartupSecretMaterial(std::move(actual), secret.move_value()));
}

Result<SessionSecret> decode_startup_record(
    std::array<std::byte, kStartupRecordBytes>& record,
    const WorkerBinding& expected) {
    auto material = decode_startup_material_record(record);
    if (!material.ok()) {
        return Result<SessionSecret>::failure(material.error());
    }
    if (material.value().binding != expected) {
        return ipc_error<SessionSecret>(
            HostProtocolError::binding_mismatch,
            L"host_ipc.startup_decode",
            L"Startup secret is bound to another worker");
    }
    return Result<SessionSecret>::success(
        std::move(material.value().secret));
}

} // namespace

struct PrivatePipeSecurityDescriptor::Impl final {
    explicit Impl(PSECURITY_DESCRIPTOR value) noexcept
        : descriptor(value) {
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
    }
    ~Impl() noexcept {
        if (descriptor != nullptr) static_cast<void>(LocalFree(descriptor));
    }

    PSECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};
};

struct RuntimeLeaseSecurityDescriptor::Impl final {
    explicit Impl(PSECURITY_DESCRIPTOR value) noexcept
        : descriptor(value) {
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
    }
    ~Impl() noexcept {
        if (descriptor != nullptr) static_cast<void>(LocalFree(descriptor));
    }

    PSECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};
};

PrivatePipeSecurityDescriptor::PrivatePipeSecurityDescriptor(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PrivatePipeSecurityDescriptor::PrivatePipeSecurityDescriptor(
    PrivatePipeSecurityDescriptor&&) noexcept = default;
PrivatePipeSecurityDescriptor&
PrivatePipeSecurityDescriptor::operator=(
    PrivatePipeSecurityDescriptor&&) noexcept = default;
PrivatePipeSecurityDescriptor::~PrivatePipeSecurityDescriptor() noexcept =
    default;

void* PrivatePipeSecurityDescriptor::native_descriptor() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->descriptor;
}

void* PrivatePipeSecurityDescriptor::native_security_attributes() noexcept {
    return impl_ == nullptr ? nullptr : &impl_->attributes;
}

Result<PrivatePipeSecurityDescriptor>
create_private_pipe_security_descriptor() {
    try {
        static_assert((kPrivatePipeClientAccessMask & 0x00000004U) == 0U);
        static_assert((kPrivatePipeClientAccessMask & 0xF0000000U) == 0U);
        const std::wstring sddl =
            L"O:SYG:SYD:P(A;;FA;;;SY)(A;;0x00100183;;;"
            + std::wstring(ipc::kMyRamDiskServiceSid) + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            const auto error = GetLastError();
            return ipc_error<PrivatePipeSecurityDescriptor>(
                HostProtocolError::invalid_argument,
                L"host_ipc.private_pipe_acl",
                L"Failed to build private pipe security descriptor",
                error);
        }
        auto impl = std::unique_ptr<PrivatePipeSecurityDescriptor::Impl>(
            new (std::nothrow)
                PrivatePipeSecurityDescriptor::Impl(descriptor));
        if (impl == nullptr) {
            static_cast<void>(LocalFree(descriptor));
            return ipc_error<PrivatePipeSecurityDescriptor>(
                HostProtocolError::invalid_argument,
                L"host_ipc.private_pipe_acl",
                L"Failed to allocate private pipe security state");
        }
        return Result<PrivatePipeSecurityDescriptor>::success(
            PrivatePipeSecurityDescriptor(std::move(impl)));
    } catch (...) {
        return ipc_error<PrivatePipeSecurityDescriptor>(
            HostProtocolError::invalid_argument,
            L"host_ipc.private_pipe_acl",
            L"Failed to allocate private pipe security state");
    }
}

RuntimeLeaseSecurityDescriptor::RuntimeLeaseSecurityDescriptor(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RuntimeLeaseSecurityDescriptor::RuntimeLeaseSecurityDescriptor(
    RuntimeLeaseSecurityDescriptor&&) noexcept = default;
RuntimeLeaseSecurityDescriptor&
RuntimeLeaseSecurityDescriptor::operator=(
    RuntimeLeaseSecurityDescriptor&&) noexcept = default;
RuntimeLeaseSecurityDescriptor::~RuntimeLeaseSecurityDescriptor() noexcept =
    default;

void* RuntimeLeaseSecurityDescriptor::native_descriptor() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->descriptor;
}

void* RuntimeLeaseSecurityDescriptor::native_security_attributes() noexcept {
    return impl_ == nullptr ? nullptr : &impl_->attributes;
}

Result<RuntimeLeaseSecurityDescriptor>
create_runtime_lease_security_descriptor() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:SYG:SYD:P(A;;FA;;;SY)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        const auto error = GetLastError();
        return ipc_error<RuntimeLeaseSecurityDescriptor>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_acl",
            L"Failed to build SYSTEM-only runtime lease descriptor",
            error);
    }
    auto impl = std::unique_ptr<RuntimeLeaseSecurityDescriptor::Impl>(
        new (std::nothrow)
            RuntimeLeaseSecurityDescriptor::Impl(descriptor));
    if (impl == nullptr) {
        static_cast<void>(LocalFree(descriptor));
        return ipc_error<RuntimeLeaseSecurityDescriptor>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_acl",
            L"Failed to allocate runtime lease security state");
    }
    return Result<RuntimeLeaseSecurityDescriptor>::success(
        RuntimeLeaseSecurityDescriptor(std::move(impl)));
}

PrivatePipeInstance::PrivatePipeInstance(
    void* handle, std::wstring name) noexcept
    : handle_(handle), name_(std::move(name)) {}

PrivatePipeInstance::~PrivatePipeInstance() noexcept { close(); }

PrivatePipeInstance::PrivatePipeInstance(
    PrivatePipeInstance&& other) noexcept
    : handle_(other.release()), name_(std::move(other.name_)) {}

PrivatePipeInstance& PrivatePipeInstance::operator=(
    PrivatePipeInstance&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.release();
        name_ = std::move(other.name_);
    }
    return *this;
}

Result<PrivatePipeInstance> PrivatePipeInstance::create(
    const product::Uuid& profile_id) {
    auto name = private_pipe_name(profile_id);
    if (!name.ok()) {
        return Result<PrivatePipeInstance>::failure(name.error());
    }
    static_assert(kPrivatePipeModeFlags
        == (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS));
    constexpr DWORD kBufferBytes = 64U * 1024U;
    const auto create_pipe = [&name](SECURITY_ATTRIBUTES* attributes) {
        return CreateNamedPipeW(
            name.value().c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED
                | FILE_FLAG_FIRST_PIPE_INSTANCE,
            static_cast<DWORD>(kPrivatePipeModeFlags),
            1,
            kBufferBytes,
            kBufferBytes,
            0,
            attributes);
    };
    auto security = create_private_pipe_security_descriptor();
    if (!security.ok()) {
        return Result<PrivatePipeInstance>::failure(security.error());
    }
    HANDLE handle = create_pipe(static_cast<SECURITY_ATTRIBUTES*>(
        security.value().native_security_attributes()));
    if (handle == INVALID_HANDLE_VALUE) {
        return ipc_error<PrivatePipeInstance>(
            HostProtocolError::invalid_argument,
            L"host_ipc.private_pipe_create",
            L"Failed to create exact private worker pipe", GetLastError());
    }
    return Result<PrivatePipeInstance>::success(
        PrivatePipeInstance(handle, name.move_value()));
}

#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
Result<PrivatePipeInstance>
PrivatePipeInstance::create_engineering_current_user(
    const product::Uuid& profile_id) {
    auto name = private_pipe_name(profile_id);
    if (!name.ok()) {
        return Result<PrivatePipeInstance>::failure(name.error());
    }
    static_assert(kPrivatePipeModeFlags
        == (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS));
    constexpr DWORD kBufferBytes = 64U * 1024U;
    const auto create_pipe = [&name](SECURITY_ATTRIBUTES* attributes) {
        return CreateNamedPipeW(
            name.value().c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED
                | FILE_FLAG_FIRST_PIPE_INSTANCE,
            static_cast<DWORD>(kPrivatePipeModeFlags), 1,
            kBufferBytes, kBufferBytes, 0, attributes);
    };
    HandleOwner token;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
            return ipc_error<PrivatePipeInstance>(
                HostProtocolError::invalid_argument,
                L"host_ipc.engineering_pipe_token",
                L"Failed to query engineering worker token",
                GetLastError());
        }
    token.reset(raw_token);
    DWORD required = 0;
    static_cast<void>(GetTokenInformation(
            token.get(), TokenUser, nullptr, 0, &required));
    std::vector<std::byte> token_bytes(required);
    if (required == 0 || !GetTokenInformation(
                token.get(), TokenUser, token_bytes.data(), required,
                &required)) {
            return ipc_error<PrivatePipeInstance>(
                HostProtocolError::invalid_argument,
                L"host_ipc.engineering_pipe_token",
                L"Failed to read engineering worker SID",
                GetLastError());
        }
    const auto* token_user = reinterpret_cast<TOKEN_USER*>(
            token_bytes.data());
    wchar_t* sid_text = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
            return ipc_error<PrivatePipeInstance>(
                HostProtocolError::invalid_argument,
                L"host_ipc.engineering_pipe_sid",
                L"Failed to encode engineering worker SID",
                GetLastError());
        }
    const std::wstring current_sid(sid_text);
    const std::wstring sddl =
            L"O:" + current_sid + L"G:" + current_sid
            + L"D:P(A;;FA;;;SY)(A;;0x00100183;;;"
            + std::wstring(ipc::kMyRamDiskServiceSid)
            + L")(A;;0x00100183;;;" + current_sid + L")";
    static_cast<void>(LocalFree(sid_text));
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            return ipc_error<PrivatePipeInstance>(
                HostProtocolError::invalid_argument,
                L"host_ipc.engineering_pipe_acl",
                L"Failed to build engineering pipe descriptor",
                GetLastError());
        }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    HANDLE handle = create_pipe(&attributes);
    static_cast<void>(LocalFree(descriptor));
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return ipc_error<PrivatePipeInstance>(
            HostProtocolError::invalid_argument,
            L"host_ipc.private_pipe_create",
            L"Failed to create exact private worker pipe",
            error);
    }
    return Result<PrivatePipeInstance>::success(
        PrivatePipeInstance(handle, name.move_value()));
}
#endif

void* PrivatePipeInstance::native_handle() const noexcept { return handle_; }
const std::wstring& PrivatePipeInstance::name() const noexcept { return name_; }
void* PrivatePipeInstance::release() noexcept {
    const auto value = handle_;
    handle_ = nullptr;
    return value;
}
void PrivatePipeInstance::close() noexcept {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(CancelIoEx(static_cast<HANDLE>(handle_), nullptr));
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle_)));
    }
    handle_ = nullptr;
}

StartupSecretTransfer::StartupSecretTransfer(void* read_handle) noexcept
    : read_handle_(read_handle) {}
StartupSecretTransfer::~StartupSecretTransfer() noexcept { close(); }
StartupSecretTransfer::StartupSecretTransfer(
    StartupSecretTransfer&& other) noexcept
    : read_handle_(other.release_read_handle()) {}
StartupSecretTransfer& StartupSecretTransfer::operator=(
    StartupSecretTransfer&& other) noexcept {
    if (this != &other) {
        close();
        read_handle_ = other.release_read_handle();
    }
    return *this;
}
void* StartupSecretTransfer::inherited_read_handle() const noexcept {
    return read_handle_;
}
void* StartupSecretTransfer::release_read_handle() noexcept {
    const auto value = read_handle_;
    read_handle_ = nullptr;
    return value;
}
void StartupSecretTransfer::close() noexcept {
    if (read_handle_ != nullptr && read_handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(read_handle_)));
    }
    read_handle_ = nullptr;
}

StagedStartupSecretTransfer::StagedStartupSecretTransfer(
    void* read_handle, void* write_handle) noexcept
    : read_handle_(read_handle), write_handle_(write_handle) {}
StagedStartupSecretTransfer::~StagedStartupSecretTransfer() noexcept {
    close();
}
StagedStartupSecretTransfer::StagedStartupSecretTransfer(
    StagedStartupSecretTransfer&& other) noexcept
    : read_handle_(other.release_read_handle()),
      write_handle_(other.write_handle_) {
    other.write_handle_ = nullptr;
}
StagedStartupSecretTransfer& StagedStartupSecretTransfer::operator=(
    StagedStartupSecretTransfer&& other) noexcept {
    if (this != &other) {
        close();
        read_handle_ = other.release_read_handle();
        write_handle_ = other.write_handle_;
        other.write_handle_ = nullptr;
    }
    return *this;
}
void* StagedStartupSecretTransfer::inherited_read_handle() const noexcept {
    return read_handle_;
}
void* StagedStartupSecretTransfer::release_read_handle() noexcept {
    const auto value = read_handle_;
    read_handle_ = nullptr;
    return value;
}
void StagedStartupSecretTransfer::close_parent_read() noexcept {
    if (read_handle_ != nullptr && read_handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(read_handle_)));
    }
    read_handle_ = nullptr;
}
void StagedStartupSecretTransfer::close() noexcept {
    close_parent_read();
    if (write_handle_ != nullptr && write_handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(write_handle_)));
    }
    write_handle_ = nullptr;
}

Result<void> StagedStartupSecretTransfer::publish(
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    if (write_handle_ == nullptr || write_handle_ == INVALID_HANDLE_VALUE) {
        return ipc_error<void>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_pipe_publish",
            L"Startup secret channel has already been published or closed");
    }
    auto record = encode_startup_record(binding, secret);
    if (!record.ok()) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(write_handle_)));
        write_handle_ = nullptr;
        return Result<void>::failure(record.error());
    }
    std::size_t offset = 0;
    while (offset < record.value().size()) {
        DWORD written = 0;
        const BOOL write_ok = WriteFile(
            static_cast<HANDLE>(write_handle_),
            record.value().data() + offset,
            static_cast<DWORD>(record.value().size() - offset),
            &written,
            nullptr);
        if (!write_ok || written == 0) {
            const auto error = write_ok ? ERROR_WRITE_FAULT : GetLastError();
            secure_zero(record.value());
            static_cast<void>(CloseHandle(
                static_cast<HANDLE>(write_handle_)));
            write_handle_ = nullptr;
            return ipc_error<void>(
                HostProtocolError::invalid_argument,
                L"host_ipc.startup_pipe_write",
                L"Failed to write the complete startup secret record",
                error);
        }
        offset += written;
    }
    secure_zero(record.value());
    static_cast<void>(CloseHandle(static_cast<HANDLE>(write_handle_)));
    write_handle_ = nullptr;
    return Result<void>::success();
}

Result<StagedStartupSecretTransfer>
prepare_staged_startup_secret_transfer() {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(
            &read_handle, &write_handle, &attributes,
            static_cast<DWORD>(kStartupRecordBytes))) {
        return ipc_error<StagedStartupSecretTransfer>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_pipe",
            L"Failed to create staged startup secret pipe",
            GetLastError());
    }
    HandleOwner read_owner(read_handle);
    HandleOwner write_owner(write_handle);
    if (!SetHandleInformation(
            write_handle, HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(
            read_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        return ipc_error<StagedStartupSecretTransfer>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_pipe_flags",
            L"Failed to restrict staged startup handle inheritance",
            GetLastError());
    }
    return Result<StagedStartupSecretTransfer>::success(
        StagedStartupSecretTransfer(
            read_owner.release(), write_owner.release()));
}

Result<StartupSecretTransfer> prepare_startup_secret_transfer(
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    auto staged = prepare_staged_startup_secret_transfer();
    if (!staged.ok()) {
        return Result<StartupSecretTransfer>::failure(staged.error());
    }
    auto published = staged.value().publish(binding, secret);
    if (!published.ok()) {
        return Result<StartupSecretTransfer>::failure(published.error());
    }
    return Result<StartupSecretTransfer>::success(
        StartupSecretTransfer(staged.value().release_read_handle()));
}

namespace {

Result<std::array<std::byte, kStartupRecordBytes>> read_startup_record(
    void* inherited_read_handle) {
    HandleOwner owner(static_cast<HANDLE>(inherited_read_handle));
    if (owner.get() == nullptr || owner.get() == INVALID_HANDLE_VALUE
        || GetFileType(owner.get()) != FILE_TYPE_PIPE) {
        return ipc_error<std::array<std::byte, kStartupRecordBytes>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_pipe_read",
            L"Inherited startup secret handle is not a pipe");
    }
    static_cast<void>(SetHandleInformation(
        owner.get(), HANDLE_FLAG_INHERIT, 0));
    std::array<std::byte, kStartupRecordBytes> record{};
    std::size_t offset = 0;
    while (offset < record.size()) {
        DWORD read = 0;
        const BOOL ok = ReadFile(
            owner.get(),
            record.data() + offset,
            static_cast<DWORD>(record.size() - offset),
            &read,
            nullptr);
        if (!ok || read == 0) {
            const auto error = ok ? ERROR_HANDLE_EOF : GetLastError();
            secure_zero(record);
            return ipc_error<std::array<std::byte, kStartupRecordBytes>>(
                HostProtocolError::invalid_argument,
                L"host_ipc.startup_pipe_read",
                L"Startup secret record is truncated",
                error);
        }
        offset += read;
    }
    std::byte trailing{};
    DWORD trailing_bytes = 0;
    const BOOL trailing_ok = ReadFile(
        owner.get(), &trailing, 1, &trailing_bytes, nullptr);
    if ((trailing_ok && trailing_bytes != 0)
        || (!trailing_ok && GetLastError() != ERROR_BROKEN_PIPE)) {
        const auto error = trailing_ok ? ERROR_INVALID_DATA : GetLastError();
        secure_zero(record);
        return ipc_error<std::array<std::byte, kStartupRecordBytes>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.startup_pipe_read",
            L"Startup secret record has trailing data",
            error);
    }
    return Result<std::array<std::byte, kStartupRecordBytes>>::success(record);
}

bool matches_startup_constraints(
    const WorkerBinding& binding,
    const StartupBindingConstraints& constraints) noexcept {
    return binding.profile_id == constraints.profile_id
        && binding.worker_pid == constraints.current_process_id
        && binding.process_creation_time
            == constraints.current_process_creation_time
        && binding.product_major == constraints.product_major
        && binding.product_minor == constraints.product_minor
        && binding.product_patch == constraints.product_patch;
}

} // namespace

StartupSecretMaterial::StartupSecretMaterial(
    WorkerBinding binding_value,
    SessionSecret secret_value) noexcept
    : binding(std::move(binding_value)), secret(std::move(secret_value)) {}

Result<StartupSecretMaterial> consume_startup_material(
    void* inherited_read_handle,
    const StartupBindingConstraints& constraints) {
    auto record = read_startup_record(inherited_read_handle);
    if (!record.ok()) {
        return Result<StartupSecretMaterial>::failure(record.error());
    }
    auto material = decode_startup_material_record(record.value());
    if (!material.ok()) return material;
    if (!matches_startup_constraints(material.value().binding, constraints)) {
        return ipc_error<StartupSecretMaterial>(
            HostProtocolError::binding_mismatch,
            L"host_ipc.startup_constraints",
            L"Startup secret does not match the live worker identity");
    }
    return material;
}

Result<SessionSecret> consume_startup_secret(
    void* inherited_read_handle,
    const WorkerBinding& expected_binding) {
    auto record = read_startup_record(inherited_read_handle);
    if (!record.ok()) {
        return Result<SessionSecret>::failure(record.error());
    }
    return decode_startup_record(record.value(), expected_binding);
}

RuntimeLeaseMaterial::RuntimeLeaseMaterial(
    WorkerBinding binding_value,
    SessionSecret secret_value) noexcept
    : binding(std::move(binding_value)), secret(std::move(secret_value)) {}

Result<std::vector<std::byte>> encode_runtime_lease(
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    if (!is_valid(binding)) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.lease_encode",
            L"Runtime lease binding is invalid");
    }
    auto protected_material = protect_reattachment(binding, secret);
    if (!protected_material.ok()) return protected_material;
    if (protected_material.value().empty()
        || protected_material.value().size()
            > kMaximumRuntimeLeaseBytes - kLeaseHeaderBytes
                - kAuthenticationTagBytes) {
        secure_zero(protected_material.value());
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_encode",
            L"DPAPI runtime lease ciphertext exceeds the fixed bound");
    }

    std::vector<std::byte> encoded;
    try {
        encoded.reserve(
            kLeaseHeaderBytes + protected_material.value().size()
            + kAuthenticationTagBytes);
        encoded.insert(encoded.end(), kLeaseMagic.begin(), kLeaseMagic.end());
        append_vector_le(encoded, kLeaseSchemaMajor);
        append_vector_le(encoded, kLeaseSchemaMinor);
        const auto total_bytes = kLeaseHeaderBytes
            + protected_material.value().size() + kAuthenticationTagBytes;
        append_vector_le(
            encoded, static_cast<std::uint32_t>(total_bytes));
        append_vector_le(
            encoded, static_cast<std::uint32_t>(kLeaseHeaderBytes));
        append_vector_le(
            encoded, static_cast<std::uint16_t>(kHostProtocolMajor));
        append_vector_le(
            encoded, static_cast<std::uint16_t>(kHostProtocolMinor));
        auto appended = append_binding_vector(
            encoded, binding, L"host_ipc.lease_encode");
        if (!appended.ok()) {
            secure_zero(protected_material.value());
            return Result<std::vector<std::byte>>::failure(appended.error());
        }
        append_vector_le(
            encoded,
            static_cast<std::uint32_t>(protected_material.value().size()));
        if (encoded.size() != kLeaseHeaderBytes) {
            secure_zero(protected_material.value());
            return ipc_error<std::vector<std::byte>>(
                HostProtocolError::lease_malformed,
                L"host_ipc.lease_encode",
                L"Runtime lease header layout invariant failed");
        }
        encoded.insert(
            encoded.end(),
            protected_material.value().begin(),
            protected_material.value().end());
    } catch (...) {
        secure_zero(protected_material.value());
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_encode",
            L"Failed to allocate bounded runtime lease bytes");
    }
    secure_zero(protected_material.value());
    auto digest = sha256_digest(encoded);
    if (!digest.ok()) {
        return Result<std::vector<std::byte>>::failure(digest.error());
    }
    encoded.insert(encoded.end(), digest.value().begin(), digest.value().end());
    secure_zero(digest.value());
    if (encoded.size() > kMaximumRuntimeLeaseBytes) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_encode",
            L"Runtime lease exceeds the fixed file bound");
    }
    return Result<std::vector<std::byte>>::success(std::move(encoded));
}

namespace {

struct RuntimeLeaseHeaderView final {
    WorkerBinding binding;
    std::size_t protected_offset{};
    std::uint32_t protected_bytes{};
};

Result<RuntimeLeaseHeaderView> inspect_runtime_lease_bytes(
    std::span<const std::byte> bytes) {
    if (bytes.size() < kLeaseHeaderBytes + kAuthenticationTagBytes
        || bytes.size() > kMaximumRuntimeLeaseBytes) {
        return ipc_error<RuntimeLeaseHeaderView>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_inspect",
            L"Runtime lease size is invalid");
    }
    const auto data_bytes = bytes.size() - kAuthenticationTagBytes;
    auto digest = sha256_digest(bytes.first(data_bytes));
    if (!digest.ok()) {
        return Result<RuntimeLeaseHeaderView>::failure(digest.error());
    }
    const bool digest_ok = digest_equal(
        digest.value(), bytes.subspan(data_bytes, kAuthenticationTagBytes));
    secure_zero(digest.value());
    if (!digest_ok) {
        return ipc_error<RuntimeLeaseHeaderView>(
            HostProtocolError::authentication_failed,
            L"host_ipc.lease_inspect",
            L"Runtime lease digest validation failed");
    }
    if (!std::equal(kLeaseMagic.begin(), kLeaseMagic.end(), bytes.begin())) {
        return ipc_error<RuntimeLeaseHeaderView>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_inspect",
            L"Runtime lease magic is invalid");
    }
    std::size_t offset = kLeaseMagic.size();
    auto schema_major = read_span_le<std::uint16_t>(
        bytes, offset, L"host_ipc.lease_inspect");
    auto schema_minor = read_span_le<std::uint16_t>(
        bytes, offset, L"host_ipc.lease_inspect");
    auto total = read_span_le<std::uint32_t>(
        bytes, offset, L"host_ipc.lease_inspect");
    auto header = read_span_le<std::uint32_t>(
        bytes, offset, L"host_ipc.lease_inspect");
    auto protocol_major = read_span_le<std::uint16_t>(
        bytes, offset, L"host_ipc.lease_inspect");
    auto protocol_minor = read_span_le<std::uint16_t>(
        bytes, offset, L"host_ipc.lease_inspect");
    auto binding = read_binding_vector(
        bytes.first(data_bytes), offset, L"host_ipc.lease_inspect");
    auto protected_bytes = read_span_le<std::uint32_t>(
        bytes, offset, L"host_ipc.lease_inspect");
    if (!schema_major.ok() || !schema_minor.ok() || !total.ok()
        || !header.ok() || !protocol_major.ok() || !protocol_minor.ok()
        || !binding.ok() || !protected_bytes.ok()
        || schema_major.value() != kLeaseSchemaMajor
        || schema_minor.value() != kLeaseSchemaMinor
        || total.value() != bytes.size()
        || header.value() != kLeaseHeaderBytes
        || protocol_major.value() != kHostProtocolMajor
        || protocol_minor.value() != kHostProtocolMinor
        || offset != kLeaseHeaderBytes
        || protected_bytes.value() == 0
        || protected_bytes.value() != data_bytes - offset) {
        return ipc_error<RuntimeLeaseHeaderView>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_inspect",
            L"Runtime lease header, version or length is invalid");
    }
    return Result<RuntimeLeaseHeaderView>::success(RuntimeLeaseHeaderView{
        binding.move_value(), offset, protected_bytes.value()});
}

} // namespace

Result<RuntimeLeaseMaterial> decode_runtime_lease(
    std::span<const std::byte> bytes,
    const WorkerBinding& expected_binding) {
    if (!is_valid(expected_binding)) {
        return ipc_error<RuntimeLeaseMaterial>(
            HostProtocolError::lease_malformed,
            L"host_ipc.lease_decode",
            L"Expected runtime lease binding is invalid");
    }
    auto inspected = inspect_runtime_lease_bytes(bytes);
    if (!inspected.ok()) {
        return Result<RuntimeLeaseMaterial>::failure(inspected.error());
    }
    if (inspected.value().binding != expected_binding) {
        return ipc_error<RuntimeLeaseMaterial>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.lease_decode",
            L"Runtime lease is stale or belongs to another worker");
    }
    auto secret = unprotect_reattachment(
        bytes.subspan(
            inspected.value().protected_offset,
            inspected.value().protected_bytes),
        expected_binding);
    if (!secret.ok()) {
        return Result<RuntimeLeaseMaterial>::failure(secret.error());
    }
    return Result<RuntimeLeaseMaterial>::success(RuntimeLeaseMaterial(
        expected_binding, secret.move_value()));
}

Result<WorkerBinding> next_reattachment_binding(
    const WorkerBinding& current,
    std::uint64_t next_profile_revision) {
    if (!is_valid(current)
        || current.session_generation
            == (std::numeric_limits<std::uint64_t>::max)()
        || next_profile_revision < current.profile_revision) {
        return ipc_error<WorkerBinding>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.lease_rotate",
            L"Runtime lease rotation would roll back or overflow identity");
    }
    auto next = current;
    ++next.session_generation;
    next.profile_revision = next_profile_revision;
    return Result<WorkerBinding>::success(std::move(next));
}

namespace {

class RuntimeLeaseTempGuard final {
public:
    RuntimeLeaseTempGuard(
        IRuntimeLeaseDirectory& directory,
        RuntimeLeaseTemp temp) noexcept
        : directory_(&directory), temp_(temp) {}
    ~RuntimeLeaseTempGuard() noexcept {
        if (directory_ != nullptr) directory_->discard_temp(temp_);
    }
    RuntimeLeaseTempGuard(const RuntimeLeaseTempGuard&) = delete;
    RuntimeLeaseTempGuard& operator=(const RuntimeLeaseTempGuard&) = delete;
    void release() noexcept { directory_ = nullptr; }

private:
    IRuntimeLeaseDirectory* directory_{};
    RuntimeLeaseTemp temp_{};
};

Result<void> lease_commit_indeterminate(const Error& source) {
    return Result<void>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(
            HostProtocolError::commit_indeterminate),
        L"host_ipc.lease_publish",
        L"Runtime lease publication may already be visible",
        source.native_code});
}

} // namespace

Result<void> publish_runtime_lease_atomically(
    IRuntimeLeaseDirectory& directory,
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    bool publication_attempted = false;
    try {
        auto encoded = encode_runtime_lease(binding, secret);
        if (!encoded.ok()) return Result<void>::failure(encoded.error());
        auto created = directory.create_secure_temp();
        if (!created.ok()) return Result<void>::failure(created.error());
        RuntimeLeaseTempGuard guard(directory, created.value());
        auto written = directory.write_temp(created.value(), encoded.value());
        if (!written.ok()) return written;
        auto flushed = directory.flush_temp(created.value());
        if (!flushed.ok()) return flushed;
        auto verified = directory.read_temp(created.value());
        if (!verified.ok()) {
            return Result<void>::failure(verified.error());
        }
        if (verified.value() != encoded.value()) {
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_verify",
                L"Flushed runtime lease temp did not verify byte-for-byte");
        }
        publication_attempted = true;
        auto published = directory.publish_temp(created.value());
        if (!published.ok()) {
            return lease_commit_indeterminate(published.error());
        }
        guard.release();
        auto directory_flushed = directory.flush_directory();
        if (!directory_flushed.ok()) {
            return lease_commit_indeterminate(directory_flushed.error());
        }
        return Result<void>::success();
    } catch (...) {
        if (publication_attempted) {
            return lease_commit_indeterminate(Error{
                ErrorDomain::internal,
                static_cast<std::uint32_t>(
                    HostProtocolError::unsafe_runtime_lease),
                L"host_ipc.lease_publish",
                L"Runtime lease backend threw after publication began"});
        }
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_publish",
            L"Runtime lease backend threw before publication");
    }
}

Result<RuntimeLeaseMaterial> load_runtime_lease(
    IRuntimeLeaseDirectory& directory,
    const WorkerBinding& expected_binding) {
    try {
        auto current = directory.read_current();
        if (!current.ok()) {
            return Result<RuntimeLeaseMaterial>::failure(current.error());
        }
        return decode_runtime_lease(current.value(), expected_binding);
    } catch (...) {
        return ipc_error<RuntimeLeaseMaterial>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_load",
            L"Runtime lease backend threw while reading current bytes");
    }
}

Result<WorkerBinding> inspect_runtime_lease_binding(
    IRuntimeLeaseDirectory& directory) {
    try {
        auto current = directory.read_current();
        if (!current.ok()) {
            return Result<WorkerBinding>::failure(current.error());
        }
        auto inspected = inspect_runtime_lease_bytes(current.value());
        if (!inspected.ok()) {
            return Result<WorkerBinding>::failure(inspected.error());
        }
        return Result<WorkerBinding>::success(
            std::move(inspected.value().binding));
    } catch (...) {
        return ipc_error<WorkerBinding>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_inspect",
            L"Runtime lease backend threw while inspecting current bytes");
    }
}

namespace {

Result<SessionSecret> copy_secret(const SessionSecret& source) {
    SecretBytes bytes{};
    std::copy(source.bytes().begin(), source.bytes().end(), bytes.begin());
    auto copied = SessionSecret::from_bytes(bytes);
    secure_zero(bytes);
    return copied;
}

Result<AuthenticationTag> reattachment_hmac(
    std::span<const std::byte> key,
    std::span<const std::byte> data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_SHA256_ALGORITHM,
        nullptr,
        BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) {
        return ipc_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_ipc.rotation_hmac",
            L"Failed to open HMAC-SHA256 provider",
            static_cast<std::uint32_t>(status));
    }
    DWORD object_bytes = 0;
    DWORD copied = 0;
    status = BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_bytes),
        sizeof(object_bytes),
        &copied,
        0);
    std::vector<unsigned char> object;
    if (status >= 0 && copied == sizeof(object_bytes)) {
        try {
            object.resize(object_bytes);
        } catch (...) {
            status = static_cast<NTSTATUS>(0xC0000017L);
        }
    } else if (status >= 0) {
        status = static_cast<NTSTATUS>(0xC000000DL);
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (status >= 0) {
        status = BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            static_cast<ULONG>(object.size()),
            const_cast<PUCHAR>(
                reinterpret_cast<const UCHAR*>(key.data())),
            static_cast<ULONG>(key.size()),
            0);
    }
    if (status >= 0) {
        status = BCryptHashData(
            hash,
            const_cast<PUCHAR>(
                reinterpret_cast<const UCHAR*>(data.data())),
            static_cast<ULONG>(data.size()),
            0);
    }
    AuthenticationTag result{};
    if (status >= 0) {
        status = BCryptFinishHash(
            hash,
            reinterpret_cast<PUCHAR>(result.data()),
            static_cast<ULONG>(result.size()),
            0);
    }
    if (hash != nullptr) static_cast<void>(BCryptDestroyHash(hash));
    static_cast<void>(BCryptCloseAlgorithmProvider(algorithm, 0));
    if (!object.empty()) {
        secure_zero(std::as_writable_bytes(std::span(object)));
    }
    if (status < 0) {
        secure_zero(result);
        return ipc_error<AuthenticationTag>(
            HostProtocolError::crypto_failure,
            L"host_ipc.rotation_hmac",
            L"Failed to derive the next reattachment secret",
            static_cast<std::uint32_t>(status));
    }
    auto wrapped = Result<AuthenticationTag>::success(result);
    secure_zero(result);
    return wrapped;
}

Result<SessionSecret> derive_next_reattachment_secret(
    const SessionSecret& current,
    const WorkerBinding& next) {
    std::vector<std::byte> input;
    try {
        constexpr std::string_view domain =
            "MYRD-RUNTIME-LEASE-ROTATION-v1";
        input.reserve(domain.size() + 1 + kBindingBytes);
        for (const auto value : domain) {
            input.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(value)));
        }
        input.push_back(std::byte{});
        auto appended = append_binding_vector(
            input, next, L"host_ipc.rotation_kdf");
        if (!appended.ok()) {
            return Result<SessionSecret>::failure(appended.error());
        }
    } catch (...) {
        return ipc_error<SessionSecret>(
            HostProtocolError::crypto_failure,
            L"host_ipc.rotation_kdf",
            L"Failed to allocate bounded rotation KDF input");
    }
    auto tag = reattachment_hmac(current.bytes(), input);
    secure_zero(input);
    if (!tag.ok()) return Result<SessionSecret>::failure(tag.error());
    SecretBytes bytes{};
    std::copy(tag.value().begin(), tag.value().end(), bytes.begin());
    secure_zero(tag.value());
    auto secret = SessionSecret::from_bytes(bytes);
    secure_zero(bytes);
    return secret;
}

Result<std::vector<std::byte>> encode_rotation_binding(
    const WorkerBinding& binding) {
    if (!is_valid(binding)) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.rotation_encode",
            L"Rotation binding is invalid");
    }
    std::vector<std::byte> payload;
    try {
        payload.reserve(kBindingBytes);
    } catch (...) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_encode",
            L"Failed to allocate rotation payload");
    }
    auto appended = append_binding_vector(
        payload, binding, L"host_ipc.rotation_encode");
    if (!appended.ok()) {
        return Result<std::vector<std::byte>>::failure(appended.error());
    }
    if (payload.size() != kBindingBytes) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_encode",
            L"Rotation payload layout invariant failed");
    }
    return Result<std::vector<std::byte>>::success(std::move(payload));
}

Result<WorkerBinding> decode_rotation_binding(
    std::span<const std::byte> payload) {
    std::size_t offset = 0;
    auto binding = read_binding_vector(
        payload, offset, L"host_ipc.rotation_decode");
    if (!binding.ok()) return binding;
    if (offset != payload.size() || offset != kBindingBytes) {
        return ipc_error<WorkerBinding>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_decode",
            L"Rotation payload contains trailing or missing bytes");
    }
    return binding;
}

bool nonce_is_zero(const HandshakeNonce& value) noexcept {
    return std::all_of(
        value.begin(), value.end(),
        [](const std::byte byte) { return byte == std::byte{}; });
}

HostMessage rotation_message(
    const WorkerBinding& binding,
    const std::uint32_t command,
    std::vector<std::byte> payload) {
    return HostMessage{
        binding.profile_id,
        binding.image_id,
        command,
        0,
        std::move(payload)};
}

Result<void> validate_rotation_response(
    const HostMessage& response,
    const WorkerBinding& binding,
    const std::uint32_t expected_command) {
    if (response.request_id != binding.profile_id
        || response.operation_id != binding.image_id
        || response.command_id != expected_command
        || response.flags != 0) {
        return ipc_error<void>(
            HostProtocolError::authentication_failed,
            L"host_ipc.rotation_response",
            L"Authenticated rotation response correlation is invalid");
    }
    auto decoded = decode_rotation_binding(response.payload);
    if (!decoded.ok()) return Result<void>::failure(decoded.error());
    if (decoded.value() != binding) {
        return ipc_error<void>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.rotation_response",
            L"Authenticated rotation response binding is stale");
    }
    return Result<void>::success();
}

struct RotationState final {
    WorkerBinding current;
    std::optional<WorkerBinding> pending;
    bool recovery_required{};
};

Result<std::vector<std::byte>> encode_rotation_state(
    const WorkerBinding& current,
    const WorkerBinding* pending,
    const bool recovery_required) {
    if (!is_valid(current)
        || (pending != nullptr && !is_valid(*pending))) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.rotation_state_encode",
            L"Rotation state binding is invalid");
    }
    std::vector<std::byte> payload;
    try {
        payload.reserve(8 + kBindingBytes
            + (pending == nullptr ? 0 : kBindingBytes));
        std::uint32_t flags = 0;
        if (pending != nullptr) flags |= 0x01U;
        if (recovery_required) flags |= 0x02U;
        append_vector_le(payload, flags);
        append_vector_le(payload, std::uint32_t{});
        auto current_result = append_binding_vector(
            payload, current, L"host_ipc.rotation_state_encode");
        if (!current_result.ok()) {
            return Result<std::vector<std::byte>>::failure(
                current_result.error());
        }
        if (pending != nullptr) {
            auto pending_result = append_binding_vector(
                payload, *pending, L"host_ipc.rotation_state_encode");
            if (!pending_result.ok()) {
                return Result<std::vector<std::byte>>::failure(
                    pending_result.error());
            }
        }
    } catch (...) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_state_encode",
            L"Failed to allocate bounded rotation state");
    }
    return Result<std::vector<std::byte>>::success(std::move(payload));
}

Result<RotationState> decode_rotation_state(
    std::span<const std::byte> payload) {
    if (payload.size() != 8 + kBindingBytes
        && payload.size() != 8 + 2 * kBindingBytes) {
        return ipc_error<RotationState>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_state_decode",
            L"Rotation state size is invalid");
    }
    std::size_t offset = 0;
    auto flags = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.rotation_state_decode");
    auto reserved = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.rotation_state_decode");
    auto current = read_binding_vector(
        payload, offset, L"host_ipc.rotation_state_decode");
    if (!flags.ok() || !reserved.ok() || !current.ok()
        || reserved.value() != 0
        || (flags.value() & ~0x03U) != 0) {
        return ipc_error<RotationState>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_state_decode",
            L"Rotation state header is invalid");
    }
    const bool has_pending = (flags.value() & 0x01U) != 0;
    const bool recovery = (flags.value() & 0x02U) != 0;
    if (has_pending != (payload.size() == 8 + 2 * kBindingBytes)) {
        return ipc_error<RotationState>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_state_decode",
            L"Rotation state pending flag disagrees with payload length");
    }
    std::optional<WorkerBinding> pending;
    if (has_pending) {
        auto value = read_binding_vector(
            payload, offset, L"host_ipc.rotation_state_decode");
        if (!value.ok()) return Result<RotationState>::failure(value.error());
        pending = value.move_value();
    }
    if (offset != payload.size()) {
        return ipc_error<RotationState>(
            HostProtocolError::invalid_argument,
            L"host_ipc.rotation_state_decode",
            L"Rotation state has trailing bytes");
    }
    return Result<RotationState>::success(RotationState{
        current.move_value(), std::move(pending), recovery});
}

Result<ServiceHandshakeAcceptance> establish_service_attachment(
    IWorkerReattachmentTransport& transport,
    const HandshakeNonce& manager_nonce,
    const WorkerBinding& binding,
    const SessionSecret& secret) {
    auto handshake_secret = copy_secret(secret);
    if (!handshake_secret.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(
            handshake_secret.error());
    }
    auto service_nonce = generate_handshake_nonce();
    if (!service_nonce.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(
            service_nonce.error());
    }
    auto handshake = ServiceHandshake::begin(
        binding, handshake_secret.move_value(), service_nonce.value());
    if (!handshake.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(
            handshake.error());
    }
    auto challenge = transport.begin_attachment(
        manager_nonce, handshake.value().hello());
    if (!challenge.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(
            challenge.error());
    }
    auto accepted = handshake.value().accept_challenge(challenge.value());
    if (!accepted.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(accepted.error());
    }
    auto authenticated = transport.authenticate_attachment(
        manager_nonce, accepted.value().proof);
    if (!authenticated.ok()) {
        return Result<ServiceHandshakeAcceptance>::failure(
            authenticated.error());
    }
    return accepted;
}

Result<RotationState> query_service_rotation_state(
    IWorkerReattachmentTransport& transport,
    const HandshakeNonce& manager_nonce,
    const WorkerBinding& binding,
    SessionAuthenticator& session) {
    auto request = session.seal(rotation_message(
        binding, kReattachmentStateQuery, {}));
    if (!request.ok()) {
        return Result<RotationState>::failure(request.error());
    }
    auto response = transport.exchange(manager_nonce, request.value());
    if (!response.ok()) {
        return Result<RotationState>::failure(response.error());
    }
    auto opened = session.open(response.value());
    if (!opened.ok()) {
        return Result<RotationState>::failure(opened.error());
    }
    if (opened.value().request_id != binding.profile_id
        || opened.value().operation_id != binding.image_id
        || opened.value().command_id != kReattachmentStateResponse
        || opened.value().flags != 0) {
        return ipc_error<RotationState>(
            HostProtocolError::authentication_failed,
            L"host_ipc.rotation_state_query",
            L"Authenticated rotation state response is invalid");
    }
    auto state = decode_rotation_state(opened.value().payload);
    if (!state.ok()) return state;
    if (state.value().current != binding
        || (state.value().pending.has_value()
            && state.value().pending.value() == binding)) {
        return ipc_error<RotationState>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.rotation_state_query",
            L"Worker rotation state disagrees with the authenticated lease binding");
    }
    return state;
}

Result<void> finalize_service_rotation_recovery(
    IWorkerReattachmentTransport& transport,
    const HandshakeNonce& manager_nonce,
    const WorkerBinding& binding,
    SessionAuthenticator& session) {
    auto payload = encode_rotation_binding(binding);
    if (!payload.ok()) return Result<void>::failure(payload.error());
    auto request = session.seal(rotation_message(
        binding,
        kReattachmentRecoveryFinalize,
        payload.move_value()));
    if (!request.ok()) return Result<void>::failure(request.error());
    auto response = transport.exchange(manager_nonce, request.value());
    if (!response.ok()) return Result<void>::failure(response.error());
    auto opened = session.open(response.value());
    if (!opened.ok()) return Result<void>::failure(opened.error());
    auto finalized = validate_rotation_response(
        opened.value(), binding, kReattachmentRecoveryFinalizeAck);
    if (!finalized.ok()) return finalized;

    payload = encode_rotation_binding(binding);
    if (!payload.ok()) return Result<void>::failure(payload.error());
    request = session.seal(rotation_message(
        binding,
        kReattachmentRecoveryConfirm,
        payload.move_value()));
    if (!request.ok()) return Result<void>::failure(request.error());
    response = transport.exchange(manager_nonce, request.value());
    if (!response.ok()) return Result<void>::failure(response.error());
    opened = session.open(response.value());
    if (!opened.ok()) return Result<void>::failure(opened.error());
    return validate_rotation_response(
        opened.value(), binding, kReattachmentRecoveryConfirmAck);
}

inline constexpr std::array<std::byte, 8> kRuntimeStatusMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'R'}, std::byte{'T'}, std::byte{'S'}, std::byte{0}};
inline constexpr std::uint16_t kRuntimeStatusSchemaMajor = 1;
inline constexpr std::uint16_t kRuntimeStatusSchemaMinor = 1;
inline constexpr std::wstring_view kRuntimeProxyPathPrefix =
    L"\\BaseNamedObjects\\Global\\myrd-m1-";
inline constexpr std::size_t kRuntimeProxyPathCharacters =
    kRuntimeProxyPathPrefix.size() + 32;
inline constexpr std::size_t kRuntimeStatusBytes =
    340 + sizeof(std::uint32_t) + sizeof(std::uint64_t)
    + kRuntimeProxyPathCharacters * sizeof(std::uint16_t);
inline constexpr std::uint32_t kRuntimeStatusKnownFlags = 0x0fU;

bool uuid_is_zero(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](const std::byte byte) { return byte == std::byte{}; });
}

bool valid_runtime_proxy_path(const std::wstring_view value) noexcept {
    if (value.size() != kRuntimeProxyPathCharacters
        || value.substr(0, kRuntimeProxyPathPrefix.size())
            != kRuntimeProxyPathPrefix) {
        return false;
    }
    return std::all_of(
        value.begin()
            + static_cast<std::ptrdiff_t>(kRuntimeProxyPathPrefix.size()),
        value.end(),
        [](const wchar_t character) {
            return (character >= L'0' && character <= L'9')
                || (character >= L'a' && character <= L'f');
        });
}

Result<void> append_runtime_uuid(
    std::vector<std::byte>& output,
    const product::Uuid& value) {
    try {
        const auto text = value.to_string();
        if (text.size() != kCanonicalUuidBytes) {
            return ipc_error<void>(
                HostProtocolError::invalid_argument,
                L"host_ipc.runtime_status_encode",
                L"Runtime status UUID is not canonical");
        }
        for (const auto byte : text) {
            output.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(byte)));
        }
        return Result<void>::success();
    } catch (...) {
        return ipc_error<void>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_encode",
            L"Failed to encode bounded runtime status UUID");
    }
}

Result<product::Uuid> read_runtime_uuid(
    std::span<const std::byte> payload,
    std::size_t& offset) {
    if (offset > payload.size()
        || payload.size() - offset < kCanonicalUuidBytes) {
        return ipc_error<product::Uuid>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_decode",
            L"Runtime status UUID is truncated");
    }
    std::array<char, kCanonicalUuidBytes> text{};
    for (auto& value : text) {
        value = static_cast<char>(std::to_integer<unsigned char>(
            payload[offset++]));
    }
    auto parsed = product::Uuid::parse(
        std::string_view(text.data(), text.size()));
    if (!parsed.ok()) {
        return ipc_error<product::Uuid>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_decode",
            L"Runtime status UUID is malformed");
    }
    return parsed;
}

} // namespace

Result<std::vector<std::byte>> encode_worker_runtime_status(
    const WorkerRuntimeStatus& status) {
    const bool has_operation = status.active_operation_id.has_value();
    if (!is_valid(status.binding)
        || status.device.length_bytes == 0
        || status.aim_device_id == 0x00ffffffU
        || status.committed_generation == 0
        || !valid_runtime_proxy_path(status.proxy_native_path)
        || uuid_is_zero(status.device.gpt_disk_id)
        || uuid_is_zero(status.device.partition_id)
        || uuid_is_zero(status.device.volume_id)
        || status.heartbeat_sequence == 0
        || status.snapshot_sequence == 0
        || status.checkpoint_policy_revision == 0
        || (status.checkpoint_policy_enabled
            && status.checkpoint_deadline_unix_ms == 0)
        || (has_operation
            && (uuid_is_zero(status.active_operation_id.value())
                || status.active_operation_phase == 0))
        || (!has_operation
            && (status.active_operation_phase != 0
                || status.active_operation_noncancellable))) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_encode",
            L"Worker runtime status is contradictory or incomplete");
    }
    std::vector<std::byte> payload;
    try {
        payload.reserve(kRuntimeStatusBytes);
        payload.insert(
            payload.end(), kRuntimeStatusMagic.begin(),
            kRuntimeStatusMagic.end());
        append_vector_le(payload, kRuntimeStatusSchemaMajor);
        append_vector_le(payload, kRuntimeStatusSchemaMinor);
        append_vector_le(
            payload, static_cast<std::uint32_t>(kRuntimeStatusBytes));
        auto binding = append_binding_vector(
            payload, status.binding, L"host_ipc.runtime_status_encode");
        if (!binding.ok()) {
            return Result<std::vector<std::byte>>::failure(binding.error());
        }
        append_vector_le(payload, status.device.physical_drive_number);
        append_vector_le(payload, status.device.length_bytes);
        append_vector_le(payload, status.device.port_number);
        payload.push_back(static_cast<std::byte>(status.device.path_id));
        payload.push_back(static_cast<std::byte>(status.device.target_id));
        payload.push_back(static_cast<std::byte>(status.device.lun));
        payload.push_back(std::byte{});
        auto gpt = append_runtime_uuid(payload, status.device.gpt_disk_id);
        auto partition = append_runtime_uuid(
            payload, status.device.partition_id);
        auto volume = append_runtime_uuid(payload, status.device.volume_id);
        if (!gpt.ok() || !partition.ok() || !volume.ok()) {
            return ipc_error<std::vector<std::byte>>(
                HostProtocolError::invalid_argument,
                L"host_ipc.runtime_status_encode",
                L"Failed to encode runtime device UUIDs");
        }
        append_vector_le(payload, status.aim_device_id);
        append_vector_le(payload, status.committed_generation);
        for (const auto character : status.proxy_native_path) {
            append_vector_le(
                payload, static_cast<std::uint16_t>(character));
        }
        append_vector_le(payload, status.heartbeat_sequence);
        append_vector_le(payload, status.snapshot_sequence);
        append_vector_le(payload, status.dirty_epoch);
        append_vector_le(payload, status.checkpoint_policy_revision);
        append_vector_le(payload, status.checkpoint_deadline_unix_ms);
        std::uint32_t flags = 0;
        if (status.dirty) flags |= 0x01U;
        if (status.checkpoint_policy_enabled) flags |= 0x02U;
        if (has_operation) flags |= 0x04U;
        if (status.active_operation_noncancellable) flags |= 0x08U;
        append_vector_le(payload, flags);
        append_vector_le(payload, status.active_operation_phase);
        if (has_operation) {
            auto operation = append_runtime_uuid(
                payload, status.active_operation_id.value());
            if (!operation.ok()) {
                return Result<std::vector<std::byte>>::failure(
                    operation.error());
            }
        } else {
            constexpr std::string_view zero_uuid =
                "00000000-0000-0000-0000-000000000000";
            for (const auto value : zero_uuid) {
                payload.push_back(static_cast<std::byte>(
                    static_cast<unsigned char>(value)));
            }
        }
    } catch (...) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_encode",
            L"Failed to allocate bounded runtime status payload");
    }
    if (payload.size() != kRuntimeStatusBytes) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_encode",
            L"Runtime status layout invariant failed");
    }
    return Result<std::vector<std::byte>>::success(std::move(payload));
}

Result<WorkerRuntimeStatus> decode_worker_runtime_status(
    std::span<const std::byte> payload,
    const WorkerBinding& authenticated_binding) {
    if (!is_valid(authenticated_binding)
        || payload.size() != kRuntimeStatusBytes
        || !std::equal(
            kRuntimeStatusMagic.begin(),
            kRuntimeStatusMagic.end(),
            payload.begin())) {
        return ipc_error<WorkerRuntimeStatus>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_decode",
            L"Runtime status size, magic or authenticated binding is invalid");
    }
    std::size_t offset = kRuntimeStatusMagic.size();
    auto major = read_span_le<std::uint16_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto minor = read_span_le<std::uint16_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto total = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto binding = read_binding_vector(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto drive = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto length = read_span_le<std::uint64_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto port = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    if (!major.ok() || !minor.ok() || !total.ok() || !binding.ok()
        || !drive.ok() || !length.ok() || !port.ok()
        || offset > payload.size() || payload.size() - offset < 4) {
        return ipc_error<WorkerRuntimeStatus>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_decode",
            L"Runtime status fixed prefix is truncated");
    }
    const auto path_id = std::to_integer<std::uint8_t>(payload[offset++]);
    const auto target_id = std::to_integer<std::uint8_t>(payload[offset++]);
    const auto lun = std::to_integer<std::uint8_t>(payload[offset++]);
    const auto reserved = payload[offset++];
    auto gpt = read_runtime_uuid(payload, offset);
    auto partition = read_runtime_uuid(payload, offset);
    auto volume = read_runtime_uuid(payload, offset);
    auto aim_device_id = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto committed_generation = read_span_le<std::uint64_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    std::wstring proxy_native_path;
    try {
        proxy_native_path.reserve(kRuntimeProxyPathCharacters);
    } catch (...) {
        return ipc_error<WorkerRuntimeStatus>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_decode",
            L"Failed to allocate the bounded runtime proxy path");
    }
    for (std::size_t index = 0;
         index < kRuntimeProxyPathCharacters; ++index) {
        auto character = read_span_le<std::uint16_t>(
            payload, offset, L"host_ipc.runtime_status_decode");
        if (!character.ok()) {
            return ipc_error<WorkerRuntimeStatus>(
                HostProtocolError::invalid_argument,
                L"host_ipc.runtime_status_decode",
                L"Runtime proxy path is truncated");
        }
        proxy_native_path.push_back(
            static_cast<wchar_t>(character.value()));
    }
    auto heartbeat = read_span_le<std::uint64_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto snapshot = read_span_le<std::uint64_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto dirty_epoch = read_span_le<std::uint64_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto policy_revision = read_span_le<std::uint64_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto deadline = read_span_le<std::uint64_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto flags = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto operation_phase = read_span_le<std::uint32_t>(
        payload, offset, L"host_ipc.runtime_status_decode");
    auto operation_id = read_runtime_uuid(payload, offset);
    if (!gpt.ok() || !partition.ok() || !volume.ok()
        || !aim_device_id.ok() || !committed_generation.ok()
        || !heartbeat.ok() || !snapshot.ok() || !dirty_epoch.ok()
        || !policy_revision.ok() || !deadline.ok() || !flags.ok()
        || !operation_phase.ok() || !operation_id.ok()
        || major.value() != kRuntimeStatusSchemaMajor
        || minor.value() != kRuntimeStatusSchemaMinor
        || total.value() != payload.size()
        || binding.value() != authenticated_binding
        || reserved != std::byte{}
        || (flags.value() & ~kRuntimeStatusKnownFlags) != 0
        || length.value() == 0
        || aim_device_id.value() == 0x00ffffffU
        || committed_generation.value() == 0
        || !valid_runtime_proxy_path(proxy_native_path)
        || uuid_is_zero(gpt.value()) || uuid_is_zero(partition.value())
        || uuid_is_zero(volume.value())
        || heartbeat.value() == 0 || snapshot.value() == 0
        || policy_revision.value() == 0
        || (((flags.value() & 0x02U) != 0)
            && deadline.value() == 0)
        || offset != payload.size()) {
        return ipc_error<WorkerRuntimeStatus>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_decode",
            L"Runtime status fields are contradictory or invalid");
    }
    const bool has_operation = (flags.value() & 0x04U) != 0;
    const bool operation_noncancellable = (flags.value() & 0x08U) != 0;
    if ((has_operation
            && (uuid_is_zero(operation_id.value())
                || operation_phase.value() == 0))
        || (!has_operation
            && (!uuid_is_zero(operation_id.value())
                || operation_phase.value() != 0
                || operation_noncancellable))) {
        return ipc_error<WorkerRuntimeStatus>(
            HostProtocolError::invalid_argument,
            L"host_ipc.runtime_status_decode",
            L"Runtime operation identity is contradictory");
    }
    WorkerRuntimeDeviceIdentity device{
        drive.value(), length.value(), port.value(), path_id, target_id, lun,
        gpt.move_value(), partition.move_value(), volume.move_value()};
    std::optional<product::Uuid> operation;
    if (has_operation) operation = operation_id.move_value();
    return Result<WorkerRuntimeStatus>::success(WorkerRuntimeStatus{
        binding.move_value(),
        std::move(device),
        aim_device_id.value(),
        committed_generation.value(),
        std::move(proxy_native_path),
        heartbeat.value(),
        snapshot.value(),
        dirty_epoch.value(),
        (flags.value() & 0x01U) != 0,
        (flags.value() & 0x02U) != 0,
        policy_revision.value(),
        deadline.value(),
        std::move(operation),
        operation_phase.value(),
        operation_noncancellable});
}

Result<WorkerRuntimeStatus> decode_persisted_worker_runtime_status(
    const std::span<const std::byte> payload,
    const WorkerBinding& current_lease_binding) {
    auto current = decode_worker_runtime_status(
        payload, current_lease_binding);
    if (current.ok() || current_lease_binding.session_generation <= 1) {
        return current;
    }

    auto immediate_predecessor = current_lease_binding;
    --immediate_predecessor.session_generation;
    return decode_worker_runtime_status(payload, immediate_predecessor);
}

namespace {

constexpr std::size_t kWorkerControlReplyBytes = 16;

Result<std::vector<std::byte>> encode_worker_control_reply(
    const std::uint32_t flags,
    const std::uint64_t generation,
    const wchar_t* operation) {
    try {
        std::vector<std::byte> payload;
        payload.reserve(kWorkerControlReplyBytes);
        append_vector_le(payload, flags);
        append_vector_le(payload, std::uint32_t{});
        append_vector_le(payload, generation);
        return Result<std::vector<std::byte>>::success(std::move(payload));
    } catch (...) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::crypto_failure,
            operation,
            L"Worker control reply allocation failed");
    }
}

Result<std::pair<std::uint32_t, std::uint64_t>>
decode_worker_control_reply(
    const std::span<const std::byte> payload,
    const std::uint32_t known_flags,
    const wchar_t* operation) {
    if (payload.size() != kWorkerControlReplyBytes) {
        return ipc_error<std::pair<std::uint32_t, std::uint64_t>>(
            HostProtocolError::invalid_argument,
            operation,
            L"Worker control reply length is invalid");
    }
    std::size_t offset = 0;
    auto flags = read_span_le<std::uint32_t>(payload, offset, operation);
    auto reserved = read_span_le<std::uint32_t>(payload, offset, operation);
    auto generation = read_span_le<std::uint64_t>(payload, offset, operation);
    if (!flags.ok() || !reserved.ok() || !generation.ok()
        || reserved.value() != 0 || (flags.value() & ~known_flags) != 0
        || offset != payload.size()) {
        return ipc_error<std::pair<std::uint32_t, std::uint64_t>>(
            HostProtocolError::invalid_argument,
            operation,
            L"Worker control reply flags or reserved fields are invalid");
    }
    return Result<std::pair<std::uint32_t, std::uint64_t>>::success(
        {flags.value(), generation.value()});
}

} // namespace

Result<std::vector<std::byte>> encode_worker_checkpoint_reply(
    const WorkerCheckpointReply& reply) {
    return encode_worker_control_reply(
        reply.confirmed ? 0x01U : 0U,
        reply.generation,
        L"host_ipc.checkpoint_reply_encode");
}

Result<WorkerCheckpointReply> decode_worker_checkpoint_reply(
    const std::span<const std::byte> payload) {
    auto decoded = decode_worker_control_reply(
        payload, 0x01U, L"host_ipc.checkpoint_reply_decode");
    if (!decoded.ok()) {
        return Result<WorkerCheckpointReply>::failure(decoded.error());
    }
    return Result<WorkerCheckpointReply>::success({
        (decoded.value().first & 0x01U) != 0,
        decoded.value().second});
}

Result<std::vector<std::byte>> encode_worker_controlled_stop_reply(
    const WorkerControlledStopReply& reply) {
    if (reply.safely_stopped && !reply.checkpoint_completed) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.controlled_stop_reply_encode",
            L"A safe stop requires a completed checkpoint");
    }
    std::uint32_t flags = reply.safely_stopped ? 0x01U : 0U;
    if (reply.checkpoint_completed) flags |= 0x02U;
    return encode_worker_control_reply(
        flags, reply.generation,
        L"host_ipc.controlled_stop_reply_encode");
}

Result<WorkerControlledStopReply> decode_worker_controlled_stop_reply(
    const std::span<const std::byte> payload) {
    auto decoded = decode_worker_control_reply(
        payload, 0x03U, L"host_ipc.controlled_stop_reply_decode");
    if (!decoded.ok()) {
        return Result<WorkerControlledStopReply>::failure(decoded.error());
    }
    const bool stopped = (decoded.value().first & 0x01U) != 0;
    const bool checkpoint = (decoded.value().first & 0x02U) != 0;
    if (stopped && !checkpoint) {
        return ipc_error<WorkerControlledStopReply>(
            HostProtocolError::invalid_argument,
            L"host_ipc.controlled_stop_reply_decode",
            L"A safe stop omitted its completed checkpoint proof");
    }
    return Result<WorkerControlledStopReply>::success({
        stopped, checkpoint, decoded.value().second});
}

struct WorkerReattachmentEndpoint::Impl final {
    struct Attachment final {
        HandshakeNonce manager_nonce{};
        WorkerBinding binding;
        SessionSecret derivation_secret;
        std::optional<WorkerHandshake> handshake;
        std::optional<SessionAuthenticator> session;
        bool authenticated{};

        Attachment(
            const HandshakeNonce& manager_nonce_value,
            WorkerBinding binding_value,
            SessionSecret derivation_secret_value,
            WorkerHandshake handshake_value) noexcept
            : manager_nonce(manager_nonce_value),
              binding(std::move(binding_value)),
              derivation_secret(std::move(derivation_secret_value)),
              handshake(std::move(handshake_value)) {}
    };

    Impl(
        ReattachmentCredentialWindow credentials_value,
        AuthenticatedWorkerRequestHandler handler_value) noexcept
        : credentials(std::move(credentials_value)),
          handler(std::move(handler_value)) {}

    mutable std::mutex mutex;
    std::mutex exchange_mutex;
    ReattachmentCredentialWindow credentials;
    AuthenticatedWorkerRequestHandler handler;
    std::optional<HandshakeNonce> manager_owner;
    std::optional<Attachment> attachment;
    bool reauthentication_allowed{};
    bool fresh_session_required{};
    bool finalize_ack_pending{};
    std::uint64_t attachment_epoch{};
};

WorkerReattachmentEndpoint::WorkerReattachmentEndpoint(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WorkerReattachmentEndpoint::~WorkerReattachmentEndpoint() noexcept = default;

Result<std::shared_ptr<WorkerReattachmentEndpoint>>
WorkerReattachmentEndpoint::create(
    WorkerBinding current_binding,
    SessionSecret current_secret,
    AuthenticatedWorkerRequestHandler handler) {
    auto window = ReattachmentCredentialWindow::begin(
        std::move(current_binding), std::move(current_secret));
    if (!window.ok()) {
        return Result<std::shared_ptr<WorkerReattachmentEndpoint>>::failure(
            window.error());
    }
    try {
        auto impl = std::make_unique<Impl>(
            window.move_value(), std::move(handler));
        auto endpoint = std::shared_ptr<WorkerReattachmentEndpoint>(
            new WorkerReattachmentEndpoint(std::move(impl)));
        return Result<std::shared_ptr<WorkerReattachmentEndpoint>>::success(
            std::move(endpoint));
    } catch (...) {
        return ipc_error<std::shared_ptr<WorkerReattachmentEndpoint>>(
            HostProtocolError::invalid_argument,
            L"host_ipc.endpoint_create",
            L"Failed to allocate worker reattachment endpoint");
    }
}

Result<WorkerChallenge> WorkerReattachmentEndpoint::begin_attachment(
    const HandshakeNonce& manager_instance_nonce,
    const ClientHello& hello) {
    std::scoped_lock lock(impl_->mutex);
    if (nonce_is_zero(manager_instance_nonce)
        || !is_valid(hello.binding)) {
        return ipc_error<WorkerChallenge>(
            HostProtocolError::invalid_argument,
            L"host_ipc.endpoint_begin",
            L"Manager nonce or worker binding is invalid");
    }
    const bool existing_owner = impl_->manager_owner.has_value();
    if (existing_owner
        && (impl_->manager_owner.value() != manager_instance_nonce
            || !impl_->reauthentication_allowed)) {
        return ipc_error<WorkerChallenge>(
            HostProtocolError::authentication_failed,
            L"host_ipc.endpoint_begin",
            L"Another manager already owns the worker attachment slot");
    }
    if (!existing_owner) impl_->manager_owner = manager_instance_nonce;
    const auto rollback = [&]() noexcept {
        impl_->attachment.reset();
        if (!existing_owner) impl_->manager_owner.reset();
        impl_->reauthentication_allowed = existing_owner;
    };
    if (!impl_->credentials.accepts(hello.binding)) {
        rollback();
        return ipc_error<WorkerChallenge>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.endpoint_begin",
            L"Worker does not accept this runtime lease binding");
    }
    auto handshake_secret = impl_->credentials.credential_for(hello.binding);
    auto derivation_secret = impl_->credentials.credential_for(hello.binding);
    if (!handshake_secret.ok() || !derivation_secret.ok()) {
        rollback();
        return ipc_error<WorkerChallenge>(
            HostProtocolError::authentication_failed,
            L"host_ipc.endpoint_begin",
            L"Worker credential selection failed");
    }
    auto handshake = WorkerHandshake::begin(
        hello.binding, handshake_secret.move_value());
    if (!handshake.ok()) {
        rollback();
        return Result<WorkerChallenge>::failure(handshake.error());
    }
    auto worker_nonce = generate_handshake_nonce();
    if (!worker_nonce.ok()) {
        rollback();
        return Result<WorkerChallenge>::failure(worker_nonce.error());
    }
    auto challenge = handshake.value().accept_hello(
        hello, worker_nonce.value());
    if (!challenge.ok()) {
        rollback();
        return challenge;
    }
    impl_->attachment.emplace(
        manager_instance_nonce,
        hello.binding,
        derivation_secret.move_value(),
        handshake.move_value());
    ++impl_->attachment_epoch;
    impl_->reauthentication_allowed = false;
    return challenge;
}

Result<void> WorkerReattachmentEndpoint::authenticate_attachment(
    const HandshakeNonce& manager_instance_nonce,
    const ServiceProof& proof) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->manager_owner.has_value()
        || impl_->manager_owner.value() != manager_instance_nonce
        || !impl_->attachment.has_value()
        || !impl_->attachment->handshake.has_value()
        || impl_->attachment->authenticated) {
        return ipc_error<void>(
            HostProtocolError::authentication_failed,
            L"host_ipc.endpoint_authenticate",
            L"Manager does not own an active worker handshake");
    }
    auto session = impl_->attachment->handshake->accept_proof(proof);
    if (!session.ok()) {
        impl_->attachment.reset();
        impl_->manager_owner.reset();
        impl_->reauthentication_allowed = false;
        return Result<void>::failure(session.error());
    }
    const auto* pending = impl_->credentials.pending_binding();
    if (pending != nullptr && *pending == proof.binding) {
        auto committed = impl_->credentials.commit(*pending);
        if (!committed.ok()) {
            impl_->attachment.reset();
            impl_->manager_owner.reset();
            impl_->reauthentication_allowed = false;
            return committed;
        }
        impl_->fresh_session_required = true;
        impl_->finalize_ack_pending = false;
    }
    impl_->attachment->session = session.move_value();
    impl_->attachment->authenticated = true;
    return Result<void>::success();
}

Result<AuthenticatedHostFrame> WorkerReattachmentEndpoint::exchange(
    const HandshakeNonce& manager_instance_nonce,
    const AuthenticatedHostFrame& request) {
    std::scoped_lock exchange_lock(impl_->exchange_mutex);
    std::unique_lock state_lock(impl_->mutex);
    if (!impl_->manager_owner.has_value()
        || impl_->manager_owner.value() != manager_instance_nonce
        || !impl_->attachment.has_value()
        || !impl_->attachment->authenticated
        || !impl_->attachment->session.has_value()) {
        return ipc_error<AuthenticatedHostFrame>(
            HostProtocolError::authentication_failed,
            L"host_ipc.endpoint_exchange",
            L"Manager does not own an authenticated worker session");
    }
    auto opened = impl_->attachment->session->open(request);
    if (!opened.ok()) {
        return Result<AuthenticatedHostFrame>::failure(opened.error());
    }
    const bool rotation_control =
        opened.value().command_id == kReattachmentStateQuery
        || opened.value().command_id == kReattachmentRecoveryFinalize
        || opened.value().command_id == kReattachmentRecoveryConfirm
        || opened.value().command_id == kReattachmentStageCommand
        || opened.value().command_id == kReattachmentCommitCommand;
    if (opened.value().request_id
            != impl_->attachment->binding.profile_id
        || uuid_is_zero(opened.value().operation_id)
        || (rotation_control
            && (opened.value().operation_id
                    != impl_->attachment->binding.image_id
                || opened.value().flags != 0))) {
        return ipc_error<AuthenticatedHostFrame>(
            HostProtocolError::authentication_failed,
            L"host_ipc.endpoint_exchange",
            L"Authenticated worker request correlation is invalid");
    }

    HostMessage response = rotation_message(
        impl_->attachment->binding,
        opened.value().command_id,
        {});
    bool invalidate_old_session = false;
    if (opened.value().command_id == kReattachmentStateQuery) {
        if (!opened.value().payload.empty()) {
            return ipc_error<AuthenticatedHostFrame>(
                HostProtocolError::invalid_argument,
                L"host_ipc.endpoint_state",
                L"Rotation state query payload must be empty");
        }
        auto payload = encode_rotation_state(
            impl_->credentials.current_binding(),
            impl_->credentials.pending_binding(),
            impl_->fresh_session_required || impl_->finalize_ack_pending);
        if (!payload.ok()) {
            return Result<AuthenticatedHostFrame>::failure(payload.error());
        }
        response = rotation_message(
            impl_->credentials.current_binding(),
            kReattachmentStateResponse,
            payload.move_value());
    } else if (opened.value().command_id
               == kReattachmentRecoveryFinalize) {
        auto finalized = decode_rotation_binding(opened.value().payload);
        if (!finalized.ok()) {
            return Result<AuthenticatedHostFrame>::failure(
                finalized.error());
        }
        if (finalized.value() != impl_->credentials.current_binding()) {
            return ipc_error<AuthenticatedHostFrame>(
                HostProtocolError::lease_binding_mismatch,
                L"host_ipc.endpoint_finalize",
                L"Recovery finalize binding is not the current generation");
        }
        auto payload = encode_rotation_binding(finalized.value());
        if (!payload.ok()) {
            return Result<AuthenticatedHostFrame>::failure(payload.error());
        }
        // Keep recovery visible until the service proves it received this ACK
        // by sending the authenticated confirmation below. If this ACK is
        // lost, a later service instance recovers the same N+1 generation.
        impl_->fresh_session_required = false;
        impl_->finalize_ack_pending = true;
        response = rotation_message(
            finalized.value(),
            kReattachmentRecoveryFinalizeAck,
            payload.move_value());
    } else if (opened.value().command_id
               == kReattachmentRecoveryConfirm) {
        auto confirmed = decode_rotation_binding(opened.value().payload);
        if (!confirmed.ok()) {
            return Result<AuthenticatedHostFrame>::failure(
                confirmed.error());
        }
        if (confirmed.value() != impl_->credentials.current_binding()
            || !impl_->finalize_ack_pending) {
            return ipc_error<AuthenticatedHostFrame>(
                HostProtocolError::lease_binding_mismatch,
                L"host_ipc.endpoint_confirm",
                L"Recovery confirmation has no matching finalize ACK");
        }
        auto payload = encode_rotation_binding(confirmed.value());
        if (!payload.ok()) {
            return Result<AuthenticatedHostFrame>::failure(payload.error());
        }
        // Once confirmation is authenticated, the worker knows that the
        // service received the finalize ACK. Losing this response is safe: a
        // future service may advance to N+2 without skipping N+1 recovery.
        impl_->finalize_ack_pending = false;
        response = rotation_message(
            confirmed.value(),
            kReattachmentRecoveryConfirmAck,
            payload.move_value());
    } else if (opened.value().command_id == kReattachmentStageCommand) {
        auto next = decode_rotation_binding(opened.value().payload);
        if (!next.ok()) {
            return Result<AuthenticatedHostFrame>::failure(next.error());
        }
        auto expected_next = next_reattachment_binding(
            impl_->attachment->binding, next.value().profile_revision);
        if (!expected_next.ok() || expected_next.value() != next.value()) {
            return ipc_error<AuthenticatedHostFrame>(
                HostProtocolError::lease_binding_mismatch,
                L"host_ipc.endpoint_stage",
                L"Rotation request is not the exact N+1 binding");
        }
        auto derived = derive_next_reattachment_secret(
            impl_->attachment->derivation_secret, next.value());
        if (!derived.ok()) {
            return Result<AuthenticatedHostFrame>::failure(derived.error());
        }
        const auto* pending = impl_->credentials.pending_binding();
        if (pending == nullptr) {
            auto staged = impl_->credentials.stage(
                next.value(), derived.move_value());
            if (!staged.ok()) {
                return Result<AuthenticatedHostFrame>::failure(
                    staged.error());
            }
        } else {
            if (*pending != next.value()) {
                return ipc_error<AuthenticatedHostFrame>(
                    HostProtocolError::lease_binding_mismatch,
                    L"host_ipc.endpoint_stage",
                    L"Worker already holds another staged successor");
            }
            auto pending_secret = impl_->credentials.credential_for(*pending);
            if (!pending_secret.ok()
                || !digest_equal(
                    pending_secret.value().bytes(), derived.value().bytes())) {
                return ipc_error<AuthenticatedHostFrame>(
                    HostProtocolError::authentication_failed,
                    L"host_ipc.endpoint_stage",
                    L"Recovered staged credential does not match deterministic rotation");
            }
        }
        auto payload = encode_rotation_binding(next.value());
        if (!payload.ok()) {
            return Result<AuthenticatedHostFrame>::failure(payload.error());
        }
        response = rotation_message(
            next.value(), kReattachmentStageAck, payload.move_value());
    } else if (opened.value().command_id
               == kReattachmentCommitCommand) {
        auto next = decode_rotation_binding(opened.value().payload);
        if (!next.ok()) {
            return Result<AuthenticatedHostFrame>::failure(next.error());
        }
        const auto* pending = impl_->credentials.pending_binding();
        if (pending == nullptr || *pending != next.value()) {
            return ipc_error<AuthenticatedHostFrame>(
                HostProtocolError::lease_binding_mismatch,
                L"host_ipc.endpoint_commit",
                L"Rotation commit does not match the staged successor");
        }
        auto committed = impl_->credentials.commit(next.value());
        if (!committed.ok()) {
            return Result<AuthenticatedHostFrame>::failure(committed.error());
        }
        auto payload = encode_rotation_binding(next.value());
        if (!payload.ok()) {
            return Result<AuthenticatedHostFrame>::failure(payload.error());
        }
        response = rotation_message(
            next.value(), kReattachmentCommitAck, payload.move_value());
        impl_->fresh_session_required = true;
        impl_->finalize_ack_pending = false;
        invalidate_old_session = true;
    } else {
        if (!impl_->handler) {
            return ipc_error<AuthenticatedHostFrame>(
                HostProtocolError::invalid_argument,
                L"host_ipc.endpoint_dispatch",
                L"Worker has no handler for this authenticated command");
        }
        const auto attachment_epoch = impl_->attachment_epoch;
        auto application_request = opened.move_value();
        state_lock.unlock();
        Result<HostMessage> handled = [&]() {
            try {
                return impl_->handler(application_request);
            } catch (...) {
                return ipc_error<HostMessage>(
                    HostProtocolError::invalid_argument,
                    L"host_ipc.endpoint_dispatch",
                    L"Worker request handler threw an exception");
            }
        }();
        state_lock.lock();
        if (!impl_->manager_owner.has_value()
            || impl_->manager_owner.value() != manager_instance_nonce
            || impl_->attachment_epoch != attachment_epoch
            || !impl_->attachment.has_value()
            || !impl_->attachment->authenticated
            || !impl_->attachment->session.has_value()) {
            return ipc_error<AuthenticatedHostFrame>(
                HostProtocolError::authentication_failed,
                L"host_ipc.endpoint_dispatch",
                L"Worker attachment changed while handling a request");
        }
        if (!handled.ok()) {
            return Result<AuthenticatedHostFrame>::failure(
                handled.error());
        }
        response = handled.move_value();
    }
    auto sealed = impl_->attachment->session->seal(response);
    if (invalidate_old_session) {
        impl_->attachment->session.reset();
        impl_->attachment->handshake.reset();
        impl_->attachment->authenticated = false;
        impl_->reauthentication_allowed = true;
    }
    return sealed;
}

void WorkerReattachmentEndpoint::disconnect_attachment(
    const HandshakeNonce& manager_instance_nonce) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->manager_owner.has_value()
        || impl_->manager_owner.value() != manager_instance_nonce) {
        return;
    }
    impl_->attachment.reset();
    impl_->manager_owner.reset();
    impl_->reauthentication_allowed = false;
    ++impl_->attachment_epoch;
}

WorkerBinding WorkerReattachmentEndpoint::current_binding() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->credentials.current_binding();
}

bool WorkerReattachmentEndpoint::manager_attached() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->manager_owner.has_value();
}

ServiceReattachmentResult::ServiceReattachmentResult(
    WorkerBinding binding_value,
    HandshakeNonce manager_nonce_value,
    SessionAuthenticator session_value,
    const ServiceReattachmentDisposition disposition,
    std::shared_ptr<IWorkerReattachmentTransport> transport) noexcept
    : binding_(std::move(binding_value)),
      manager_instance_nonce_(manager_nonce_value),
      session_(std::move(session_value)),
      disposition_(disposition),
      transport_(std::move(transport)) {}

ServiceReattachmentResult::~ServiceReattachmentResult() noexcept {
    disconnect();
}

ServiceReattachmentResult::ServiceReattachmentResult(
    ServiceReattachmentResult&& other) noexcept
    : binding_(std::move(other.binding_)),
      manager_instance_nonce_(other.manager_instance_nonce_),
      session_(std::move(other.session_)),
      disposition_(other.disposition_),
      transport_(std::move(other.transport_)) {
    secure_zero(other.manager_instance_nonce_);
}

ServiceReattachmentResult& ServiceReattachmentResult::operator=(
    ServiceReattachmentResult&& other) noexcept {
    if (this != &other) {
        disconnect();
        binding_ = std::move(other.binding_);
        manager_instance_nonce_ = other.manager_instance_nonce_;
        session_ = std::move(other.session_);
        disposition_ = other.disposition_;
        transport_ = std::move(other.transport_);
        secure_zero(other.manager_instance_nonce_);
    }
    return *this;
}

const WorkerBinding& ServiceReattachmentResult::binding() const noexcept {
    return binding_;
}

const HandshakeNonce&
ServiceReattachmentResult::manager_instance_nonce() const noexcept {
    return manager_instance_nonce_;
}

SessionAuthenticator& ServiceReattachmentResult::session() noexcept {
    return session_;
}

ServiceReattachmentDisposition
ServiceReattachmentResult::disposition() const noexcept {
    return disposition_;
}

Result<HostMessage> ServiceReattachmentResult::exchange(
    const HostMessage& request) {
    if (!transport_ || uuid_is_zero(request.request_id)
        || uuid_is_zero(request.operation_id) || request.command_id == 0) {
        return ipc_error<HostMessage>(
            HostProtocolError::invalid_argument,
            L"host_ipc.service_exchange",
            L"Authenticated worker request is absent or incorrectly bound");
    }
    auto sealed = session_.seal(request);
    if (!sealed.ok()) return Result<HostMessage>::failure(sealed.error());
    auto response = transport_->exchange(
        manager_instance_nonce_, sealed.value());
    if (!response.ok()) {
        return Result<HostMessage>::failure(response.error());
    }
    auto opened = session_.open(response.value());
    if (!opened.ok()) return opened;
    if (opened.value().request_id != request.request_id
        || opened.value().operation_id != request.operation_id) {
        return ipc_error<HostMessage>(
            HostProtocolError::authentication_failed,
            L"host_ipc.service_exchange",
            L"Authenticated worker response correlation is invalid");
    }
    return opened;
}

void ServiceReattachmentResult::disconnect() noexcept {
    if (transport_) {
        transport_->disconnect_attachment(manager_instance_nonce_);
        transport_.reset();
    }
    secure_zero(manager_instance_nonce_);
}

Result<ServiceReattachmentResult> reattach_and_rotate_runtime_lease(
    IRuntimeLeaseDirectory& directory,
    std::shared_ptr<IWorkerReattachmentTransport> transport,
    const WorkerBinding& current_binding,
    const std::uint64_t next_profile_revision) {
    if (!transport) {
        return ipc_error<ServiceReattachmentResult>(
            HostProtocolError::invalid_argument,
            L"host_ipc.reattach",
            L"Worker reattachment transport is absent");
    }
    auto lease = load_runtime_lease(directory, current_binding);
    if (!lease.ok()) {
        return Result<ServiceReattachmentResult>::failure(lease.error());
    }
    auto manager_nonce = generate_handshake_nonce();
    if (!manager_nonce.ok()) {
        return Result<ServiceReattachmentResult>::failure(
            manager_nonce.error());
    }
    auto initial = establish_service_attachment(
        *transport,
        manager_nonce.value(),
        current_binding,
        lease.value().secret);
    if (!initial.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(initial.error());
    }
    auto initial_state = query_service_rotation_state(
        *transport,
        manager_nonce.value(),
        current_binding,
        initial.value().session);
    if (!initial_state.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            initial_state.error());
    }
    if (initial_state.value().recovery_required) {
        if (initial_state.value().current != current_binding
            || initial_state.value().pending.has_value()) {
            transport->disconnect_attachment(manager_nonce.value());
            return ipc_error<ServiceReattachmentResult>(
                HostProtocolError::lease_binding_mismatch,
                L"host_ipc.reattach_recover",
                L"Recovered worker generation has contradictory pending state");
        }
        auto finalized = finalize_service_rotation_recovery(
            *transport,
            manager_nonce.value(),
            current_binding,
            initial.value().session);
        if (!finalized.ok()) {
            transport->disconnect_attachment(manager_nonce.value());
            return Result<ServiceReattachmentResult>::failure(
                finalized.error());
        }
        return Result<ServiceReattachmentResult>::success(
            ServiceReattachmentResult(
                current_binding,
                manager_nonce.value(),
                std::move(initial.value().session),
                ServiceReattachmentDisposition::recovered_current,
                std::move(transport)));
    }

    auto next = next_reattachment_binding(
        current_binding, next_profile_revision);
    if (!next.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(next.error());
    }
    if (initial_state.value().pending.has_value()
        && initial_state.value().pending.value() != next.value()) {
        transport->disconnect_attachment(manager_nonce.value());
        return ipc_error<ServiceReattachmentResult>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.reattach_recover",
            L"Worker has a staged successor different from exact N+1");
    }
    auto next_secret = derive_next_reattachment_secret(
        lease.value().secret, next.value());
    if (!next_secret.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            next_secret.error());
    }

    auto next_payload = encode_rotation_binding(next.value());
    if (!next_payload.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            next_payload.error());
    }
    auto stage_frame = initial.value().session.seal(rotation_message(
        current_binding,
        kReattachmentStageCommand,
        next_payload.value()));
    if (!stage_frame.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            stage_frame.error());
    }
    auto stage_response = transport->exchange(
        manager_nonce.value(), stage_frame.value());
    if (!stage_response.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            stage_response.error());
    }
    auto opened_stage = initial.value().session.open(stage_response.value());
    if (!opened_stage.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            opened_stage.error());
    }
    auto valid_stage = validate_rotation_response(
        opened_stage.value(), next.value(), kReattachmentStageAck);
    if (!valid_stage.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            valid_stage.error());
    }

    auto published = publish_runtime_lease_atomically(
        directory, next.value(), next_secret.value());
    if (!published.ok()) {
        const bool indeterminate = published.error().code
            == static_cast<std::uint32_t>(
                HostProtocolError::commit_indeterminate);
        if (!indeterminate) {
            transport->disconnect_attachment(manager_nonce.value());
            return Result<ServiceReattachmentResult>::failure(
                published.error());
        }
        auto visible = load_runtime_lease(directory, next.value());
        if (!visible.ok()
            || !digest_equal(
                visible.value().secret.bytes(),
                next_secret.value().bytes())) {
            transport->disconnect_attachment(manager_nonce.value());
            return Result<ServiceReattachmentResult>::failure(
                published.error());
        }
    }

    auto commit_frame = initial.value().session.seal(rotation_message(
        current_binding,
        kReattachmentCommitCommand,
        next_payload.move_value()));
    if (!commit_frame.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            commit_frame.error());
    }
    auto commit_response = transport->exchange(
        manager_nonce.value(), commit_frame.value());
    if (!commit_response.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            commit_response.error());
    }
    auto opened_commit = initial.value().session.open(commit_response.value());
    if (!opened_commit.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            opened_commit.error());
    }
    auto valid_commit = validate_rotation_response(
        opened_commit.value(), next.value(), kReattachmentCommitAck);
    if (!valid_commit.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            valid_commit.error());
    }

    auto final_session = establish_service_attachment(
        *transport,
        manager_nonce.value(),
        next.value(),
        next_secret.value());
    if (!final_session.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            final_session.error());
    }
    auto final_state = query_service_rotation_state(
        *transport,
        manager_nonce.value(),
        next.value(),
        final_session.value().session);
    if (!final_state.ok()
        || final_state.value().current != next.value()
        || final_state.value().pending.has_value()
        || !final_state.value().recovery_required) {
        transport->disconnect_attachment(manager_nonce.value());
        if (!final_state.ok()) {
            return Result<ServiceReattachmentResult>::failure(
                final_state.error());
        }
        return ipc_error<ServiceReattachmentResult>(
            HostProtocolError::lease_binding_mismatch,
            L"host_ipc.reattach_finalize",
            L"Worker did not prove the exact committed N+1 session");
    }
    auto finalized = finalize_service_rotation_recovery(
        *transport,
        manager_nonce.value(),
        next.value(),
        final_session.value().session);
    if (!finalized.ok()) {
        transport->disconnect_attachment(manager_nonce.value());
        return Result<ServiceReattachmentResult>::failure(
            finalized.error());
    }
    return Result<ServiceReattachmentResult>::success(
        ServiceReattachmentResult(
            next.move_value(),
            manager_nonce.value(),
            std::move(final_session.value().session),
            ServiceReattachmentDisposition::rotated_successor,
            std::move(transport)));
}

namespace {

class LocalDescriptor final {
public:
    explicit LocalDescriptor(void* value) noexcept : value_(value) {}
    ~LocalDescriptor() noexcept {
        if (value_ != nullptr) static_cast<void>(LocalFree(value_));
    }
    LocalDescriptor(const LocalDescriptor&) = delete;
    LocalDescriptor& operator=(const LocalDescriptor&) = delete;

private:
    void* value_{};
};

Result<void> validate_runtime_lease_handle(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tag, sizeof(tag))
        || !GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard))) {
        const auto error = GetLastError();
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_identity",
            L"Failed to inspect runtime lease file identity",
            error);
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || tag.ReparseTag != 0
        || standard.NumberOfLinks != 1
        || standard.EndOfFile.QuadPart < 0
        || standard.EndOfFile.QuadPart
            > static_cast<LONGLONG>(kMaximumRuntimeLeaseBytes)) {
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_identity",
            L"Runtime lease is a directory, reparse point, hard link or oversized");
    }

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    const DWORD security_status = GetSecurityInfo(
        handle,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &dacl,
        nullptr,
        &raw_descriptor);
    if (security_status != ERROR_SUCCESS) {
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_acl",
            L"Failed to inspect runtime lease ACL",
            security_status);
    }
    LocalDescriptor descriptor(raw_descriptor);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid{};
    DWORD system_bytes = static_cast<DWORD>(system_sid.size());
    ACL_SIZE_INFORMATION acl_size{};
    if (!GetSecurityDescriptorControl(
            raw_descriptor, &control, &revision)
        || (control & SE_DACL_PRESENT) == 0
        || (control & SE_DACL_PROTECTED) == 0
        || owner == nullptr || dacl == nullptr || !IsValidAcl(dacl)
        || !CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            system_sid.data(),
            &system_bytes)
        || !EqualSid(owner, system_sid.data())
        || !GetAclInformation(
            dacl, &acl_size, sizeof(acl_size), AclSizeInformation)
        || acl_size.AceCount != 1) {
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_acl",
            L"Runtime lease owner or protected DACL is not SYSTEM-only");
    }
    void* raw_ace = nullptr;
    if (!GetAce(dacl, 0, &raw_ace)) {
        const auto error = GetLastError();
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_acl",
            L"Failed to read runtime lease ACL entry",
            error);
    }
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE
        || header->AceFlags != 0
        || header->AceSize
            < offsetof(ACCESS_ALLOWED_ACE, SidStart) + sizeof(DWORD)) {
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_acl",
            L"Runtime lease ACL entry is malformed");
    }
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    PSID ace_sid = const_cast<DWORD*>(&ace->SidStart);
    if (ace->Mask != FILE_ALL_ACCESS
        || !IsValidSid(ace_sid)
        || !EqualSid(ace_sid, system_sid.data())
        || offsetof(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(ace_sid)
            > header->AceSize) {
        return ipc_error<void>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_acl",
            L"Runtime lease grants access outside LocalSystem");
    }
    return Result<void>::success();
}

Result<std::vector<std::byte>> read_runtime_lease_handle(HANDLE handle) {
    auto valid = validate_runtime_lease_handle(handle);
    if (!valid.ok()) {
        return Result<std::vector<std::byte>>::failure(valid.error());
    }
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard))) {
        const auto error = GetLastError();
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_read",
            L"Failed to read runtime lease file size",
            error);
    }
    const auto size = static_cast<std::size_t>(standard.EndOfFile.QuadPart);
    std::vector<std::byte> bytes;
    try {
        bytes.resize(size);
    } catch (...) {
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_read",
            L"Failed to allocate bounded runtime lease read buffer");
    }
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) {
        const auto error = GetLastError();
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_read",
            L"Failed to seek runtime lease",
            error);
    }
    DWORD read = 0;
    const BOOL read_ok = size == 0 || ReadFile(
        handle,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        &read,
        nullptr);
    if (!read_ok || read != bytes.size()) {
        const auto error = read_ok ? ERROR_HANDLE_EOF : GetLastError();
        return ipc_error<std::vector<std::byte>>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_file_read",
            L"Failed to read complete runtime lease bytes",
            error);
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

std::wstring join_lease_leaf(
    std::wstring_view directory,
    std::wstring_view leaf) {
    std::wstring path(directory);
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    path.append(leaf);
    return path;
}

class NativeRuntimeLeaseDirectory final : public IRuntimeLeaseDirectory {
public:
    explicit NativeRuntimeLeaseDirectory(
        profile::ProfileDirectoryCapability capability) noexcept
        : capability_(std::move(capability)),
          profile_root_(capability_.paths().profile_root) {}

    ~NativeRuntimeLeaseDirectory() noexcept override {
        if (temp_.has_value()) {
            const auto path = temp_->path;
            temp_->handle.reset();
            static_cast<void>(DeleteFileW(path.c_str()));
            temp_.reset();
        }
    }

    Result<RuntimeLeaseTemp> create_secure_temp() override {
        std::scoped_lock lock(mutex_);
        if (temp_.has_value()) {
            return ipc_error<RuntimeLeaseTemp>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_create",
                L"A runtime lease temp is already active");
        }
        auto random = product::Uuid::random();
        if (!random.ok()) {
            return Result<RuntimeLeaseTemp>::failure(random.error());
        }
        const auto narrow = random.value().to_string();
        const std::wstring id(narrow.begin(), narrow.end());
        const auto leaf = L".runtime-lease." + id + L".tmp";
        const auto path = join_lease_leaf(profile_root_, leaf);
        auto security = create_runtime_lease_security_descriptor();
        if (!security.ok()) {
            return Result<RuntimeLeaseTemp>::failure(security.error());
        }
        HANDLE handle = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            static_cast<SECURITY_ATTRIBUTES*>(
                security.value().native_security_attributes()),
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return ipc_error<RuntimeLeaseTemp>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_create",
                L"Failed to create secure runtime lease temp",
                error);
        }
        HandleOwner owner(handle);
        auto valid = validate_runtime_lease_handle(handle);
        if (!valid.ok()) {
            owner.reset();
            static_cast<void>(DeleteFileW(path.c_str()));
            return Result<RuntimeLeaseTemp>::failure(valid.error());
        }
        const RuntimeLeaseTemp token{next_token_++};
        if (token.value == 0) {
            owner.reset();
            static_cast<void>(DeleteFileW(path.c_str()));
            return ipc_error<RuntimeLeaseTemp>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_create",
                L"Runtime lease temp token space was exhausted");
        }
        temp_.emplace(TempState{
            token, path, std::move(owner), false, false, false, {}});
        return Result<RuntimeLeaseTemp>::success(token);
    }

    Result<void> write_temp(
        RuntimeLeaseTemp token,
        std::span<const std::byte> bytes) override {
        std::scoped_lock lock(mutex_);
        if (!matches(token) || bytes.empty()
            || bytes.size() > kMaximumRuntimeLeaseBytes) {
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_write",
                L"Runtime lease temp token or bytes are invalid");
        }
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(
                temp_->handle.get(), start, nullptr, FILE_BEGIN)
            || !SetEndOfFile(temp_->handle.get())) {
            const auto error = GetLastError();
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_write",
                L"Failed to truncate runtime lease temp",
                error);
        }
        DWORD written = 0;
        const BOOL write_ok = WriteFile(
            temp_->handle.get(),
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &written,
            nullptr);
        if (!write_ok || written != bytes.size()) {
            const auto error = write_ok ? ERROR_WRITE_FAULT : GetLastError();
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_write",
                L"Failed to write complete runtime lease temp",
                error);
        }
        temp_->written = true;
        temp_->flushed = false;
        temp_->verified = false;
        temp_->verified_bytes.clear();
        return Result<void>::success();
    }

    Result<void> flush_temp(RuntimeLeaseTemp token) override {
        std::scoped_lock lock(mutex_);
        if (!matches(token) || !temp_->written) {
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_flush",
                L"Runtime lease temp is not ready to flush");
        }
        if (!FlushFileBuffers(temp_->handle.get())) {
            const auto error = GetLastError();
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_flush",
                L"Failed to flush runtime lease temp",
                error);
        }
        temp_->flushed = true;
        return Result<void>::success();
    }

    Result<std::vector<std::byte>> read_temp(
        RuntimeLeaseTemp token) override {
        std::scoped_lock lock(mutex_);
        if (!matches(token) || !temp_->flushed) {
            return ipc_error<std::vector<std::byte>>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_read",
                L"Runtime lease temp is not ready to verify");
        }
        auto bytes = read_runtime_lease_handle(temp_->handle.get());
        if (bytes.ok()) {
            temp_->verified_bytes = bytes.value();
            temp_->verified = true;
        }
        return bytes;
    }

    Result<void> publish_temp(RuntimeLeaseTemp token) override {
        std::scoped_lock lock(mutex_);
        if (!matches(token) || !temp_->verified) {
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_publish",
                L"Runtime lease temp is not verified for publication");
        }
        const auto target = join_lease_leaf(
            profile_root_, kRuntimeLeaseFileName);
        HandleOwner existing(CreateFileW(
            target.c_str(),
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        const bool exists = existing.get() != INVALID_HANDLE_VALUE;
        if (exists) {
            auto valid = validate_runtime_lease_handle(existing.get());
            if (!valid.ok()) return valid;
        } else {
            existing.reset();
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND
                && error != ERROR_PATH_NOT_FOUND) {
                return ipc_error<void>(
                    HostProtocolError::unsafe_runtime_lease,
                    L"host_ipc.lease_temp_publish",
                    L"Failed to inspect current runtime lease",
                    error);
            }
        }
        existing.reset();
        temp_->handle.reset();
        DWORD flags = MOVEFILE_WRITE_THROUGH;
        if (exists) flags |= MOVEFILE_REPLACE_EXISTING;
        if (!MoveFileExW(temp_->path.c_str(), target.c_str(), flags)) {
            const auto error = GetLastError();
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_publish",
                L"Failed to atomically replace runtime lease",
                error);
        }
        HandleOwner published(CreateFileW(
            target.c_str(),
            GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (published.get() == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_publish",
                L"Failed to reopen published runtime lease",
                error);
        }
        auto bytes = read_runtime_lease_handle(published.get());
        if (!bytes.ok()) {
            return Result<void>::failure(bytes.error());
        }
        if (bytes.value() != temp_->verified_bytes) {
            return ipc_error<void>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_temp_publish",
                L"Published runtime lease differs from verified temp");
        }
        temp_.reset();
        return Result<void>::success();
    }

    Result<void> flush_directory() override {
        return capability_.flush_directory();
    }

    Result<std::vector<std::byte>> read_current() override {
        std::scoped_lock lock(mutex_);
        const auto target = join_lease_leaf(
            profile_root_, kRuntimeLeaseFileName);
        HandleOwner handle(CreateFileW(
            target.c_str(),
            GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return ipc_error<std::vector<std::byte>>(
                HostProtocolError::unsafe_runtime_lease,
                L"host_ipc.lease_current_read",
                L"Failed to open current runtime lease",
                error);
        }
        return read_runtime_lease_handle(handle.get());
    }

    void discard_temp(RuntimeLeaseTemp token) noexcept override {
        std::scoped_lock lock(mutex_);
        if (!matches(token)) return;
        const auto path = temp_->path;
        temp_->handle.reset();
        static_cast<void>(DeleteFileW(path.c_str()));
        temp_.reset();
    }

private:
    struct TempState final {
        RuntimeLeaseTemp token;
        std::wstring path;
        HandleOwner handle;
        bool written{};
        bool flushed{};
        bool verified{};
        std::vector<std::byte> verified_bytes;
    };

    bool matches(RuntimeLeaseTemp token) const noexcept {
        return temp_.has_value()
            && temp_->token == token;
    }

    profile::ProfileDirectoryCapability capability_;
    std::wstring profile_root_;
    std::mutex mutex_;
    std::optional<TempState> temp_;
    std::uint64_t next_token_{1};
};

} // namespace

Result<std::unique_ptr<IRuntimeLeaseDirectory>>
open_runtime_lease_directory(
    std::wstring_view trusted_program_data_root,
    const product::Uuid& profile_id) {
    auto capability = profile::ProfileDirectoryCapability::open_existing(
        trusted_program_data_root, profile_id);
    if (!capability.ok()) {
        return Result<std::unique_ptr<IRuntimeLeaseDirectory>>::failure(
            capability.error());
    }
    try {
        auto directory = std::unique_ptr<IRuntimeLeaseDirectory>(
            new NativeRuntimeLeaseDirectory(capability.move_value()));
        return Result<std::unique_ptr<IRuntimeLeaseDirectory>>::success(
            std::move(directory));
    } catch (...) {
        return ipc_error<std::unique_ptr<IRuntimeLeaseDirectory>>(
            HostProtocolError::unsafe_runtime_lease,
            L"host_ipc.lease_directory_open",
            L"Failed to allocate runtime lease directory capability");
    }
}

} // namespace myrd::host
