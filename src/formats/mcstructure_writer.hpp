#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>

namespace water_structure {

Result<void> write_mcstructure(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output);

} // namespace water_structure
