#include "qingxu.hpp"

#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
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

std::string reverse_split(std::string value)
{
    const auto separator = value.find('.');
    if (separator == std::string::npos) return value;
    return value.substr(separator + 1) + "_" + value.substr(0, separator);
}

std::int64_t json_integer(const nlohmann::json& value, std::string_view field)
{
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(number);
        }
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (std::isfinite(number) &&
            number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(number);
        }
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        std::int64_t number = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return number;
    }
    throw std::runtime_error(std::string(field) + " is not an integer");
}

std::int32_t json_i32(const nlohmann::json& value, std::string_view field)
{
    const auto number = json_integer(value, field);
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
            throw std::runtime_error("QingXu bounds are invalid");
        }
        return { static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) };
    }
};

BlockPos local_position(BlockPos world, BlockPos origin)
{
    return { world.x - origin.x, world.y - origin.y, world.z - origin.z };
}

} // namespace

std::uint32_t QingXuStructure::runtime_id(std::string name)
{
    name = reverse_split(trim(name));
    if (name.empty()) {
        if (const auto unknown = mRegistry.find("minecraft:unknown")) return *unknown;
        return mRegistry.register_state({ "minecraft:unknown", {}, 0 });
    }
    if (const auto cached = mPaletteCache.find(name); cached != mPaletteCache.end()) return cached->second;

    std::optional<std::uint32_t> runtime = mRegistry.java_runtime_id(name);
    if (!runtime) runtime = mRegistry.legacy_runtime_id(name, 0);
    if (!runtime) {
        if (const auto unknown = mRegistry.find("minecraft:unknown")) runtime = *unknown;
        else runtime = mRegistry.register_state({ "minecraft:unknown", {}, 0 });
    }
    mPaletteCache.emplace(std::move(name), *runtime);
    return *runtime;
}

Result<void> QingXuStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mPaletteCache.clear();
    mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open QingXu file: " + path.string());
        const auto root = nlohmann::json::parse(input);
        if (!root.is_object() || !root.contains("totalBlocks") ||
            (!root["totalBlocks"].is_number_integer() && !root["totalBlocks"].is_number_unsigned())) {
            throw std::runtime_error("root is missing integer totalBlocks");
        }
        const auto total_blocks = json_integer(root["totalBlocks"], "totalBlocks");
        if (total_blocks < 0 || total_blocks > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("totalBlocks is outside the supported range");
        }

        std::map<BlockPos, std::uint32_t, std::less<>> accumulated;
        Bounds bounds;
        for (std::int64_t chunk_index = 0; chunk_index < total_blocks; ++chunk_index) {
            const auto chunk_key = std::to_string(chunk_index);
            const auto found_chunk = root.find(chunk_key);
            if (found_chunk == root.end()) continue;
            if (!found_chunk->is_string()) {
                throw std::runtime_error("chunk index " + chunk_key + " is not a JSON string");
            }
            const auto chunk_text = trim(found_chunk->get<std::string>());
            if (chunk_text.empty()) continue;
            nlohmann::json chunk;
            try {
                chunk = nlohmann::json::parse(chunk_text);
            } catch (const std::exception& error) {
                throw std::runtime_error("chunk index " + chunk_key + ": " + error.what());
            }
            if (!chunk.is_object()) throw std::runtime_error("chunk index " + chunk_key + " payload is not an object");
            const auto points = chunk.find("totalPoints");
            if (points == chunk.end()) continue;
            const auto total_points = json_integer(*points, "totalPoints");
            if (total_points < 0 || total_points > std::numeric_limits<std::int32_t>::max()) {
                throw std::runtime_error("chunk index " + chunk_key + " totalPoints is invalid");
            }
            for (std::int64_t point_index = 0; point_index < total_points; ++point_index) {
                const auto point_key = std::to_string(point_index);
                const auto found_point = chunk.find(point_key);
                if (found_point == chunk.end()) continue;
                try {
                    if (!found_point->is_string()) throw std::runtime_error("block is not a JSON string");
                    const auto block_text = trim(found_point->get<std::string>());
                    if (block_text.empty()) continue;
                    const auto block = nlohmann::json::parse(block_text);
                    if (!block.is_object() || !block.contains("Name") || !block["Name"].is_string()) {
                        throw std::runtime_error("block is missing Name");
                    }
                    if (!block.contains("X") || !block.contains("Y") || !block.contains("Z")) {
                        throw std::runtime_error("block is missing coordinates");
                    }
                    const BlockPos world{
                        json_i32(block["X"], "X"), json_i32(block["Y"], "Y"), json_i32(block["Z"], "Z")
                    };
                    accumulated[world] = runtime_id(block["Name"].get<std::string>());
                    bounds.add(world);
                } catch (const std::exception& error) {
                    throw std::runtime_error("chunk index " + chunk_key + " block index " +
                        point_key + ": " + error.what());
                }
            }
        }
        if (accumulated.empty() || mPaletteCache.empty()) throw std::runtime_error("QingXu structure has no valid blocks");
        mStore.set_size(bounds.size());
        for (const auto& [world, runtime] : accumulated) {
            mStore.put(local_position(world, bounds.minimum), runtime);
            if (runtime != mRegistry.air_runtime_id()) ++mNonAirBlocks;
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse QingXuV1 failed: " + std::string(error.what()));
    }
}

Result<void> QingXuStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> QingXuStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("QingXuV1 has no Go FromMCWorld capability");
}

} // namespace water_structure
