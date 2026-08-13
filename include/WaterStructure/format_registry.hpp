#pragma once

#include "result.hpp"
#include "runtime_registry.hpp"
#include "structure.hpp"

#include <filesystem>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace water_structure {

struct FormatInfo {
    StructureId id = StructureId::Unknown;
    std::string name;
    std::vector<std::string> extensions;
    bool reader_implemented = false;
    bool writer_implemented = false;
    bool world_import_implemented = false;
    bool world_export_implemented = false;
    std::vector<std::string> magic_signatures;
};

struct ConversionOptions {
    // Zero selects a conservative hardware-derived default. One disables
    // parallel writer stages and is useful for reproducible benchmarks.
    std::size_t thread_count = 0;
    // Zero keeps at most two tasks per worker in flight.
    std::size_t max_in_flight_tasks = 0;
};

struct OpenOptions {
    // Let readers with a dedicated world path defer expensive block and NBT
    // construction until write_to_world() consumes the source stream.
    bool streaming_world_import = false;
};

class FormatRegistry {
public:
    static const std::vector<FormatInfo>& formats();
    static Result<FormatInfo> detect(const std::filesystem::path& path);
    static Result<std::unique_ptr<IStructure>> open(
        const std::filesystem::path& path,
        RuntimeRegistry& registry,
        const OpenOptions& options = {});
    static Result<std::unique_ptr<IStructure>> open_as(
        const std::filesystem::path& path,
        StructureId format,
        RuntimeRegistry& registry,
        const OpenOptions& options = {});
    static Result<void> write(
        const IStructure& structure,
        StructureId format,
        const std::filesystem::path& path,
        RuntimeRegistry& registry,
        const ConversionOptions& options = {});
};

std::string to_string(StructureId id);

} // namespace water_structure
