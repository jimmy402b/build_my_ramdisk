#pragma once

#include "src/common/result.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace myrd::product {

class Uuid final {
public:
    static Result<Uuid> parse(std::string_view canonical);
    static Result<Uuid> random();

    std::string to_string() const;
    std::span<const std::byte, 16> bytes() const noexcept;
    bool operator==(const Uuid&) const noexcept = default;

private:
    Uuid() = default;
    std::array<std::byte, 16> data_{};
};

} // namespace myrd::product
