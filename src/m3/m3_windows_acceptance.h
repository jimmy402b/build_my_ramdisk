#pragma once

#include "src/m1/m1_lifecycle_runner.h"
#include "src/m3/m3_provider.h"

#include <memory>

namespace myrd::m3 {

struct M3AimProviderFactoryConfig final {
    OperationContext context{};
    M3ProviderConfig provider{};
};

// Composition root adapter: the M3 provider owns the M1-compatible SHM
// endpoint and exposes its driver object path to the AIM lifecycle runner.
// No AIM or Windows disk mutation occurs in this factory itself.
class M3AimProviderFactory final : public IM1ProviderFactory {
public:
    explicit M3AimProviderFactory(M3AimProviderFactoryConfig config);

    Result<std::unique_ptr<IM1ProviderLifecycle>> create() override;

private:
    M3AimProviderFactoryConfig config_;
};

} // namespace myrd::m3
