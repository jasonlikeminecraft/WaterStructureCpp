#include <WaterStructure/canonical.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/world.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace water_structure;

namespace {

std::int32_t integer(std::string_view text)
{
    std::int32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("invalid integer: " + std::string(text));
    return value;
}

std::string state_text(const RuntimeRegistry& registry, std::uint32_t id)
{
    const auto state = registry.resolve_state(id);
    if (!state) return "<runtime:" + std::to_string(id) + ">";
    std::ostringstream out;
    out << state->name << "{v=" << state->version;
    if (!state->states.empty()) {
        auto properties = state->states;
        std::sort(properties.begin(), properties.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        out << ";p=";
        for (std::size_t i = 0; i < properties.size(); ++i) {
            if (i) out << ',';
            out << properties[i].name << '=' << properties[i].value
                << ":t" << static_cast<int>(properties[i].type);
        }
    }
    out << '}';
    return out.str();
}

struct Difference {
    std::int64_t missing = 0;
    std::int64_t extra = 0;
    std::int64_t changed = 0;
    std::vector<std::string> examples;
};

struct Semantic {
    std::string key;
    std::string display;
    std::uint64_t hash = 0;
    bool air = false;
};

std::uint64_t hash_key(std::string_view key)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : key) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

const Semantic& semantic_for(
    const RuntimeRegistry& registry,
    std::uint32_t id,
    std::unordered_map<std::uint32_t, Semantic>& cache)
{
    if (const auto found = cache.find(id); found != cache.end()) return found->second;
    Semantic result;
    const auto state = registry.resolve_state(id);
    if (state) {
        // World round-trips may legitimately rewrite the Bedrock protocol
        // version while preserving the block state. Compare name/properties,
        // not the version field, for the purpose of detecting lost blocks.
        auto properties = state->states;
        std::sort(properties.begin(), properties.end(), [](const auto& a, const auto& b) {
            if (a.name != b.name) return a.name < b.name;
            if (a.type != b.type) return a.type < b.type;
            return a.value < b.value;
        });
        result.key = state->name;
        for (const auto& property : properties) {
            result.key.push_back('\0');
            result.key += property.name;
            result.key.push_back('\0');
            result.key.push_back(static_cast<char>(property.type));
            result.key.push_back('\0');
            result.key += property.value;
        }
        result.display = state_text(registry, id);
        result.air = state->name == "minecraft:air" && state->states.empty();
    } else {
        result.key = "<runtime:" + std::to_string(id) + ">";
        result.display = result.key;
    }
    result.hash = hash_key(result.key);
    return cache.emplace(id, std::move(result)).first->second;
}

std::int64_t count_non_air(
    const BlockLayer& layer,
    ChunkPos chunk,
    std::int32_t sub_y,
    BlockBox bounds,
    const RuntimeRegistry& registry,
    std::unordered_map<std::uint32_t, Semantic>& cache)
{
    std::int64_t count = 0;
    for (int y = 0; y < 16; ++y) {
        const auto gy = sub_y * 16 + y;
        if (gy < bounds.min.y || gy > bounds.max.y) continue;
        for (int z = 0; z < 16; ++z) {
            const auto gz = chunk.z * 16 + z;
            if (gz < bounds.min.z || gz > bounds.max.z) continue;
            for (int x = 0; x < 16; ++x) {
                const auto gx = chunk.x * 16 + x;
                if (gx < bounds.min.x || gx > bounds.max.x) continue;
                const auto id = layer[static_cast<std::size_t>((y * 16 + z) * 16 + x)];
                if (!semantic_for(registry, id, cache).air) ++count;
            }
        }
    }
    return count;
}

void compare_layer(
    const BlockLayer& source,
    const BlockLayer& target,
    ChunkPos chunk,
    std::int32_t sub_y,
    int layer,
    BlockBox bounds,
    const RuntimeRegistry& registry,
    std::unordered_map<std::uint32_t, Semantic>& cache,
    std::int64_t& compared,
    Difference& difference)
{
    for (int y = 0; y < 16; ++y) {
        const auto gy = sub_y * 16 + y;
        if (gy < bounds.min.y || gy > bounds.max.y) continue;
        for (int z = 0; z < 16; ++z) {
            const auto gz = chunk.z * 16 + z;
            if (gz < bounds.min.z || gz > bounds.max.z) continue;
            for (int x = 0; x < 16; ++x) {
                const auto gx = chunk.x * 16 + x;
                if (gx < bounds.min.x || gx > bounds.max.x) continue;
                ++compared;
                const auto index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
                const auto a = source[index];
                const auto b = target[index];
                const auto& a_semantic = semantic_for(registry, a, cache);
                const auto& b_semantic = semantic_for(registry, b, cache);
                if (a_semantic.hash == b_semantic.hash && a_semantic.key == b_semantic.key) continue;
                const bool a_air = a_semantic.air;
                const bool b_air = b_semantic.air;
                if (a_air && !b_air) ++difference.extra;
                else if (!a_air && b_air) ++difference.missing;
                else ++difference.changed;
                if (difference.examples.size() < 20) {
                    std::ostringstream item;
                    item << "layer=" << layer << " pos=(" << gx << ',' << gy << ',' << gz
                         << ") source=" << a_semantic.display
                         << " target=" << b_semantic.display;
                    difference.examples.push_back(item.str());
                }
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 9) {
        std::cerr << "usage: world_compare <source.mcworld|dir> <target.dir> "
                     "minX minY minZ maxX maxY maxZ\n";
        return 2;
    }
    try {
        const BlockBox bounds{
            { integer(argv[3]), integer(argv[4]), integer(argv[5]) },
            { integer(argv[6]), integer(argv[7]), integer(argv[8]) }
        };
        if (bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z)
            throw std::runtime_error("invalid bounds");

        RuntimeRegistry registry;
        const auto mapping = std::filesystem::path(argv[0]).parent_path().parent_path().parent_path() /
            "assets" / "block_mappings_v1.json";
        if (std::filesystem::is_regular_file(mapping)) {
            const auto loaded = registry.load_block_mappings(mapping);
            if (!loaded) throw std::runtime_error(loaded.error());
        }
        registry.install_as_bwo_resolver();
        auto source_opened = BedrockWorldAdapter::open(argv[1], false);
        if (!source_opened) throw std::runtime_error("source: " + source_opened.error());
        BedrockWorldAdapter source = std::move(source_opened).value();
        auto target_opened = BedrockWorldAdapter::open(argv[2], false);
        if (!target_opened) throw std::runtime_error("target: " + target_opened.error());
        BedrockWorldAdapter target = std::move(target_opened).value();

        const auto floor_div = [](std::int32_t value, std::int32_t divisor) {
            const auto quotient = value / divisor;
            const auto remainder = value % divisor;
            return quotient - (remainder < 0 ? 1 : 0);
        };
        const auto min_chunk_x = floor_div(bounds.min.x, 16);
        const auto max_chunk_x = floor_div(bounds.max.x, 16);
        const auto min_chunk_z = floor_div(bounds.min.z, 16);
        const auto max_chunk_z = floor_div(bounds.max.z, 16);
        const auto min_sub_y = floor_div(bounds.min.y, 16);
        const auto max_sub_y = floor_div(bounds.max.y, 16);
        const auto air = registry.air_runtime_id();
        Difference difference;
        std::unordered_map<std::uint32_t, Semantic> semantic_cache;
        semantic_cache.reserve(65536);
        std::int64_t compared = 0, source_non_air = 0, target_non_air = 0;
        std::int64_t chunks = 0, subchunks = 0;

        for (auto cx = min_chunk_x; cx <= max_chunk_x; ++cx) {
            for (auto cz = min_chunk_z; cz <= max_chunk_z; ++cz) {
                const ChunkPos pos{ cx, cz };
                const auto source_chunk = source.load_chunk_range(pos, min_sub_y, max_sub_y, true);
                if (!source_chunk) throw std::runtime_error("source chunk: " + source_chunk.error());
                const auto target_chunk = target.load_chunk_range(pos, min_sub_y, max_sub_y, true);
                if (!target_chunk) throw std::runtime_error("target chunk: " + target_chunk.error());
                ++chunks;
                for (auto sy = min_sub_y; sy <= max_sub_y; ++sy) {
                    const auto source_it = source_chunk.value().sub_chunks.find(sy);
                    const auto target_it = target_chunk.value().sub_chunks.find(sy);
                    if (source_it == source_chunk.value().sub_chunks.end() &&
                        target_it == target_chunk.value().sub_chunks.end()) continue;
                    ++subchunks;
                    // Missing subchunks are all air; avoid heap-backed temporary arrays.
                    BlockLayer source_air, target_air;
                    if (source_it == source_chunk.value().sub_chunks.end()) source_air.fill(air);
                    if (target_it == target_chunk.value().sub_chunks.end()) target_air.fill(air);
                    const auto& s0 = source_it == source_chunk.value().sub_chunks.end() ? source_air : source_it->second.layer0;
                    const auto& t0 = target_it == target_chunk.value().sub_chunks.end() ? target_air : target_it->second.layer0;
                    const auto& s1 = source_it == source_chunk.value().sub_chunks.end() ? source_air : source_it->second.layer1;
                    const auto& t1 = target_it == target_chunk.value().sub_chunks.end() ? target_air : target_it->second.layer1;
                    source_non_air += count_non_air(s0, pos, sy, bounds, registry, semantic_cache);
                    source_non_air += count_non_air(s1, pos, sy, bounds, registry, semantic_cache);
                    target_non_air += count_non_air(t0, pos, sy, bounds, registry, semantic_cache);
                    target_non_air += count_non_air(t1, pos, sy, bounds, registry, semantic_cache);
                    compare_layer(s0, t0, pos, sy, 0, bounds, registry, semantic_cache, compared, difference);
                    compare_layer(s1, t1, pos, sy, 1, bounds, registry, semantic_cache, compared, difference);
                }
            }
        }
        std::cout << "chunks=" << chunks << " subchunks=" << subchunks << " compared_slots=" << compared << '\n'
                  << "source_non_air=" << source_non_air << " target_non_air=" << target_non_air << '\n'
                  << "missing=" << difference.missing << " extra=" << difference.extra
                  << " changed=" << difference.changed << '\n';
        for (const auto& example : difference.examples) std::cout << example << '\n';
        return (difference.missing || difference.extra || difference.changed) ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
