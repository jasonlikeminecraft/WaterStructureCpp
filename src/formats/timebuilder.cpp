#include "timebuilder.hpp"

#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace water_structure {
namespace {

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::int64_t integer(const nlohmann::json& value, std::string_view field)
{
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(number);
        }
    }
    throw std::runtime_error(std::string(field) + " is not an integer");
}

std::int32_t i32(const nlohmann::json& value, std::string_view field)
{
    const auto number = integer(value, field);
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(number);
}

struct Bounds {
    bool populated = false;
    BlockPos minimum{};
    BlockPos maximum{};

    void add(BlockPos pos)
    {
        if (!populated) {
            minimum = maximum = pos;
            populated = true;
            return;
        }
        minimum.x = std::min(minimum.x, pos.x);
        minimum.y = std::min(minimum.y, pos.y);
        minimum.z = std::min(minimum.z, pos.z);
        maximum.x = std::max(maximum.x, pos.x);
        maximum.y = std::max(maximum.y, pos.y);
        maximum.z = std::max(maximum.z, pos.z);
    }

    Size size() const
    {
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (!populated || width <= 0 || height <= 0 || length <= 0 ||
            width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("TimeBuilder bounds are invalid");
        }
        return { static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) };
    }
};

} // namespace

std::uint32_t TimeBuilderStructure::runtime_id(std::string name, std::int64_t aux)
{
    name = trim(name);
    const auto cache_key = name + "|" + std::to_string(aux);
    if (const auto cached = mPaletteCache.find(cache_key); cached != mPaletteCache.end()) return cached->second;

    auto runtime = mRegistry.legacy_runtime_id(name, static_cast<std::uint16_t>(aux));
    if (!runtime) runtime = mRegistry.java_runtime_id(name + " " + std::to_string(aux));
    if (!runtime) runtime = mRegistry.java_runtime_id(name);
    if (!runtime) {
        if (const auto unknown = mRegistry.find("minecraft:unknown")) runtime = *unknown;
        else runtime = mRegistry.register_state({ "minecraft:unknown", {}, 0 });
    }
    mPaletteCache.emplace(cache_key, *runtime);
    return *runtime;
}

Result<void> TimeBuilderStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mPaletteCache.clear();
    mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open TimeBuilder file: " + path.string());
        const auto root = nlohmann::json::parse(input);
        if (!root.is_object() || trim(root.value("version", std::string{})) != "TimeBuilder") {
            throw std::runtime_error("unsupported TimeBuilder version");
        }
        const auto entries = root.find("block");
        if (entries == root.end() || !entries->is_array()) {
            throw std::runtime_error("block is not an array");
        }

        std::map<BlockPos, std::uint32_t, std::less<>> accumulated;
        Bounds bounds;
        for (std::size_t entry_index = 0; entry_index < entries->size(); ++entry_index) {
            try {
                const auto& entry = (*entries)[entry_index];
                if (!entry.is_object()) throw std::runtime_error("entry is not an object");
                const auto name = entry.value("name", std::string{});
                const auto aux = entry.contains("aux") ? integer(entry["aux"], "aux") : 0;
                const auto runtime = runtime_id(name, aux);
                const auto positions = entry.find("pos");
                if (positions == entry.end()) continue;
                if (!positions->is_array()) throw std::runtime_error("pos is not an array");
                for (std::size_t position_index = 0; position_index < positions->size(); ++position_index) {
                    const auto& position = (*positions)[position_index];
                    if (!position.is_array()) throw std::runtime_error("position is not an array");
                    if (position.size() < 3) continue;
                    const BlockPos world{
                        i32(position[0], "x"), i32(position[1], "y"), i32(position[2], "z")
                    };
                    accumulated[world] = runtime;
                    bounds.add(world);
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("block entry " + std::to_string(entry_index) + ": " + error.what());
            }
        }
        if (accumulated.empty() || mPaletteCache.empty()) throw std::runtime_error("TimeBuilder has no valid blocks");
        mStore.set_size(bounds.size());
        for (const auto& [world, runtime] : accumulated) {
            const BlockPos local{
                world.x - bounds.minimum.x, world.y - bounds.minimum.y, world.z - bounds.minimum.z
            };
            mStore.put(local, runtime);
            if (runtime != mRegistry.air_runtime_id()) ++mNonAirBlocks;
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse TimeBuilderV1 failed: " + std::string(error.what()));
    }
}

Result<void> TimeBuilderStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> TimeBuilderStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("TimeBuilderV1 has no Go FromMCWorld capability");
}

} // namespace water_structure
