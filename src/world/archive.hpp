#pragma once

#include <WaterStructure/result.hpp>

#include <filesystem>
#include <cstdint>

namespace water_structure::archive {

Result<void> extract_zip(
    const std::filesystem::path& archive,
    const std::filesystem::path& destination,
    std::uint64_t maximum_uncompressed_bytes = 0);
Result<void> create_zip(
    const std::filesystem::path& directory,
    const std::filesystem::path& archive,
    std::uint64_t maximum_temporary_bytes = 0);

} // namespace water_structure::archive
