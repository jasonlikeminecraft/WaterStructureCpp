#pragma once

#include "result.hpp"
#include "types.hpp"

#include <span>
#include <string>
#include <vector>

namespace water_structure {

struct CanonicalNbtField {
    std::string path;
    std::vector<std::uint8_t> value;
};

// Stable, type-preserving encodings used by the Go/C++ manifest oracle.
Result<std::vector<std::uint8_t>> canonical_block_state(const BlockState& state);
Result<std::vector<std::uint8_t>> canonical_nbt(std::span<const std::uint8_t> payload);
Result<std::vector<CanonicalNbtField>> canonical_nbt_fields(std::span<const std::uint8_t> payload);

} // namespace water_structure
