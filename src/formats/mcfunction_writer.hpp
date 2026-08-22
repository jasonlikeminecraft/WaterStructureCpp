#pragma once

#include <WaterStructure/result.hpp>
#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <cstddef>

namespace water_structure {

Result<void> write_mcfunction(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path,
    const ConversionOptions& options = {});

} // namespace water_structure
