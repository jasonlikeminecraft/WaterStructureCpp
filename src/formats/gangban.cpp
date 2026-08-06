#include "gangban.hpp"

#include "nbt_text.hpp"

#include <WaterStructure/world.hpp>

#include <io/stream_writer.h>
#include <nbt_tags.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;

struct PendingBlock {
    BlockPos world{};
    std::uint32_t runtime_id = 0;
    std::optional<NbtPayload> nbt;
};

struct AccumulatedBlock {
    std::uint32_t runtime_id = 0;
    std::optional<NbtPayload> nbt;
};

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
        if (!populated) throw std::runtime_error("structure has no blocks");
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (width <= 0 || height <= 0 || length <= 0 ||
            width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("structure bounds exceed int32 dimensions");
        }
        return {
            static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length)
        };
    }
};

std::vector<std::uint8_t> inflate_zlib_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open compressed GangBan file: " + path.string());

    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) throw std::runtime_error("cannot initialize zlib decoder");
    struct Guard {
        z_stream* stream;
        ~Guard() { inflateEnd(stream); }
    } guard{ &stream };

    std::array<std::uint8_t, kStreamChunk> compressed{};
    std::array<std::uint8_t, kStreamChunk> decoded{};
    std::vector<std::uint8_t> output;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0) {
            input.read(reinterpret_cast<char*>(compressed.data()),
                static_cast<std::streamsize>(compressed.size()));
            const auto count = input.gcount();
            if (count <= 0) {
                throw std::runtime_error(
                    "zlib stream truncated at compressed offset " + std::to_string(stream.total_in));
            }
            stream.next_in = compressed.data();
            stream.avail_in = static_cast<uInt>(count);
        }
        stream.next_out = decoded.data();
        stream.avail_out = static_cast<uInt>(decoded.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::runtime_error(
                "zlib decode failed at compressed offset " + std::to_string(stream.total_in));
        }
        const auto produced = decoded.size() - stream.avail_out;
        if (output.size() > kMaxDecodedBytes - produced) {
            throw std::runtime_error("decoded GangBan payload exceeds 2 GiB");
        }
        output.insert(output.end(), decoded.begin(), decoded.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    return output;
}

nlohmann::json read_document(const std::filesystem::path& path, bool compressed)
{
    if (compressed) {
        const auto bytes = inflate_zlib_file(path);
        return nlohmann::json::parse(bytes.begin(), bytes.end());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open GangBan file: " + path.string());
    return nlohmann::json::parse(input);
}

std::int64_t json_integer(const nlohmann::json& value, std::string_view field)
{
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error(std::string(field) + " exceeds int64");
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number) ||
            number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error(std::string(field) + " is outside int64");
        }
        return static_cast<std::int64_t>(number);
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

std::uint16_t json_u16(const nlohmann::json& value, std::string_view field)
{
    const auto number = json_integer(value, field);
    if (number < 0 || number > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds uint16");
    }
    return static_cast<std::uint16_t>(number);
}

std::vector<std::string> string_palette(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_array() || value.empty()) {
        throw std::runtime_error(std::string(field) + " is not a non-empty array");
    }
    std::vector<std::string> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!value[index].is_string()) {
            throw std::runtime_error(std::string(field) + " entry " + std::to_string(index) + " is not a string");
        }
        result.push_back(value[index].get<std::string>());
    }
    return result;
}

BlockPos position3(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_array() || value.size() != 3) {
        throw std::runtime_error(std::string(field) + " must contain three coordinates");
    }
    return {
        json_i32(value[0], std::string(field) + ".x"),
        json_i32(value[1], std::string(field) + ".y"),
        json_i32(value[2], std::string(field) + ".z")
    };
}

BlockPos subtract(BlockPos value, BlockPos origin)
{
    const auto x = static_cast<std::int64_t>(value.x) - origin.x;
    const auto y = static_cast<std::int64_t>(value.y) - origin.y;
    const auto z = static_cast<std::int64_t>(value.z) - origin.z;
    if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
        y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max() ||
        z < std::numeric_limits<std::int32_t>::min() || z > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("local block coordinate exceeds int32");
    }
    return { static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z) };
}

NbtPayload serialize_nbt(const nbt::tag_compound& root)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", root, output, endian::little);
    const auto bytes = output.str();
    return { bytes.begin(), bytes.end() };
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return character >= 'A' && character <= 'Z'
            ? static_cast<char>(character - 'A' + 'a')
            : static_cast<char>(character);
    });
    return value;
}

std::optional<std::int32_t> command_mode(std::string value)
{
    value = lower_ascii(std::move(value));
    if (value == "tick" || value == "impulse" || value == "normal") return 0;
    if (value == "repeating" || value == "repeat") return 1;
    if (value == "chain") return 2;
    return std::nullopt;
}

NbtPayload command_nbt_v1(const nlohmann::json& value)
{
    if (!value.is_object()) throw std::runtime_error("cmds is not an object");
    const auto automatic = value.value("auto", false);
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("CommandBlock");
    root["Command"] = nbt::tag_string(value.value("cmd", std::string{}));
    root["CustomName"] = nbt::tag_string(value.value("name", std::string{}));
    root["LastOutput"] = nbt::tag_string(value.value("last", std::string{}));
    root.emplace<nbt::tag_int>("TickDelay", value.value("tick", 0));
    root.emplace<nbt::tag_byte>("ExecuteOnFirstTick", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("TrackOutput", value.value("should", false) ? 1 : 0);
    root.emplace<nbt::tag_byte>("conditionalMode", value.value("condition", false) ? 1 : 0);
    root.emplace<nbt::tag_byte>("auto", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("Powered", value.value("on", false) ? 1 : 0);
    if (const auto mode = command_mode(value.value("mode", std::string{}))) {
        root.emplace<nbt::tag_int>("LPCommandMode", *mode);
    }
    return serialize_nbt(root);
}

std::string command_block_name(std::int32_t variant)
{
    switch (variant) {
    case 0: return "minecraft:command_block";
    case 1: return "minecraft:repeating_command_block";
    case 2: return "minecraft:chain_command_block";
    default: return "minecraft:command_block";
    }
}

NbtPayload command_nbt_v5(const nlohmann::json& value, std::int32_t variant)
{
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("CommandBlock");
    root["Command"] = nbt::tag_string(
        value.contains("cmd") && value["cmd"].is_string() ? value["cmd"].get<std::string>() : std::string{});
    if (value.contains("name") && value["name"].is_string()) {
        root["CustomName"] = nbt::tag_string(value["name"].get<std::string>());
    }
    if (value.contains("delay")) {
        try {
            root.emplace<nbt::tag_int>("TickDelay", json_i32(value["delay"], "command delay"));
        } catch (...) {
        }
    }
    const auto automatic = value.contains("auto") && value["auto"].is_boolean() && value["auto"].get<bool>();
    const auto conditional = value.contains("condition") &&
        value["condition"].is_boolean() && value["condition"].get<bool>();
    root.emplace<nbt::tag_byte>("ExecuteOnFirstTick", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("TrackOutput", 0);
    root.emplace<nbt::tag_byte>("conditionalMode", conditional ? 1 : 0);
    root.emplace<nbt::tag_byte>("auto", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("Powered", 0);
    root.emplace<nbt::tag_int>("LPCommandMode", variant);
    root["LastOutput"] = nbt::tag_string("");
    return serialize_nbt(root);
}

std::string container_id(std::string_view name)
{
    if (name == "minecraft:blast_furnace" || name == "minecraft:lit_blast_furnace") return "BlastFurnace";
    if (name == "minecraft:furnace" || name == "minecraft:lit_furnace") return "Furnace";
    if (name == "minecraft:smoker" || name == "minecraft:lit_smoker") return "Smoker";
    if (name == "minecraft:chest" || name == "minecraft:trapped_chest") return "Chest";
    if (name == "minecraft:hopper") return "Hopper";
    if (name == "minecraft:dispenser") return "Dispenser";
    if (name == "minecraft:dropper") return "Dropper";
    if (name == "minecraft:barrel") return "Barrel";
    if (name == "minecraft:crafter") return "Crafter";
    if (name.find("shulker_box") != std::string_view::npos) return "ShulkerBox";
    return {};
}

std::optional<NbtPayload> container_nbt(std::string_view block_name, const nlohmann::json& payload)
{
    if (!payload.is_array()) return std::nullopt;
    nbt::tag_list items(nbt::tag_type::Compound);
    for (const auto& raw : payload) {
        if (!raw.is_object()) continue;
        const auto integer_or_zero = [&](std::string_view key) -> std::int64_t {
            const auto found = raw.find(std::string(key));
            if (found == raw.end()) return 0;
            try { return json_integer(*found, key); } catch (...) { return 0; }
        };
        nbt::tag_compound item;
        item["Slot"] = nbt::tag_byte(static_cast<std::int8_t>(integer_or_zero("slot")));
        item["Name"] = nbt::tag_string(
            raw.contains("ns") && raw["ns"].is_string() ? raw["ns"].get<std::string>() : std::string{});
        item["Count"] = nbt::tag_byte(static_cast<std::int8_t>(integer_or_zero("num")));
        item["Damage"] = nbt::tag_short(static_cast<std::int16_t>(integer_or_zero("aux")));
        items.push_back(std::move(item));
    }

    const auto id = container_id(block_name);
    if (id.empty() && items.size() == 0) return std::nullopt;
    nbt::tag_compound root;
    if (!id.empty()) root["id"] = nbt::tag_string(id);
    if (items.size() != 0) root["Items"] = std::move(items);
    return serialize_nbt(root);
}

std::vector<std::string> parse_v3_palette(std::string_view encoded)
{
    std::map<std::int32_t, std::string> indexed;
    std::int32_t maximum = 0;
    std::size_t position = 0;
    while (position < encoded.size()) {
        const auto open = encoded.find('[', position);
        if (open == std::string_view::npos) break;
        const auto close = encoded.find(']', open + 1);
        const auto next_open = close == std::string_view::npos
            ? std::string_view::npos : encoded.find('[', close + 1);
        const auto next_close = next_open == std::string_view::npos
            ? std::string_view::npos : encoded.find(']', next_open + 1);
        if (close == std::string_view::npos || next_open == std::string_view::npos ||
            next_close == std::string_view::npos) break;
        std::int32_t first = 0, second = 0;
        const auto first_text = encoded.substr(open + 1, close - open - 1);
        const auto second_text = encoded.substr(next_open + 1, next_close - next_open - 1);
        const auto first_result = std::from_chars(first_text.data(), first_text.data() + first_text.size(), first);
        const auto second_result = std::from_chars(second_text.data(), second_text.data() + second_text.size(), second);
        if (first_result.ec == std::errc{} && first_result.ptr == first_text.data() + first_text.size() &&
            second_result.ec == std::errc{} && second_result.ptr == second_text.data() + second_text.size() &&
            first == second) {
            indexed[first] = std::string(encoded.substr(close + 1, next_open - close - 1));
            maximum = std::max(maximum, first);
        }
        position = next_close + 1;
    }
    if (indexed.empty()) throw std::runtime_error("V3 palette string has no entries");
    std::vector<std::string> result(static_cast<std::size_t>(maximum) + 1);
    for (std::int32_t index = 0; index <= maximum; ++index) {
        const auto found = indexed.find(index);
        if (found == indexed.end()) throw std::runtime_error("V3 palette is missing index " + std::to_string(index));
        result[static_cast<std::size_t>(index)] = found->second;
    }
    return result;
}

std::optional<NbtPayload> snbt_payload(const nlohmann::json& entry)
{
    if (!entry.is_array() || entry.size() < 7 || !entry[5].is_string() ||
        entry[5].get<std::string>() != "nbt" || !entry[6].is_string()) return std::nullopt;
    auto parsed = parse_mianyang_nbt(entry[6].get<std::string>());
    if (!parsed || parsed.value().empty()) return std::nullopt;
    return std::move(parsed).value();
}

void validate_palette_index(std::int32_t index, std::size_t size, std::size_t block_index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= size) {
        throw std::runtime_error(
            "block index " + std::to_string(block_index) + " palette index " +
            std::to_string(index) + " is out of range");
    }
}

} // namespace

std::string_view GangBanStructure::name() const noexcept
{
    switch (mVersion) {
    case StructureId::GangBanV1: return "GangBanV1";
    case StructureId::GangBanV2: return "GangBanV2";
    case StructureId::GangBanV3: return "GangBanV3";
    case StructureId::GangBanV4: return "GangBanV4";
    case StructureId::GangBanV5: return "GangBanV5";
    case StructureId::GangBanV6: return "GangBanV6";
    case StructureId::GangBanV7: return "GangBanV7";
    default: return "GangBan";
    }
}

std::uint32_t GangBanStructure::runtime_id(std::string_view name, std::uint16_t data)
{
    if (const auto runtime = mRegistry.legacy_runtime_id(name, data)) return *runtime;
    if (const auto unknown = mRegistry.find("minecraft:unknown")) return *unknown;
    return mRegistry.register_state({ "minecraft:unknown", {}, 0 });
}

Result<void> GangBanStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    try {
        const auto compressed = mVersion == StructureId::GangBanV7;
        const auto document = read_document(path, compressed);
        if (!document.is_array()) throw std::runtime_error("root is not an array");

        if (mVersion == StructureId::GangBanV1 || mVersion == StructureId::GangBanV2) {
            const auto has_range = mVersion == StructureId::GangBanV1 ||
                (document.size() >= 2 && document[document.size() - 2].is_object() &&
                    document[document.size() - 2].contains("start") &&
                    document[document.size() - 2].contains("end"));
            if (document.size() < (has_range ? 2u : 1u)) throw std::runtime_error("root array is too short");
            const auto& palette_object = document.back();
            if (!palette_object.is_object() || !palette_object.contains("list")) {
                throw std::runtime_error("palette object is missing list");
            }
            const auto palette = string_palette(palette_object["list"], "palette.list");
            const auto block_count = document.size() - (has_range ? 2 : 1);
            std::vector<PendingBlock> blocks;
            blocks.reserve(block_count);
            Bounds inferred;

            BlockPos origin{};
            Size declared_size{};
            if (has_range) {
                const auto& range = document[document.size() - 2];
                if (!range.is_object() || !range.contains("start") || !range.contains("end")) {
                    throw std::runtime_error("range object is missing start/end");
                }
                const auto start = position3(range["start"], "range.start");
                const auto end = position3(range["end"], "range.end");
                Bounds declared;
                declared.add(start);
                declared.add(end);
                origin = declared.minimum;
                declared_size = declared.size();
            }

            for (std::size_t index = 0; index < block_count; ++index) {
                try {
                    const auto& entry = document[index];
                    if (!entry.is_object() || !entry.contains("id") || !entry.contains("p")) {
                        throw std::runtime_error("entry is missing id/p");
                    }
                    const auto palette_index = json_i32(entry["id"], "id");
                    validate_palette_index(palette_index, palette.size(), index);
                    const auto aux = entry.contains("aux") ? json_u16(entry["aux"], "aux") : 0;
                    const auto world = position3(entry["p"], "p");
                    inferred.add(world);
                    PendingBlock block{ world, runtime_id(palette[palette_index], aux), std::nullopt };
                    if (entry.contains("cmds") && !entry["cmds"].is_null()) {
                        block.nbt = command_nbt_v1(entry["cmds"]);
                    }
                    blocks.push_back(std::move(block));
                } catch (const std::exception& error) {
                    throw std::runtime_error("block index " + std::to_string(index) + ": " + error.what());
                }
            }
            if (!has_range) {
                origin = inferred.minimum;
                declared_size = inferred.size();
            }
            mStore.set_size(declared_size);
            for (std::size_t index = 0; index < blocks.size(); ++index) {
                const auto local = subtract(blocks[index].world, origin);
                if (has_range && (local.x < 0 || local.y < 0 || local.z < 0 ||
                    local.x >= declared_size.width || local.y >= declared_size.height ||
                    local.z >= declared_size.length)) {
                    throw std::runtime_error("block index " + std::to_string(index) + " is outside declared range");
                }
                mStore.put(local, blocks[index].runtime_id);
                if (blocks[index].nbt) mStore.put_entity(local, std::move(*blocks[index].nbt));
                if (blocks[index].runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
            }
            return Result<void>::success();
        }

        if (mVersion == StructureId::GangBanV3 || mVersion == StructureId::GangBanV4) {
            if (document.size() < 3 || !document[0].is_object()) {
                throw std::runtime_error("header/chunk array is incomplete");
            }
            std::vector<std::string> palette;
            BlockPos declared_origin{};
            Size declared_size{};
            if (mVersion == StructureId::GangBanV3) {
                if (!document[1].is_string()) throw std::runtime_error("V3 palette is not a string");
                palette = parse_v3_palette(document[1].get<std::string>());
                declared_origin = {
                    json_i32(document[0].at("x"), "header.x"),
                    json_i32(document[0].at("y"), "header.y"),
                    json_i32(document[0].at("z"), "header.z")
                };
                declared_size = {
                    json_i32(document[0].at("xcha"), "header.xcha"),
                    json_i32(document[0].at("ycha"), "header.ycha"),
                    json_i32(document[0].at("zcha"), "header.zcha")
                };
                if (declared_size.width <= 0 || declared_size.height <= 0 || declared_size.length <= 0) {
                    throw std::runtime_error("V3 dimensions must be positive");
                }
            } else {
                palette = string_palette(document[1], "V4 palette");
            }

            std::map<BlockPos, AccumulatedBlock, std::less<>> accumulated;
            Bounds bounds;
            std::size_t block_index = 0;
            for (std::size_t chunk_index = 2; chunk_index < document.size(); ++chunk_index) {
                const auto& chunk = document[chunk_index];
                if (!chunk.is_object() || !chunk.contains("grids") || !chunk.contains("data") ||
                    !chunk["grids"].is_object() || !chunk["data"].is_array()) {
                    throw std::runtime_error("chunk index " + std::to_string(chunk_index - 2) + " is invalid");
                }
                const auto chunk_x = json_i32(chunk["grids"].at("x"), "grids.x");
                const auto chunk_z = json_i32(chunk["grids"].at("z"), "grids.z");
                for (const auto& entry : chunk["data"]) {
                    try {
                        if (!entry.is_array() || entry.size() < 5) throw std::runtime_error("entry has fewer than five fields");
                        const auto palette_index = json_i32(entry[0], "palette index");
                        validate_palette_index(palette_index, palette.size(), block_index);
                        const auto aux = json_u16(entry[1], "aux");
                        const auto relative_x = json_i32(entry[2], "local x");
                        const auto world_y = json_i32(entry[3], "y");
                        const auto relative_z = json_i32(entry[4], "local z");
                        const auto x64 = static_cast<std::int64_t>(chunk_x) + relative_x;
                        const auto z64 = static_cast<std::int64_t>(chunk_z) + relative_z;
                        if (x64 < std::numeric_limits<std::int32_t>::min() || x64 > std::numeric_limits<std::int32_t>::max() ||
                            z64 < std::numeric_limits<std::int32_t>::min() || z64 > std::numeric_limits<std::int32_t>::max()) {
                            throw std::runtime_error("world coordinate exceeds int32");
                        }
                        const BlockPos world{ static_cast<std::int32_t>(x64), world_y, static_cast<std::int32_t>(z64) };
                        const auto current_nbt = snbt_payload(entry);
                        const auto runtime = runtime_id(palette[palette_index], aux);
                        if (mVersion == StructureId::GangBanV3) {
                            const auto local = subtract(world, declared_origin);
                            auto [found, inserted] = accumulated.try_emplace(local, AccumulatedBlock{ runtime, current_nbt });
                            if (inserted) {
                                if (runtime != mRegistry.air_runtime_id()) ++mNonAirBlocks;
                            } else {
                                found->second.runtime_id = runtime;
                                if (!found->second.nbt && current_nbt) found->second.nbt = current_nbt;
                            }
                        } else {
                            bounds.add(world);
                            auto [found, inserted] = accumulated.try_emplace(world, AccumulatedBlock{ runtime, current_nbt });
                            if (!inserted) {
                                found->second.runtime_id = runtime;
                                if (current_nbt) found->second.nbt = current_nbt;
                            }
                        }
                    } catch (const std::exception& error) {
                        throw std::runtime_error("block index " + std::to_string(block_index) + ": " + error.what());
                    }
                    ++block_index;
                }
            }
            if (accumulated.empty()) throw std::runtime_error("structure has no blocks");
            if (mVersion == StructureId::GangBanV3) {
                mStore.set_size(declared_size);
                for (auto& [local, block] : accumulated) {
                    mStore.put(local, block.runtime_id);
                    if (block.nbt) mStore.put_entity(local, std::move(*block.nbt));
                }
            } else {
                mStore.set_size(bounds.size());
                for (auto& [world, block] : accumulated) {
                    const auto local = subtract(world, bounds.minimum);
                    mStore.put(local, block.runtime_id);
                    if (block.nbt) mStore.put_entity(local, std::move(*block.nbt));
                    if (block.runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
                }
            }
            return Result<void>::success();
        }

        if (mVersion == StructureId::GangBanV5 || mVersion == StructureId::GangBanV6 ||
            mVersion == StructureId::GangBanV7) {
            if (document.empty()) throw std::runtime_error("root array is empty");
            const auto palette = string_palette(document.back(), "palette");
            const auto v5 = mVersion == StructureId::GangBanV5;
            const auto stream_end = document.size() - (v5 ? 2 : 1);
            if (v5) {
                if (document.size() < 2 || !document[document.size() - 2].is_object() ||
                    !document[document.size() - 2].contains("ep") ||
                    !document[document.size() - 2]["ep"].is_array() ||
                    document[document.size() - 2]["ep"].size() != 3) {
                    throw std::runtime_error("V5 area ep is invalid");
                }
                for (std::size_t index = 0; index < 3; ++index) {
                    (void)json_integer(document[document.size() - 2]["ep"][index], "area.ep");
                }
            }

            std::map<BlockPos, AccumulatedBlock, std::less<>> accumulated;
            Bounds bounds;
            auto place = [&](BlockPos world, std::int32_t primary, std::int32_t secondary,
                             const nlohmann::json* payload, std::size_t block_index) {
                std::uint32_t runtime = 0;
                std::optional<NbtPayload> nbt;
                if (payload != nullptr && payload->is_object()) {
                    runtime = runtime_id(command_block_name(secondary), static_cast<std::uint16_t>(primary));
                    nbt = command_nbt_v5(*payload, secondary);
                } else {
                    validate_palette_index(primary, palette.size(), block_index);
                    runtime = runtime_id(palette[primary], static_cast<std::uint16_t>(secondary));
                    if (payload != nullptr && payload->is_array()) nbt = container_nbt(palette[primary], *payload);
                }
                bounds.add(world);
                auto [found, inserted] = accumulated.try_emplace(world, AccumulatedBlock{ runtime, nbt });
                if (!inserted) {
                    found->second.runtime_id = runtime;
                    if (!found->second.nbt && nbt) found->second.nbt = std::move(nbt);
                }
            };

            if (v5) {
                std::size_t index = 0;
                std::size_t block_index = 0;
                while (index < stream_end) {
                    if (stream_end - index < 6) {
                        throw std::runtime_error("block index " + std::to_string(block_index) +
                            " is truncated at stream index " + std::to_string(index));
                    }
                    try {
                        std::array<std::int32_t, 6> base{};
                        for (std::size_t field = 0; field < base.size(); ++field) {
                            base[field] = json_i32(document[index + field], "V5 block field");
                        }
                        index += base.size();
                        const nlohmann::json* payload = nullptr;
                        if (index < stream_end && (document[index].is_array() || document[index].is_object())) {
                            payload = &document[index++];
                        }
                        place({ base[1], base[2], base[3] }, base[4], base[5], payload, block_index);
                    } catch (const std::exception& error) {
                        throw std::runtime_error("block index " + std::to_string(block_index) + ": " + error.what());
                    }
                    ++block_index;
                }
            } else {
                std::int64_t x = 0, y = 0, z = 0;
                std::size_t block_index = 0;
                for (std::size_t index = 0; index < stream_end; ++index) {
                    const auto& entry = document[index];
                    try {
                        if (!entry.is_array()) throw std::runtime_error("stream entry is not an array");
                        if (entry.size() >= 5 && entry[3].is_string() && entry[4].is_string()) continue;
                        if (entry.size() < 5) throw std::runtime_error("stream entry has fewer than five fields");
                        x += json_i32(entry[0], "dx");
                        y += json_i32(entry[1], "dy");
                        z += json_i32(entry[2], "dz");
                        if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
                            y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max() ||
                            z < std::numeric_limits<std::int32_t>::min() || z > std::numeric_limits<std::int32_t>::max()) {
                            throw std::runtime_error("delta cursor exceeds int32");
                        }
                        const auto primary = json_i32(entry[3], "primary");
                        const auto secondary = json_i32(entry[4], "secondary");
                        const auto* payload = entry.size() >= 6 ? &entry[5] : nullptr;
                        place({ static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z) },
                            primary, secondary, payload, block_index);
                    } catch (const std::exception& error) {
                        throw std::runtime_error("block index " + std::to_string(block_index) +
                            " stream index " + std::to_string(index) + ": " + error.what());
                    }
                    ++block_index;
                }
            }
            if (accumulated.empty()) throw std::runtime_error("structure has no blocks");
            mStore.set_size(bounds.size());
            for (auto& [world, block] : accumulated) {
                const auto local = subtract(world, bounds.minimum);
                mStore.put(local, block.runtime_id);
                if (block.nbt) mStore.put_entity(local, std::move(*block.nbt));
                if (block.runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
            }
            return Result<void>::success();
        }

        return Result<void>::failure("GangBan reader received an invalid version");
    } catch (const std::exception& error) {
        return Result<void>::failure("parse " + std::string(name()) + " failed: " + error.what());
    }
}

Result<void> GangBanStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> GangBanStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(std::string(name()) + " has no Go FromMCWorld capability");
}

} // namespace water_structure
