#pragma once

#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>

namespace water_structure {

Result<void> write_mcstructure(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output,
    const ConversionOptions& options = {});

} // namespace water_structure
