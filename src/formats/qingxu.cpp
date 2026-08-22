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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace water_structure {
namespace {

std::string_view trim_view(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string trim(std::string_view value)
{
    return std::string(trim_view(value));
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
        bool root_is_object = false;
        std::string root_key;
        nlohmann::json total_blocks_raw;
        std::unordered_map<std::int64_t, std::size_t> chunk_occurrences;
        const auto numbered_root_key = [](std::string_view key)
            -> std::optional<std::int64_t> {
            std::int64_t index = 0;
            const auto converted = std::from_chars(
                key.data(), key.data() + key.size(), index);
            if (converted.ec != std::errc{} ||
                converted.ptr != key.data() + key.size() || index < 0 ||
                std::to_string(index) != key) {
                return std::nullopt;
            }
            return index;
        };

        // totalBlocks is not guaranteed to precede the numbered payload fields.  Read the
        // small piece of metadata in a detached first pass so payloads outside
        // [0,totalBlocks) are never expanded merely because they occur first in the file.
        const auto metadata_callback = [&](int depth, nlohmann::json::parse_event_t event,
                                           nlohmann::json& parsed) -> bool {
            if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                root_is_object = true;
                return true;
            }
            if (depth == 0 && (event == nlohmann::json::parse_event_t::array_start ||
                    event == nlohmann::json::parse_event_t::value)) {
                return false;
            }
            if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                root_key = parsed.get<std::string>();
                return true;
            }
            if (depth == 1 && (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start)) {
                if (root_key == "totalBlocks") {
                    if (event == nlohmann::json::parse_event_t::value) {
                        total_blocks_raw = parsed;
                    } else {
                        total_blocks_raw = event == nlohmann::json::parse_event_t::object_start
                            ? nlohmann::json::object() : nlohmann::json::array();
                    }
                }
                if (const auto index = numbered_root_key(root_key)) {
                    auto& count = chunk_occurrences[*index];
                    if (count == std::numeric_limits<std::size_t>::max()) {
                        throw std::runtime_error("chunk key occurrence count overflow");
                    }
                    ++count;
                }
                return false;
            }
            if (depth == 1 && (event == nlohmann::json::parse_event_t::object_end ||
                    event == nlohmann::json::parse_event_t::array_end)) {
                return false;
            }
            return true;
        };
        const auto discarded_metadata = nlohmann::json::parse(input, metadata_callback);
        (void)discarded_metadata;
        if (!root_is_object || total_blocks_raw.is_null() ||
            (!total_blocks_raw.is_number_integer() && !total_blocks_raw.is_number_unsigned())) {
            throw std::runtime_error("root is missing integer totalBlocks");
        }
        const auto total_blocks = json_integer(total_blocks_raw, "totalBlocks");
        if (total_blocks < 0 || total_blocks > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("totalBlocks is outside the supported range");
        }
        for (auto found = chunk_occurrences.begin(); found != chunk_occurrences.end();) {
            if (found->first >= total_blocks) found = chunk_occurrences.erase(found);
            else ++found;
        }

        struct AccumulatedBlock {
            std::uint32_t runtime_id = 0;
            std::int64_t chunk_index = 0;
            std::int64_t point_index = 0;
        };
        std::map<BlockPos, AccumulatedBlock, std::less<>> accumulated;
        Bounds bounds;
        auto consume_chunk = [&](std::int64_t chunk_index, const std::string& chunk_storage) {
            const auto chunk_key = std::to_string(chunk_index);
            const auto chunk_text = trim_view(chunk_storage);
            if (chunk_text.empty()) return;
            bool chunk_root_is_object = false;
            std::string point_key;
            nlohmann::json total_points_raw;
            std::unordered_map<std::int64_t, std::size_t> occurrences;
            const auto canonical_point_index = [](std::string_view key)
                -> std::optional<std::int64_t> {
                std::int64_t index = 0;
                const auto converted = std::from_chars(
                    key.data(), key.data() + key.size(), index);
                if (converted.ec != std::errc{} ||
                    converted.ptr != key.data() + key.size() || index < 0 ||
                    std::to_string(index) != key) {
                    return std::nullopt;
                }
                return index;
            };

            // The nested chunk is itself JSON stored in a JSON string.  Scan only scalar
            // metadata and key occurrence counts first; retaining the old nested DOM made
            // peak memory proportional to all point strings in the largest chunk.
            const auto metadata_callback = [&](int depth,
                                               nlohmann::json::parse_event_t event,
                                               nlohmann::json& parsed) -> bool {
                if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                    chunk_root_is_object = true;
                    return true;
                }
                if (depth == 0 && (event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::value)) {
                    return false;
                }
                if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                    point_key = parsed.get<std::string>();
                    return true;
                }
                if (depth != 1) return true;
                if (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start) {
                    if (point_key == "totalPoints") {
                        if (event == nlohmann::json::parse_event_t::value) {
                            total_points_raw = parsed;
                        } else {
                            total_points_raw = event == nlohmann::json::parse_event_t::object_start
                                ? nlohmann::json::object() : nlohmann::json::array();
                        }
                    }
                    if (const auto index = canonical_point_index(point_key)) {
                        auto& count = occurrences[*index];
                        if (count == std::numeric_limits<std::size_t>::max()) {
                            throw std::runtime_error("point key occurrence count overflow");
                        }
                        ++count;
                    }
                    return false;
                }
                if (event == nlohmann::json::parse_event_t::object_end ||
                    event == nlohmann::json::parse_event_t::array_end) {
                    return false;
                }
                return true;
            };
            try {
                const auto discarded = nlohmann::json::parse(
                    chunk_text.begin(), chunk_text.end(), metadata_callback);
                (void)discarded;
            } catch (const std::exception& error) {
                throw std::runtime_error("chunk index " + chunk_key + ": " + error.what());
            }
            if (!chunk_root_is_object) {
                throw std::runtime_error("chunk index " + chunk_key +
                    " payload is not an object");
            }
            if (total_points_raw.is_null()) return;
            const auto total_points = json_integer(total_points_raw, "totalPoints");
            if (total_points < 0 || total_points > std::numeric_limits<std::int32_t>::max()) {
                throw std::runtime_error("chunk index " + chunk_key +
                    " totalPoints is invalid");
            }
            for (auto found = occurrences.begin(); found != occurrences.end();) {
                if (found->first >= total_points) found = occurrences.erase(found);
                else ++found;
            }

            auto consume_point = [&](std::int64_t point_index, const nlohmann::json& point) {
                const auto current_point_key = std::to_string(point_index);
                try {
                    if (!point.is_string()) {
                        throw std::runtime_error("block is not a JSON string");
                    }
                    const auto block_text = trim_view(point.get_ref<const std::string&>());
                    if (block_text.empty()) return;
                    const auto block = nlohmann::json::parse(
                        block_text.begin(), block_text.end());
                    if (!block.is_object() || !block.contains("Name") ||
                        !block["Name"].is_string()) {
                        throw std::runtime_error("block is missing Name");
                    }
                    if (!block.contains("X") || !block.contains("Y") ||
                        !block.contains("Z")) {
                        throw std::runtime_error("block is missing coordinates");
                    }
                    const BlockPos world{
                        json_i32(block["X"], "X"), json_i32(block["Y"], "Y"),
                        json_i32(block["Z"], "Z") };
                    const AccumulatedBlock candidate{
                        runtime_id(block["Name"].get<std::string>()), chunk_index, point_index };
                    const auto found = accumulated.find(world);
                    if (found == accumulated.end()) {
                        accumulated.emplace(world, candidate);
                    } else if (std::pair{ found->second.chunk_index, found->second.point_index } <=
                        std::pair{ candidate.chunk_index, candidate.point_index }) {
                        found->second = candidate;
                    }
                    bounds.add(world);
                } catch (const std::exception& error) {
                    throw std::runtime_error("chunk index " + chunk_key +
                        " block index " + current_point_key + ": " + error.what());
                }
            };

            point_key.clear();
            const auto points_callback = [&](int depth,
                                             nlohmann::json::parse_event_t event,
                                             nlohmann::json& parsed) -> bool {
                if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                    return true;
                }
                if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                    point_key = parsed.get<std::string>();
                    return true;
                }
                if (depth != 1) return true;
                if (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start) {
                    const auto point_index = canonical_point_index(point_key);
                    const auto found = point_index ? occurrences.find(*point_index) : occurrences.end();
                    if (found == occurrences.end()) return false;
                    if (--found->second != 0) return false;
                    if (event != nlohmann::json::parse_event_t::value) {
                        throw std::runtime_error(
                            "block index " + std::to_string(*point_index) +
                            " is not a JSON string");
                    }
                    consume_point(*point_index, parsed);
                    return false;
                }
                if (event == nlohmann::json::parse_event_t::object_end ||
                    event == nlohmann::json::parse_event_t::array_end) {
                    return false;
                }
                return true;
            };
            try {
                const auto discarded = nlohmann::json::parse(
                    chunk_text.begin(), chunk_text.end(), points_callback);
                (void)discarded;
            } catch (const std::exception& error) {
                const auto prefix = "chunk index " + chunk_key + ": ";
                if (std::string_view(error.what()).starts_with(prefix)) throw;
                throw std::runtime_error(prefix + error.what());
            }
        };
        input.clear();
        input.seekg(0);
        if (!input) throw std::runtime_error("cannot rewind QingXu file");
        root_is_object = false;
        root_key.clear();
        const auto payload_callback = [&](int depth, nlohmann::json::parse_event_t event,
                                          nlohmann::json& parsed) -> bool {
            if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                root_is_object = true;
                return true;
            }
            if (depth == 0 && (event == nlohmann::json::parse_event_t::array_start ||
                    event == nlohmann::json::parse_event_t::value)) {
                return false;
            }
            if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                root_key = parsed.get<std::string>();
                return true;
            }
            if (depth != 1) return true;
            if (event == nlohmann::json::parse_event_t::value) {
                const auto index = numbered_root_key(root_key);
                const auto occurrence = index ? chunk_occurrences.find(*index) :
                    chunk_occurrences.end();
                if (occurrence != chunk_occurrences.end() && --occurrence->second == 0) {
                    if (!parsed.is_string()) {
                        throw std::runtime_error(
                            "chunk index " + std::to_string(*index) + " is not a JSON string");
                    }
                    consume_chunk(*index, parsed.get_ref<const std::string&>());
                }
                // Every top-level payload is detached as soon as it is complete, avoiding a
                // second copy in a whole-root DOM.
                return false;
            }
            if (event == nlohmann::json::parse_event_t::object_start ||
                event == nlohmann::json::parse_event_t::array_start) {
                const auto index = numbered_root_key(root_key);
                const auto occurrence = index ? chunk_occurrences.find(*index) :
                    chunk_occurrences.end();
                if (occurrence != chunk_occurrences.end() && --occurrence->second == 0) {
                    throw std::runtime_error("chunk index " + std::to_string(*index) +
                        " is not a JSON string");
                }
                return false;
            }
            if (event == nlohmann::json::parse_event_t::object_end ||
                event == nlohmann::json::parse_event_t::array_end) {
                return false;
            }
            return true;
        };
        const auto discarded_root = nlohmann::json::parse(input, payload_callback);
        (void)discarded_root;
        std::unordered_map<std::int64_t, std::size_t>{}.swap(chunk_occurrences);
        if (!root_is_object) throw std::runtime_error("QingXu root is not an object");
        if (accumulated.empty() || mPaletteCache.empty()) throw std::runtime_error("QingXu structure has no valid blocks");
        mStore.set_size(bounds.size());
        while (!accumulated.empty()) {
            auto node = accumulated.extract(accumulated.begin());
            mStore.put(local_position(node.key(), bounds.minimum), node.mapped().runtime_id);
        }
        mNonAirBlocks = mStore.count_non_air();
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
