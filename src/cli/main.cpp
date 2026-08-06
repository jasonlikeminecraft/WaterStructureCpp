#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/world.hpp>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace {

void print_usage()
{
    std::cout << "WaterStructureCpp\n"
                 "  inspect <input>\n"
                 "  to-world <input> <world-directory>\n"
                 "  convert <input> --format <target> --output <path>\n";
}

std::optional<water_structure::StructureId> parse_format(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto& format : water_structure::FormatRegistry::formats()) {
        auto name = format.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (name == value) return format.id;
    }
    return std::nullopt;
}

water_structure::Result<void> load_default_mappings(
    water_structure::RuntimeRegistry& registry,
    const std::filesystem::path& executable)
{
    std::vector<std::filesystem::path> candidates{
        std::filesystem::current_path() / "assets" / "block_mappings_v1.json"
    };
    auto parent = std::filesystem::absolute(executable).parent_path();
    for (int i = 0; i < 6 && !parent.empty(); ++i) {
        candidates.push_back(parent / "assets" / "block_mappings_v1.json");
        parent = parent.parent_path();
    }
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return registry.load_block_mappings(candidate);
        }
    }
    return water_structure::Result<void>::success();
}

int inspect(const std::filesystem::path& path, const std::filesystem::path& executable)
{
    water_structure::RuntimeRegistry registry;
    if (const auto mappings = load_default_mappings(registry, executable); !mappings) {
        std::cerr << mappings.error() << '\n';
        return 2;
    }
    registry.install_as_bwo_resolver();
    const auto detected = water_structure::FormatRegistry::detect(path);
    if (!detected) {
        std::cerr << detected.error() << '\n';
        return 2;
    }

    const auto& info = detected.value();
    if (!info.reader_implemented) {
        std::cout << "format: " << info.name << '\n'
                  << "path: " << path.string() << '\n'
                  << "reader: pending\n"
                  << "writer: " << (info.writer_implemented ? "yes" : "pending") << '\n'
                  << "world import: " << (info.world_import_implemented ? "yes" : "pending") << '\n'
                  << "world export: " << (info.world_export_implemented ? "yes" : "pending") << '\n';
        return 0;
    }

    auto opened = water_structure::FormatRegistry::open(path, registry);
    if (!opened) {
        std::cerr << opened.error() << '\n';
        return 2;
    }
    std::cout << "format: " << opened.value()->name() << '\n'
              << "path: " << path.string() << '\n'
              << "reader: yes\n"
              << "writer: " << (info.writer_implemented ? "yes" : "pending") << '\n'
              << "world import: " << (info.world_import_implemented ? "yes" : "pending") << '\n'
              << "world export: " << (info.world_export_implemented ? "yes" : "pending") << '\n';
    const auto size = opened.value()->size();
    const auto offset = opened.value()->offset();
    const auto non_air = opened.value()->count_non_air_blocks();
    if (!non_air) {
        std::cerr << non_air.error() << '\n';
        return 2;
    }
    std::vector<water_structure::ChunkPos> positions;
    for (std::int32_t x = 0; x < size.chunk_x_count(); ++x) {
        for (std::int32_t z = 0; z < size.chunk_z_count(); ++z) {
            positions.push_back({ x, z });
        }
    }
    const auto entities = opened.value()->get_chunk_nbt(positions);
    if (!entities) {
        std::cerr << entities.error() << '\n';
        return 2;
    }
    std::size_t entity_count = 0;
    for (const auto& [pos, values] : entities.value()) {
        entity_count += values.size();
    }
    std::cout << "size: " << size.width << 'x' << size.height << 'x' << size.length << '\n'
              << "offset: " << offset.x << ',' << offset.y << ',' << offset.z << '\n'
              << "non-air blocks: " << non_air.value() << '\n'
              << "chunks: " << positions.size() << '\n'
              << "block entities: " << entity_count << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string_view command = argv[1];
    if (command == "inspect" && argc == 3) {
        return inspect(argv[2], argv[0]);
    }

    if (command == "to-world" && argc == 4) {
        water_structure::RuntimeRegistry registry;
        if (const auto mappings = load_default_mappings(registry, argv[0]); !mappings) {
            std::cerr << mappings.error() << '\n';
            return 2;
        }
        registry.install_as_bwo_resolver();
        auto reader = water_structure::FormatRegistry::open(argv[2], registry);
        if (!reader) {
            std::cerr << reader.error() << '\n';
            return 2;
        }
        auto world = water_structure::BedrockWorldAdapter::open(argv[3]);
        if (!world) {
            std::cerr << world.error() << '\n';
            return 2;
        }
        std::size_t completed = 0;
        const auto converted = reader.value()->write_to_world(
            world.value(),
            { 0, -4, 0 },
            {
                [](std::size_t total) { std::cout << "chunks: " << total << '\n'; },
                [&completed]() { ++completed; }
            }
        );
        if (!converted) {
            std::cerr << converted.error() << '\n';
            return 3;
        }
        const auto closed = world.value().close();
        if (!closed) {
            std::cerr << closed.error() << '\n';
            return 3;
        }
        std::cout << "converted chunks: " << completed << '\n';
        return 0;
    }

    if (command == "convert" && argc == 7 && std::string_view(argv[3]) == "--format" &&
        std::string_view(argv[5]) == "--output") {
        const auto format = parse_format(argv[4]);
        if (!format) {
            std::cerr << "unknown target format: " << argv[4] << '\n';
            return 2;
        }
        water_structure::RuntimeRegistry registry;
        if (const auto mappings = load_default_mappings(registry, argv[0]); !mappings) {
            std::cerr << mappings.error() << '\n';
            return 2;
        }
        registry.install_as_bwo_resolver();
        auto reader = water_structure::FormatRegistry::open(argv[2], registry);
        if (!reader) {
            std::cerr << reader.error() << '\n';
            return 2;
        }
        const auto written = water_structure::FormatRegistry::write(
            *reader.value(), *format, argv[6], registry);
        if (!written) {
            std::cerr << written.error() << '\n';
            return 3;
        }
        std::cout << "converted " << reader.value()->name() << " -> "
                  << water_structure::to_string(*format) << ": " << argv[6] << '\n';
        return 0;
    }

    print_usage();
    return 1;
}
