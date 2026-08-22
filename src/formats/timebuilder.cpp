#include "timebuilder.hpp"

#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

        bool root_is_object = false;
        bool block_is_array = false;
        std::string root_key;
        nlohmann::json version;
        std::size_t block_member_occurrences = 0;
        const auto metadata_callback = [&](int depth, nlohmann::json::parse_event_t event,
                                           nlohmann::json& parsed) -> bool {
            if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                root_is_object = true;
                return true;
            }
            if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                root_key = parsed.get<std::string>();
                return true;
            }
            if (depth != 1) {
                // A non-object root is invalid; discard it instead of building its DOM.
                if (depth == 0 && (event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::value)) return false;
                return true;
            }
            if (root_key == "version") {
                if (event == nlohmann::json::parse_event_t::value) version = parsed;
                else if (event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start) {
                    version = nlohmann::json{};
                }
                return false;
            }
            if (root_key == "block") {
                if (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start) {
                    if (block_member_occurrences ==
                        std::numeric_limits<std::size_t>::max()) {
                        throw std::runtime_error("block occurrence count overflow");
                    }
                    ++block_member_occurrences;
                    block_is_array = event == nlohmann::json::parse_event_t::array_start;
                }
                return false;
            }
            // Unknown root members are irrelevant and are discarded immediately.
            if (event == nlohmann::json::parse_event_t::value ||
                event == nlohmann::json::parse_event_t::object_start ||
                event == nlohmann::json::parse_event_t::array_start ||
                event == nlohmann::json::parse_event_t::object_end ||
                event == nlohmann::json::parse_event_t::array_end) return false;
            return true;
        };
        const auto discarded_metadata = nlohmann::json::parse(input, metadata_callback);
        (void)discarded_metadata;
        if (!root_is_object || !version.is_string() ||
            trim(version.get_ref<const std::string&>()) != "TimeBuilder") {
            throw std::runtime_error("unsupported TimeBuilder version");
        }
        if (!block_is_array || block_member_occurrences == 0) {
            throw std::runtime_error("block is not an array");
        }

        struct EntryMetadata {
            nlohmann::json name;
            nlohmann::json aux;
            bool has_name = false;
            bool has_aux = false;
            bool has_pos = false;
            bool final_pos_is_array = false;
            std::size_t pos_occurrences = 0;
            std::uint32_t runtime_id = 0;
        };
        std::vector<EntryMetadata> entries;

        // Scan only the small name/aux fields and the final type/occurrence of pos.  Position
        // arrays are discarded at SAX depth 4, so one block type may contain millions of
        // coordinates without creating a correspondingly large nlohmann DOM.
        input.clear();
        input.seekg(0);
        if (!input) throw std::runtime_error("cannot rewind TimeBuilder file");
        {
            bool inside_block_array = false;
            bool active_block_array = false;
            bool inside_entry = false;
            bool inside_pos_array = false;
            std::string current_root_key;
            std::string entry_key;
            std::size_t root_block_occurrence = 0;
            EntryMetadata current;
            const auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                                      nlohmann::json& parsed) -> bool {
                if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                    return true;
                }
                if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                    current_root_key = parsed.get<std::string>();
                    return true;
                }
                if (depth == 1 && current_root_key == "block" &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start)) {
                    ++root_block_occurrence;
                    inside_block_array =
                        event == nlohmann::json::parse_event_t::array_start;
                    active_block_array = inside_block_array &&
                        root_block_occurrence == block_member_occurrences;
                    return inside_block_array;
                }
                if (inside_block_array && !active_block_array && depth == 2 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                if (inside_block_array && depth == 2 &&
                    event == nlohmann::json::parse_event_t::object_start) {
                    if (!active_block_array) return false;
                    inside_entry = true;
                    current = {};
                    return true;
                }
                if (inside_block_array && depth == 2 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::array_start)) {
                    if (!active_block_array) return false;
                    throw std::runtime_error(
                        "block entry " + std::to_string(entries.size()) +
                        ": entry is not an object");
                }
                if (inside_entry && depth == 3 &&
                    event == nlohmann::json::parse_event_t::key) {
                    entry_key = parsed.get<std::string>();
                    return true;
                }
                if (inside_entry && depth == 3 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start)) {
                    if (entry_key == "name") {
                        current.has_name = true;
                        current.name = event == nlohmann::json::parse_event_t::value
                            ? parsed
                            : (event == nlohmann::json::parse_event_t::object_start
                                ? nlohmann::json::object() : nlohmann::json::array());
                        return false;
                    }
                    if (entry_key == "aux") {
                        current.has_aux = true;
                        current.aux = event == nlohmann::json::parse_event_t::value
                            ? parsed
                            : (event == nlohmann::json::parse_event_t::object_start
                                ? nlohmann::json::object() : nlohmann::json::array());
                        return false;
                    }
                    if (entry_key == "pos") {
                        current.has_pos = true;
                        if (current.pos_occurrences ==
                            std::numeric_limits<std::size_t>::max()) {
                            throw std::runtime_error("pos occurrence count overflow");
                        }
                        ++current.pos_occurrences;
                        current.final_pos_is_array =
                            event == nlohmann::json::parse_event_t::array_start;
                        inside_pos_array = current.final_pos_is_array;
                        return inside_pos_array;
                    }
                    return false;
                }
                if (inside_pos_array && depth == 4 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                if (inside_pos_array && depth == 3 &&
                    event == nlohmann::json::parse_event_t::array_end) {
                    inside_pos_array = false;
                    return false;
                }
                if (inside_entry && depth == 2 &&
                    event == nlohmann::json::parse_event_t::object_end) {
                    inside_entry = false;
                    entries.push_back(std::move(current));
                    return false;
                }
                if (inside_block_array && depth == 1 &&
                    event == nlohmann::json::parse_event_t::array_end) {
                    inside_block_array = false;
                    active_block_array = false;
                    return false;
                }
                if (depth == 1 && (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                return true;
            };
            const auto discarded = nlohmann::json::parse(input, callback);
            (void)discarded;
        }

        for (std::size_t index = 0; index < entries.size(); ++index) {
            auto& entry = entries[index];
            try {
                const auto name = entry.has_name
                    ? entry.name.get<std::string>() : std::string{};
                const auto aux = entry.has_aux ? integer(entry.aux, "aux") : 0;
                if (entry.has_pos && !entry.final_pos_is_array) {
                    throw std::runtime_error("pos is not an array");
                }
                entry.runtime_id = runtime_id(name, aux);
                // Release detached scalar JSON strings before the large coordinate passes.
                entry.name = {};
                entry.aux = {};
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    "block entry " + std::to_string(index) + ": " + error.what());
            }
        }

        Bounds bounds;
        // Stream only the final pos member of each entry.  Returning false after each
        // position detaches its tiny three-number array before the next coordinate arrives.
        auto stream_positions = [&](const bool materialize) -> std::size_t {
            input.clear();
            input.seekg(0);
            if (!input) throw std::runtime_error("cannot rewind TimeBuilder file");

            bool inside_block_array = false;
            bool active_block_array = false;
            bool inside_entry = false;
            bool inside_selected_pos = false;
            std::string current_root_key;
            std::string entry_key;
            std::size_t entry_index = 0;
            std::size_t root_block_occurrence = 0;
            std::size_t pos_occurrence = 0;
            std::size_t valid_position_count = 0;
            auto consume_position = [&](const nlohmann::json& position) {
                try {
                    if (!position.is_array()) {
                        throw std::runtime_error("position is not an array");
                    }
                    if (position.size() < 3) return;
                    const BlockPos world{
                        i32(position[0], "x"), i32(position[1], "y"),
                        i32(position[2], "z")
                    };
                    if (valid_position_count >= static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
                        throw std::runtime_error(
                            "valid position count exceeds uint32 capacity");
                    }
                    ++valid_position_count;
                    if (materialize) {
                        const BlockPos local{
                            world.x - bounds.minimum.x,
                            world.y - bounds.minimum.y,
                            world.z - bounds.minimum.z
                        };
                        mStore.put(local, entries[entry_index].runtime_id);
                    } else {
                        bounds.add(world);
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error(
                        "block entry " + std::to_string(entry_index) + ": " + error.what());
                }
            };
            const auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                                      nlohmann::json& parsed) -> bool {
                if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                    return true;
                }
                if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                    current_root_key = parsed.get<std::string>();
                    return true;
                }
                if (depth == 1 && current_root_key == "block" &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start)) {
                    ++root_block_occurrence;
                    inside_block_array =
                        event == nlohmann::json::parse_event_t::array_start;
                    active_block_array = inside_block_array &&
                        root_block_occurrence == block_member_occurrences;
                    return inside_block_array;
                }
                if (inside_block_array && !active_block_array && depth == 2 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                if (inside_block_array && depth == 2 &&
                    event == nlohmann::json::parse_event_t::object_start) {
                    if (!active_block_array) return false;
                    if (entry_index >= entries.size()) {
                        throw std::runtime_error("TimeBuilder changed while parsing");
                    }
                    inside_entry = true;
                    pos_occurrence = 0;
                    return true;
                }
                if (inside_entry && depth == 3 &&
                    event == nlohmann::json::parse_event_t::key) {
                    entry_key = parsed.get<std::string>();
                    return true;
                }
                if (inside_entry && depth == 3 && entry_key == "pos" &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start)) {
                    ++pos_occurrence;
                    inside_selected_pos =
                        event == nlohmann::json::parse_event_t::array_start &&
                        pos_occurrence == entries[entry_index].pos_occurrences;
                    return inside_selected_pos;
                }
                if (inside_selected_pos && depth == 4) {
                    if (event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start) {
                        return true;
                    }
                    if (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end) {
                        consume_position(parsed);
                        return false;
                    }
                }
                if (inside_selected_pos && depth == 3 &&
                    event == nlohmann::json::parse_event_t::array_end) {
                    inside_selected_pos = false;
                    return false;
                }
                if (inside_entry && depth == 3 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                if (inside_entry && depth == 2 &&
                    event == nlohmann::json::parse_event_t::object_end) {
                    inside_entry = false;
                    ++entry_index;
                    return false;
                }
                if (inside_block_array && depth == 1 &&
                    event == nlohmann::json::parse_event_t::array_end) {
                    inside_block_array = false;
                    active_block_array = false;
                    return false;
                }
                if (depth == 1 && (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                return true;
            };
            const auto discarded_root = nlohmann::json::parse(input, callback);
            (void)discarded_root;
            if (entry_index != entries.size()) {
                throw std::runtime_error("TimeBuilder changed while parsing");
            }
            return valid_position_count;
        };

        const auto valid_positions = stream_positions(false);
        if (valid_positions == 0 || mPaletteCache.empty()) {
            throw std::runtime_error("TimeBuilder has no valid blocks");
        }
        const auto normalized_size = bounds.size();
        const auto width = static_cast<std::uint64_t>(normalized_size.width);
        const auto height = static_cast<std::uint64_t>(normalized_size.height);
        const auto length = static_cast<std::uint64_t>(normalized_size.length);
        if (width > std::numeric_limits<std::uint64_t>::max() / height ||
            width * height > std::numeric_limits<std::uint64_t>::max() / length) {
            throw std::runtime_error("TimeBuilder volume exceeds uint64");
        }
        mStore.set_size(normalized_size);
        const auto materialized_positions = stream_positions(true);
        if (materialized_positions != valid_positions) {
            throw std::runtime_error("TimeBuilder changed while parsing");
        }
        mNonAirBlocks = mStore.count_non_air();
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
