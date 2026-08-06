#pragma once

#include <WaterStructure/result.hpp>

#include <filesystem>

namespace water_structure::archive {

Result<void> extract_zip(const std::filesystem::path& archive, const std::filesystem::path& destination);
Result<void> create_zip(const std::filesystem::path& directory, const std::filesystem::path& archive);

} // namespace water_structure::archive
