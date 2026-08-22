#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/world.hpp>

#if defined(_WIN32)
#  include <Windows.h>
#  include <Psapi.h>
#else
#  include <sys/resource.h>
#  include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <limits>
#include <sstream>
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
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return 0;
    }
    return counters.PeakWorkingSetSize;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#  if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);
#  else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024u;
#  endif
#endif
}

std::size_t peak_private_usage()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return 0;
    }
    return counters.PeakPagefileUsage;
#else
    // ru_maxrss is the portable resident metric. Linux does not expose a
    // process-private peak through getrusage; leave it unavailable instead of
    // reporting virtual address space as private memory.
    return 0;
#endif
}

std::size_t peak_virtual_usage()
{
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmPeak:")) continue;
        std::istringstream fields(line.substr(7));
        std::size_t kib = 0;
        fields >> kib;
        return kib * 1024u;
    }
#endif
    return 0;
}

template<class Callback>
water_structure::Result<void> visit_chunk_grid(
    water_structure::Size size,
    std::size_t batch_size,
    Callback&& callback)
{
    batch_size = std::max<std::size_t>(batch_size, 1);
    std::vector<water_structure::ChunkPos> batch;
    batch.reserve(batch_size);
    const auto x_count = size.chunk_x_count();
    const auto z_count = size.chunk_z_count();
    for (std::int32_t z = 0; z < z_count; ++z) {
        for (std::int32_t x = 0; x < x_count; ++x) {
            batch.push_back({ x, z });
            if (batch.size() != batch_size) continue;
            auto result = callback(std::span<const water_structure::ChunkPos>(batch));
            if (!result) return result;
            batch.clear();
        }
    }
    if (batch.empty()) return water_structure::Result<void>::success();
    return callback(std::span<const water_structure::ChunkPos>(batch));
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
    const auto x_count = size.chunk_x_count();
    const auto z_count = size.chunk_z_count();
    if (x_count <= 0 || z_count <= 0 ||
        static_cast<std::uint64_t>(x_count) >
            std::numeric_limits<std::uint64_t>::max() /
                static_cast<std::uint64_t>(z_count)) {
        std::cerr << "invalid or overflowing chunk grid\n";
        return 2;
    }
    const auto total_chunks = static_cast<std::uint64_t>(x_count) *
        static_cast<std::uint64_t>(z_count);

    std::uint64_t checksum = 0;
    const auto chunks_start = Clock::now();
    const std::size_t format_batch_size =
        (detected.value().id == water_structure::StructureId::SchemV1 ||
         detected.value().id == water_structure::StructureId::SchemV2)
        ? static_cast<std::size_t>(size.chunk_x_count())
        : 64;
    // Keep the benchmark itself eligible for the 500 MiB runner. The common
    // pipeline uses the same conservative 2 MiB-per-live-chunk estimate.
    constexpr std::size_t kBenchmarkSoftBudget = 450u * 1024u * 1024u;
    constexpr std::size_t kEstimatedBytesPerChunk = 2u * 1024u * 1024u;
    const auto batch_size = std::max<std::size_t>(1, std::min(
        format_batch_size, kBenchmarkSoftBudget / kEstimatedBytesPerChunk));
    const auto visited_chunks = visit_chunk_grid(size, batch_size,
        [&](std::span<const water_structure::ChunkPos> positions) {
        const auto visited = structure.value()->visit_chunks(positions,
            [&checksum](water_structure::ChunkPos pos, const water_structure::ChunkData& chunk) {
                checksum += static_cast<std::uint32_t>(pos.x) + static_cast<std::uint32_t>(pos.z);
                for (const auto& [sub_y, sub_chunk] : chunk.sub_chunks) {
                    checksum += static_cast<std::uint32_t>(sub_y);
                    for (const auto runtime_id : sub_chunk.layer0) checksum += runtime_id;
                    for (const auto runtime_id : sub_chunk.layer1) checksum += runtime_id;
                }
                return water_structure::Result<void>::success();
            });
        if (!visited) return visited;
        structure.value()->release_cached_chunks();
        return water_structure::Result<void>::success();
    });
    if (!visited_chunks) {
        std::cerr << visited_chunks.error() << '\n';
        return 3;
    }
    const auto chunks_elapsed = Clock::now() - chunks_start;

    std::size_t entity_count = 0;
    const auto nbt_start = Clock::now();
    const auto visited_nbt = visit_chunk_grid(size, batch_size,
        [&](std::span<const water_structure::ChunkPos> positions) {
        return structure.value()->visit_chunk_nbt(positions,
            [&entity_count](water_structure::ChunkPos, std::span<const water_structure::BlockEntity> values) {
                entity_count += values.size();
                return water_structure::Result<void>::success();
            });
    });
    if (!visited_nbt) {
        std::cerr << visited_nbt.error() << '\n';
        return 3;
    }
    const auto nbt_elapsed = Clock::now() - nbt_start;

    std::cout << "format=" << structure.value()->name() << '\n'
              << "size=" << size.width << 'x' << size.height << 'x' << size.length << '\n'
              << "chunks=" << total_chunks << " entities=" << entity_count << '\n'
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
        water_structure::ConversionStats world_stats;
        bool world_stats_ready = false;
        water_structure::ConversionCallbacks callbacks;
        callbacks.collect_statistics = true;
        callbacks.soft_memory_budget_bytes = kBenchmarkSoftBudget;
        callbacks.statistics = [&](const water_structure::ConversionStats& value) {
            world_stats = value;
            world_stats_ready = true;
        };
        const auto written = structure.value()->write_to_world(
            world.value(), { 0, -4, 0 }, std::move(callbacks));
        const auto closed = world.value().close();
        const auto write_elapsed = Clock::now() - write_start;
        if (!written || !closed) {
            std::cerr << (!written ? written.error() : closed.error()) << '\n';
            return 4;
        }
        std::cout << "world_open_or_unpack_ms=" << milliseconds(world_open_elapsed) << '\n'
                  << "world_write_and_close_or_repack_ms=" << milliseconds(write_elapsed) << '\n';
        if (world_stats_ready) {
            std::cout << "chunk_materialization_ms=" << world_stats.chunk_materialization_ms << '\n'
                      << "encode_compress_ms=" << world_stats.encode_compress_ms << '\n'
                      << "leveldb_write_ms=" << world_stats.leveldb_write_ms << '\n'
                      << "leveldb_close_ms=" << world_stats.leveldb_close_ms << '\n'
                      << "mcworld_unpack_ms=" << world_stats.mcworld_unpack_ms << '\n'
                      << "mcworld_pack_ms=" << world_stats.mcworld_pack_ms << '\n'
                      << "leveldb_batch_count=" << world_stats.leveldb_batches << '\n'
                      << "temporary_spool_bytes=" << world_stats.temporary_spool_bytes << '\n';
        }
    }
    std::cout << "total_conversion_ms=" << milliseconds(Clock::now() - total_start) << '\n'
              << "peak_working_set_mib=" << (peak_working_set() / (1024.0 * 1024.0)) << '\n'
              << "peak_private_mib=" << (peak_private_usage() / (1024.0 * 1024.0)) << '\n'
              << "peak_virtual_mib=" << (peak_virtual_usage() / (1024.0 * 1024.0)) << '\n';
    return 0;
}
