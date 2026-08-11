#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/world.hpp>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <span>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::filesystem::path find_mapping_asset(const std::filesystem::path& executable)
{
    std::vector<std::filesystem::path> roots{ std::filesystem::current_path() };
    auto parent = std::filesystem::absolute(executable).parent_path();
    for (int i = 0; i < 6 && !parent.empty(); ++i) {
        roots.push_back(parent);
        parent = parent.parent_path();
    }
    for (const auto& root : roots) {
        const auto candidate = root / "assets" / "block_mappings_v1.json";
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return {};
}

std::size_t peak_working_set()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return 0;
    }
    return counters.PeakWorkingSetSize;
}

std::size_t peak_private_usage()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return 0;
    }
    return counters.PeakPagefileUsage;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: water_structure_bench <input> [world-directory]\n";
        return 1;
    }

    const auto total_start = Clock::now();
    const auto detect_start = Clock::now();
    const auto detected = water_structure::FormatRegistry::detect(argv[1]);
    const auto detect_elapsed = Clock::now() - detect_start;
    if (!detected) {
        std::cerr << detected.error() << '\n';
        return 2;
    }

    water_structure::RuntimeRegistry registry;
    const auto mapping_path = find_mapping_asset(argv[0]);
    const auto mapping_start = Clock::now();
    if (!mapping_path.empty()) {
        const auto loaded = registry.load_block_mappings(mapping_path);
        if (!loaded) {
            std::cerr << loaded.error() << '\n';
            return 2;
        }
    }
    const auto mapping_elapsed = Clock::now() - mapping_start;
    registry.install_as_bwo_resolver();

    const auto parse_start = Clock::now();
    auto structure = water_structure::FormatRegistry::open(argv[1], registry);
    const auto parse_elapsed = Clock::now() - parse_start;
    if (!structure) {
        std::cerr << structure.error() << '\n';
        return 2;
    }

    const auto size = structure.value()->size();
    std::vector<water_structure::ChunkPos> positions;
    for (std::int32_t z = 0; z < size.chunk_z_count(); ++z) {
        for (std::int32_t x = 0; x < size.chunk_x_count(); ++x) positions.push_back({ x, z });
    }

    std::uint64_t checksum = 0;
    const auto chunks_start = Clock::now();
    const std::size_t batch_size =
        (detected.value().id == water_structure::StructureId::SchemV1 ||
         detected.value().id == water_structure::StructureId::SchemV2)
        ? static_cast<std::size_t>(size.chunk_x_count())
        : 64;
    for (std::size_t begin = 0; begin < positions.size(); begin += batch_size) {
        const auto count = std::min(batch_size, positions.size() - begin);
        const auto visited = structure.value()->visit_chunks(
            std::span<const water_structure::ChunkPos>(positions).subspan(begin, count),
            [&checksum](water_structure::ChunkPos pos, const water_structure::ChunkData& chunk) {
                checksum += static_cast<std::uint32_t>(pos.x) + static_cast<std::uint32_t>(pos.z);
                for (const auto& [sub_y, sub_chunk] : chunk.sub_chunks) {
                    checksum += static_cast<std::uint32_t>(sub_y);
                    for (const auto runtime_id : sub_chunk.layer0) checksum += runtime_id;
                    for (const auto runtime_id : sub_chunk.layer1) checksum += runtime_id;
                }
                return water_structure::Result<void>::success();
            });
        if (!visited) {
            std::cerr << visited.error() << '\n';
            return 3;
        }
        structure.value()->release_cached_chunks();
    }
    const auto chunks_elapsed = Clock::now() - chunks_start;

    std::size_t entity_count = 0;
    const auto nbt_start = Clock::now();
    for (std::size_t begin = 0; begin < positions.size(); begin += batch_size) {
        const auto count = std::min(batch_size, positions.size() - begin);
        const auto visited = structure.value()->visit_chunk_nbt(
            std::span<const water_structure::ChunkPos>(positions).subspan(begin, count),
            [&entity_count](water_structure::ChunkPos, std::span<const water_structure::BlockEntity> values) {
                entity_count += values.size();
                return water_structure::Result<void>::success();
            });
        if (!visited) {
            std::cerr << visited.error() << '\n';
            return 3;
        }
    }
    const auto nbt_elapsed = Clock::now() - nbt_start;

    std::cout << "format=" << structure.value()->name() << '\n'
              << "size=" << size.width << 'x' << size.height << 'x' << size.length << '\n'
              << "chunks=" << positions.size() << " entities=" << entity_count << '\n'
              << "detect_ms=" << milliseconds(detect_elapsed) << '\n'
              << "mapping_ms=" << milliseconds(mapping_elapsed) << '\n'
              << "parse_ms=" << milliseconds(parse_elapsed) << '\n'
              << "get_chunks_ms=" << milliseconds(chunks_elapsed) << '\n'
              << "nbt_ms=" << milliseconds(nbt_elapsed) << '\n'
              << "checksum=" << checksum << '\n';

    if (argc == 3) {
        const auto world_open_start = Clock::now();
        auto world = water_structure::BedrockWorldAdapter::open(argv[2]);
        const auto world_open_elapsed = Clock::now() - world_open_start;
        if (!world) {
            std::cerr << world.error() << '\n';
            return 4;
        }
        const auto write_start = Clock::now();
        const auto written = structure.value()->write_to_world(world.value(), { 0, -4, 0 }, {});
        const auto closed = world.value().close();
        const auto write_elapsed = Clock::now() - write_start;
        if (!written || !closed) {
            std::cerr << (!written ? written.error() : closed.error()) << '\n';
            return 4;
        }
        std::cout << "world_open_or_unpack_ms=" << milliseconds(world_open_elapsed) << '\n'
                  << "world_write_and_close_or_repack_ms=" << milliseconds(write_elapsed) << '\n';
    }
    std::cout << "total_conversion_ms=" << milliseconds(Clock::now() - total_start) << '\n'
              << "peak_working_set_mib=" << (peak_working_set() / (1024.0 * 1024.0)) << '\n'
              << "peak_private_mib=" << (peak_private_usage() / (1024.0 * 1024.0)) << '\n';
    return 0;
}
