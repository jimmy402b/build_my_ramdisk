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

    // **A default-constructed uuid is the nil uuid, and it is public since T3-01.**
    //
    // It was private so that no durable record could be written without an identifier, which
    // is the right rule where identity is the subject.  A *wire* struct is not such a place:
    // it is default-constructed and then filled field by field, and a private default
    // constructor forced every such struct to be built in one expression.  Nil still means "no
    // identifier", and the service never writes a nil request id.
    Uuid() = default;

private:
    std::array<std::byte, 16> data_{};
};

} // namespace myrd::product
