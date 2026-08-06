#pragma once

#include "result.hpp"
#include "runtime_registry.hpp"
#include "structure.hpp"

#include <filesystem>
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

class FormatRegistry {
public:
    static const std::vector<FormatInfo>& formats();
    static Result<FormatInfo> detect(const std::filesystem::path& path);
    static Result<std::unique_ptr<IStructure>> open(const std::filesystem::path& path, RuntimeRegistry& registry);
    static Result<std::unique_ptr<IStructure>> open_as(
        const std::filesystem::path& path, StructureId format, RuntimeRegistry& registry);
    static Result<void> write(
        const IStructure& structure,
        StructureId format,
        const std::filesystem::path& path,
        RuntimeRegistry& registry);
};

std::string to_string(StructureId id);

} // namespace water_structure
