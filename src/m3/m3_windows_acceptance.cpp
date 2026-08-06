#include "src/m3/m3_windows_acceptance.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>

namespace myrd::m3 {
namespace {

bool named_objects_absent(
    const proxy::ProxyObjectNames& names,
    Error& error) {
    struct Probe final {
        HANDLE handle;
        const wchar_t* operation;
    };
    const std::array<Probe, 5> probes{{
        {OpenMutexW(SYNCHRONIZE, FALSE, names.server.c_str()), L"OpenMutexW"},
        {OpenFileMappingW(FILE_MAP_READ, FALSE, names.mapping.c_str()),
         L"OpenFileMappingW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.request.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.response.c_str()), L"OpenEventW"},
        {OpenEventW(SYNCHRONIZE, FALSE, names.cancel.c_str()), L"OpenEventW"},
    }};
    for (const auto& probe : probes) {
        if (probe.handle != nullptr) {
            CloseHandle(probe.handle);
            return false;
        }
        const auto code = static_cast<std::uint32_t>(GetLastError());
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_INVALID_NAME) {
            error = {ErrorDomain::cleanup, code,
                     probe.operation, L"failed to inspect named object"};
            return false;
        }
    }
    return true;
}

class M3AimProviderLifecycle final : public IM1ProviderLifecycle {
public:
    M3AimProviderLifecycle(
        std::unique_ptr<M3Provider> provider,
        OperationContext context)
        : provider_(std::move(provider)),
          context_(std::move(context)),
          names_(provider_->names()) {}

    Result<void> start() override {
        return provider_->start(context_);
    }

    Result<void> stop_and_release() override {
        const auto stopped = provider_->stop(context_, {});
        final_snapshot_ = provider_->transport_snapshot();
        if (!stopped.ok()) {
            return Result<void>::failure(stopped.error.value_or(Error{
                ErrorDomain::cleanup,
                static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
                L"M3AimProviderLifecycle::stop_and_release",
                L"M3 provider stop failed without an error"}));
        }
        return Result<void>::success();
    }

    std::wstring native_object_path() const override {
        return names_.driver_path;
    }

    proxy::SessionSnapshot snapshot() const override {
        return final_snapshot_.value_or(provider_->transport_snapshot());
    }

    Result<bool> named_objects_released() const override {
        Error error{};
        const bool absent = named_objects_absent(names_, error);
        if (!absent && !error.operation.empty()) {
            return Result<bool>::failure(std::move(error));
        }
        return Result<bool>::success(absent);
    }

private:
    std::unique_ptr<M3Provider> provider_;
    OperationContext context_;
    proxy::ProxyObjectNames names_;
    std::optional<proxy::SessionSnapshot> final_snapshot_;
};

} // namespace

M3AimProviderFactory::M3AimProviderFactory(
    M3AimProviderFactoryConfig config)
    : config_(std::move(config)) {}

Result<std::unique_ptr<IM1ProviderLifecycle>>
M3AimProviderFactory::create() {
    auto checked = check_operation_context(config_.context);
    if (!checked) {
        return Result<std::unique_ptr<IM1ProviderLifecycle>>::failure(
            checked.error());
    }
    auto provider = M3Provider::open(config_.context, config_.provider);
    if (!provider) {
        return Result<std::unique_ptr<IM1ProviderLifecycle>>::failure(
            provider.error());
    }
    std::unique_ptr<IM1ProviderLifecycle> lifecycle =
        std::make_unique<M3AimProviderLifecycle>(
            provider.move_value(), config_.context);
    return Result<std::unique_ptr<IM1ProviderLifecycle>>::success(
        std::move(lifecycle));
}

} // namespace myrd::m3
