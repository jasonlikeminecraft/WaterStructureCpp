#pragma once

#include <WaterStructure/result.hpp>
#include <WaterStructure/types.hpp>

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace water_structure {

// Converts the text NBT variants used by vendor formats to an unnamed,
// little-endian compound payload suitable for block entities.
Result<NbtPayload> parse_mianyang_nbt(std::string_view input);
Result<NbtPayload> json_compound_to_nbt(const nlohmann::json& input);

} // namespace water_structure
