#pragma once

#include <WaterStructure/result.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>

namespace water_structure {

Result<void> write_fuhong(
    const IStructure& structure,
    RuntimeRegistry& registry,
    StructureId format,
    const std::filesystem::path& output_path);

} // namespace water_structure
