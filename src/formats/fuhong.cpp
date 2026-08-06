#include "fuhong.hpp"

#include "nbt_text.hpp"

#include <WaterStructure/world.hpp>

#include <io/stream_reader.h>
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

constexpr std::string_view kFuHongV5Key = "FuHongBuild";
constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;

struct PendingBlock {
    BlockPos world{};
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
            populated = true;
            minimum = maximum = pos;
            return;
        }
        minimum.x = std::min(minimum.x, pos.x);
        minimum.y = std::min(minimum.y, pos.y);
        minimum.z = std::min(minimum.z, pos.z);
        maximum.x = std::max(maximum.x, pos.x);
        maximum.y = std::max(maximum.y, pos.y);
        maximum.z = std::max(maximum.z, pos.z);
    }

    Size size(std::string_view format) const
    {
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (!populated || width <= 0 || height <= 0 || length <= 0 ||
            width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error(std::string(format) + " bounds are invalid");
        }
        return { static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) };
    }
};

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n\"");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n\"");
    return std::string(value.substr(first, last - first + 1));
}

std::string json_string(const nlohmann::json& value)
{
    if (value.is_null()) return "<nil>";
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
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

std::int32_t i32(const nlohmann::json& value, std::string_view field)
{
    const auto number = integer(value, field);
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(number);
}

std::int32_t checked_add(std::int32_t left, std::int32_t right, std::string_view field)
{
    const auto result = static_cast<std::int64_t>(left) + right;
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(result);
}

std::vector<std::int32_t> coordinates(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_array()) throw std::runtime_error(std::string(field) + " is not an array");
    std::vector<std::int32_t> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        result.push_back(i32(value[index], std::string(field) + "[" + std::to_string(index) + "]"));
    }
    return result;
}

std::int32_t first_coordinate(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_array()) return i32(value, field);
    if (value.empty()) throw std::runtime_error(std::string(field) + " coordinate array is empty");
    return i32(value.front(), field);
}

BlockPos local_position(BlockPos world, BlockPos origin)
{
    return { world.x - origin.x, world.y - origin.y, world.z - origin.z };
}

NbtPayload serialize_nbt(const nbt::tag_compound& root)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", root, output, endian::little);
    const auto bytes = output.str();
    return { bytes.begin(), bytes.end() };
}

std::unique_ptr<nbt::tag_compound> decode_compound(const NbtPayload& payload)
{
    const std::string bytes(payload.begin(), payload.end());
    std::istringstream input(bytes, std::ios::binary);
    auto [_, root] = nbt::io::read_tag(input, endian::little);
    if (!root || root->get_type() != nbt::tag_type::Compound) return {};
    return std::unique_ptr<nbt::tag_compound>(
        static_cast<nbt::tag_compound*>(root.release()));
}

std::int8_t auto_byte(const nlohmann::json& value)
{
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_string()) {
        auto text = trim(value.get<std::string>());
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (text == "true") return 1;
        if (text == "false" || text.empty()) return 0;
    }
    try { return integer(value, "auto") != 0 ? 1 : 0; } catch (...) { return 0; }
}

std::unique_ptr<nbt::tag_compound> command_nbt(const nlohmann::json& values)
{
    if (!values.is_array() || values.empty()) return {};
    const auto command = json_string(values[0]);
    std::int32_t delay = 0;
    if (values.size() > 1) {
        try { delay = static_cast<std::int32_t>(integer(values[1], "TickDelay")); } catch (...) {}
    }
    const auto automatic = values.size() > 2 ? auto_byte(values[2]) : 0;
    const auto custom_name = values.size() > 3 ? json_string(values[3]) : std::string{};
    auto root = std::make_unique<nbt::tag_compound>();
    (*root)["id"] = nbt::tag_string("CommandBlock");
    (*root)["Command"] = nbt::tag_string(command);
    (*root)["CustomName"] = nbt::tag_string(custom_name);
    root->emplace<nbt::tag_byte>("ExecuteOnFirstTick", 1);
    root->emplace<nbt::tag_byte>("auto", automatic);
    root->emplace<nbt::tag_int>("TickDelay", delay);
    root->emplace<nbt::tag_byte>("conditionalMode", 0);
    root->emplace<nbt::tag_byte>("TrackOutput", 1);
    root->emplace<nbt::tag_int>("Version", command.find("execute") != std::string::npos ? 38 : 19);
    return root;
}

std::string container_id(std::string_view raw_name)
{
    const auto name = trim(raw_name);
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

std::int64_t integer_or_zero(const nlohmann::json& object,
    std::initializer_list<std::string_view> names)
{
    for (const auto name : names) {
        const auto found = object.find(std::string(name));
        if (found == object.end() || found->is_null()) continue;
        try { return integer(*found, name); } catch (...) { return 0; }
    }
    return 0;
}

std::string string_field(const nlohmann::json& object,
    std::initializer_list<std::string_view> names)
{
    for (const auto name : names) {
        const auto found = object.find(std::string(name));
        if (found == object.end() || found->is_null()) continue;
        return json_string(*found);
    }
    return {};
}

std::string item_name(std::string name)
{
    name = trim(name);
    if (!name.empty() && name.find(':') == std::string::npos) name.insert(0, "minecraft:");
    return name;
}

nbt::tag_compound item_block_nbt(const std::string& name)
{
    nbt::tag_compound block;
    block["name"] = nbt::tag_string(name);
    block["states"] = nbt::tag_compound{};
    block.emplace<nbt::tag_short>("val", 0);
    block.emplace<nbt::tag_int>("version", 17959425);
    return block;
}

std::unique_ptr<nbt::tag_compound> container_nbt_v3(
    std::string_view block_name, const nlohmann::json& payload)
{
    const auto id = container_id(block_name);
    if (id.empty()) return {};
    nbt::tag_list items(nbt::tag_type::Compound);
    if (payload.is_array()) {
        for (const auto& raw : payload) {
            std::string name;
            std::int64_t damage = 0, count = 0, slot = 0;
            if (raw.is_array() && raw.size() >= 4) {
                name = item_name(json_string(raw[0]));
                try { damage = integer(raw[1], "Damage"); } catch (...) {}
                try { count = integer(raw[2], "Count"); } catch (...) {}
                try { slot = integer(raw[3], "Slot"); } catch (...) {}
            } else if (raw.is_object()) {
                name = item_name(string_field(raw, { "Name", "name", "ns" }));
                damage = integer_or_zero(raw, { "Damage", "damage", "aux" });
                count = integer_or_zero(raw, { "Count", "count", "num" });
                slot = integer_or_zero(raw, { "Slot", "slot" });
            } else {
                continue;
            }
            if (name.empty()) continue;
            nbt::tag_compound item;
            item["Name"] = nbt::tag_string(name);
            item.emplace<nbt::tag_byte>("Count", static_cast<std::int8_t>(count));
            item.emplace<nbt::tag_short>("Damage", static_cast<std::int16_t>(damage));
            item.emplace<nbt::tag_byte>("Slot", static_cast<std::int8_t>(slot));
            item["Block"] = item_block_nbt(name);
            items.push_back(std::move(item));
        }
    }
    auto root = std::make_unique<nbt::tag_compound>();
    (*root)["id"] = nbt::tag_string(id);
    root->emplace<nbt::tag_byte>("Findable", 0);
    root->emplace<nbt::tag_byte>("IsOpened", 0);
    root->emplace<nbt::tag_byte>("isMovable", 1);
    (*root)["Items"] = std::move(items);
    return root;
}

std::unique_ptr<nbt::tag_compound> container_nbt_v2(
    std::string_view block_name, const nlohmann::json& raw)
{
    if (!raw.is_array() || raw.empty()) return {};
    nbt::tag_list items(nbt::tag_type::Compound);
    for (const auto& value : raw) {
        if (!value.is_object()) continue;
        auto name = item_name(string_field(value, { "name", "Name" }));
        nbt::tag_compound item;
        item["Name"] = nbt::tag_string(name);
        item.emplace<nbt::tag_short>("Damage", static_cast<std::int16_t>(
            integer_or_zero(value, { "damage", "Damage" })));
        item.emplace<nbt::tag_byte>("Count", static_cast<std::int8_t>(
            integer_or_zero(value, { "count", "Count" })));
        item.emplace<nbt::tag_byte>("Slot", static_cast<std::int8_t>(
            integer_or_zero(value, { "slot", "Slot" })));
        items.push_back(std::move(item));
    }
    if (items.size() == 0) return {};
    auto root = std::make_unique<nbt::tag_compound>();
    if (const auto id = container_id(block_name); !id.empty()) (*root)["id"] = nbt::tag_string(id);
    (*root)["Items"] = std::move(items);
    return root;
}

std::unique_ptr<nbt::tag_compound> sign_nbt(
    std::string_view block_name, const nlohmann::json& payload)
{
    std::string text;
    if (payload.is_array()) {
        for (std::size_t index = 0; index < payload.size(); ++index) {
            if (index != 0) text.push_back('\n');
            text += json_string(payload[index]);
        }
    } else {
        text = json_string(payload);
    }
    const auto name = trim(block_name);
    const auto hanging = name.size() >= 12 && name.ends_with("hanging_sign");
    auto text_compound = [](std::string value) {
        nbt::tag_compound result;
        result["FilteredText"] = nbt::tag_string("");
        result.emplace<nbt::tag_byte>("HideGlowOutline", 0);
        result.emplace<nbt::tag_byte>("IgnoreLighting", 0);
        result.emplace<nbt::tag_byte>("PersistFormatting", 1);
        result.emplace<nbt::tag_int>("SignTextColor", -16777216);
        result["Text"] = nbt::tag_string(std::move(value));
        result["TextOwner"] = nbt::tag_string("");
        return result;
    };
    auto root = std::make_unique<nbt::tag_compound>();
    (*root)["id"] = nbt::tag_string(hanging ? "HangingSign" : "Sign");
    root->emplace<nbt::tag_byte>("IsWaxed", 0);
    root->emplace<nbt::tag_byte>("isMovable", 1);
    (*root)["BackText"] = text_compound("");
    (*root)["FrontText"] = text_compound(std::move(text));
    return root;
}

std::unique_ptr<nbt::tag_compound> extra_nbt(
    std::string_view block_name, const nlohmann::json& payload)
{
    auto lower = std::string(block_name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("command_block") != std::string::npos) return command_nbt(payload);
    if (!container_id(block_name).empty()) return container_nbt_v3(block_name, payload);
    if (lower.find("sign") != std::string::npos) return sign_nbt(block_name, payload);
    return {};
}

std::vector<std::uint8_t> inflate_zlib(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open FuHongV5 file: " + path.string());
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) throw std::runtime_error("initialize FuHongV5 zlib failed");
    struct Guard { z_stream* stream; ~Guard() { inflateEnd(stream); } } guard{ &stream };
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
                throw std::runtime_error("FuHongV5 zlib stream truncated at compressed offset " +
                    std::to_string(stream.total_in));
            }
            stream.next_in = compressed.data();
            stream.avail_in = static_cast<uInt>(count);
        }
        stream.next_out = decoded.data();
        stream.avail_out = static_cast<uInt>(decoded.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::runtime_error("FuHongV5 zlib decode failed at compressed offset " +
                std::to_string(stream.total_in));
        }
        const auto produced = decoded.size() - stream.avail_out;
        if (output.size() > kMaxDecodedBytes - produced) {
            throw std::runtime_error("FuHongV5 decoded payload exceeds 2 GiB");
        }
        output.insert(output.end(), decoded.begin(),
            decoded.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    return output;
}

std::pair<std::uint32_t, std::size_t> decode_utf8(
    std::span<const std::uint8_t> data, std::size_t offset)
{
    const auto first = data[offset];
    std::size_t length = 0;
    std::uint32_t value = 0;
    if (first < 0x80) { length = 1; value = first; }
    else if ((first & 0xe0) == 0xc0) { length = 2; value = first & 0x1f; }
    else if ((first & 0xf0) == 0xe0) { length = 3; value = first & 0x0f; }
    else if ((first & 0xf8) == 0xf0) { length = 4; value = first & 0x07; }
    else throw std::runtime_error("FuHongV5 invalid UTF-8 at decoded offset " + std::to_string(offset));
    if (length > data.size() - offset) {
        throw std::runtime_error("FuHongV5 UTF-8 truncated at decoded offset " + std::to_string(offset));
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto next = data[offset + index];
        if ((next & 0xc0) != 0x80) {
            throw std::runtime_error("FuHongV5 invalid UTF-8 at decoded offset " + std::to_string(offset));
        }
        value = (value << 6) | (next & 0x3f);
    }
    const auto minimum = std::array<std::uint32_t, 5>{ 0, 0, 0x80, 0x800, 0x10000 };
    if (value < minimum[length] || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        throw std::runtime_error("FuHongV5 invalid UTF-8 rune at decoded offset " + std::to_string(offset));
    }
    return { value, length };
}

void append_utf8(std::string& output, std::uint32_t value)
{
    if (value <= 0x7f) output.push_back(static_cast<char>(value));
    else if (value <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

std::string decode_v5(std::span<const std::uint8_t> encrypted)
{
    if (encrypted.empty()) throw std::runtime_error("FuHongV5 payload is empty");
    std::string result;
    result.reserve(encrypted.size());
    std::size_t offset = 0;
    std::size_t rune_index = 0;
    while (offset < encrypted.size()) {
        auto [value, length] = decode_utf8(encrypted, offset);
        const auto shifted = static_cast<std::int64_t>(value) - static_cast<std::int64_t>(rune_index % 3);
        const auto plain = shifted ^ static_cast<unsigned char>(kFuHongV5Key[rune_index % kFuHongV5Key.size()]);
        if (plain < 0 || plain > 0x10ffff || (plain >= 0xd800 && plain <= 0xdfff)) {
            throw std::runtime_error("FuHongV5 invalid decrypted rune at rune index " +
                std::to_string(rune_index));
        }
        append_utf8(result, static_cast<std::uint32_t>(plain));
        offset += length;
        ++rune_index;
    }
    return result;
}

} // namespace

std::string_view FuHongStructure::name() const noexcept
{
    switch (mVersion) {
    case StructureId::FuHongV1: return "FuHongV1";
    case StructureId::FuHongV2: return "FuHongV2";
    case StructureId::FuHongV3: return "FuHongV3";
    case StructureId::FuHongV4: return "FuHongV4";
    case StructureId::FuHongV5: return "FuHongV5";
    default: return "FuHong";
    }
}

std::uint32_t FuHongStructure::runtime_id(
    std::string name, std::int64_t aux, const std::map<std::string, std::string>& states)
{
    name = trim(name);
    auto effective_states = states;
    if (const auto bracket = name.find('['); bracket != std::string::npos && name.ends_with(']')) {
        const auto encoded_states = name.substr(bracket + 1, name.size() - bracket - 2);
        name = trim(name.substr(0, bracket));
        std::size_t begin = 0;
        while (begin <= encoded_states.size()) {
            const auto end = encoded_states.find(',', begin);
            const auto pair = trim(encoded_states.substr(begin,
                end == std::string::npos ? std::string::npos : end - begin));
            if (const auto equals = pair.find('='); equals != std::string::npos) {
                effective_states.try_emplace(
                    trim(pair.substr(0, equals)), trim(pair.substr(equals + 1)));
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    std::ostringstream key;
    key << name << '|';
    if (effective_states.empty()) key << aux;
    else for (const auto& [property, value] : effective_states) key << property << '=' << value << ',';
    if (const auto cached = mPaletteCache.find(key.str()); cached != mPaletteCache.end()) {
        return cached->second;
    }

    std::optional<std::uint32_t> runtime;
    if (!effective_states.empty()) {
        // FuHong V2 stores a partial Bedrock state map. The Go block resolver
        // fills omitted properties with the palette default before lookup.
        if (const auto base = mRegistry.find(name); base) {
            if (auto state = mRegistry.state(*base)) {
                auto merged = state->states;
                for (auto& property : merged) {
                    if (const auto provided = effective_states.find(property.name);
                        provided != effective_states.end()) {
                        property.value = provided->second;
                        if (property.value == "true") property.value = "1";
                        else if (property.value == "false") property.value = "0";
                    }
                }
                for (const auto& [property_name, property_value] : effective_states) {
                    if (std::ranges::none_of(merged, [&](const auto& item) {
                        return item.name == property_name;
                    })) {
                        BlockStateProperty property;
                        property.name = property_name;
                        property.value = property_value;
                        if (property.value == "true" || property.value == "false") {
                            property.type = BlockStateValueType::Byte;
                            property.value = property.value == "true" ? "1" : "0";
                        } else {
                            const auto is_number = !property.value.empty() &&
                                std::ranges::all_of(property.value, [](unsigned char c) {
                                    return std::isdigit(c) || c == '-';
                                });
                            property.type = is_number ? BlockStateValueType::Int : BlockStateValueType::String;
                        }
                        merged.push_back(std::move(property));
                    }
                }
                runtime = mRegistry.find(name, merged);
            }
        }
        std::ostringstream java;
        if (!runtime) {
            java << name << '[';
            std::size_t index = 0;
            for (const auto& [property, value] : effective_states) {
                if (index++ != 0) java << ',';
                java << property << '=' << value;
            }
            java << ']';
            runtime = mRegistry.java_runtime_id(java.str());
        }
    }
    if (!runtime) runtime = mRegistry.legacy_runtime_id(name, static_cast<std::uint16_t>(aux));
    if (!runtime) runtime = mRegistry.java_runtime_id(name);
    if (!runtime) {
        if (const auto unknown = mRegistry.find("minecraft:unknown")) runtime = *unknown;
        else runtime = mRegistry.register_state({ "minecraft:unknown", {}, 0 });
    }
    mPaletteCache.emplace(key.str(), *runtime);
    return *runtime;
}

Result<void> FuHongStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mPaletteCache.clear();
    mNonAirBlocks = 0;
    try {
        nlohmann::json root;
        if (mVersion == StructureId::FuHongV5) {
            const auto encrypted = inflate_zlib(path);
            root = nlohmann::json::parse(decode_v5(encrypted));
        } else {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("cannot open " + std::string(name()) + " file: " + path.string());
            root = nlohmann::json::parse(input);
        }

        std::map<BlockPos, PendingBlock, std::less<>> blocks;
        Bounds bounds;

        if (mVersion == StructureId::FuHongV1) {
            if (!root.is_array() || root.empty()) throw std::runtime_error("root is not a non-empty array");
            for (std::size_t index = 0; index < root.size(); ++index) {
                try {
                    const auto& raw = root[index];
                    if (!raw.is_object()) throw std::runtime_error("block is not an object");
                    const auto block_name = trim(raw.value("name", std::string{}));
                    if (block_name.empty()) throw std::runtime_error("block name is empty");
                    if (!raw.contains("x") || !raw.contains("y") || !raw.contains("z")) {
                        throw std::runtime_error("block coordinates are missing");
                    }
                    const auto aux = raw.contains("aux") && !raw["aux"].is_null()
                        ? integer(raw["aux"], "aux") : 0;
                    const BlockPos world{ first_coordinate(raw["x"], "x"),
                        first_coordinate(raw["y"], "y"), first_coordinate(raw["z"], "z") };
                    blocks[world] = { world, runtime_id(block_name, aux), std::nullopt };
                    bounds.add(world);
                } catch (const std::exception& error) {
                    throw std::runtime_error("block index " + std::to_string(index) + ": " + error.what());
                }
            }
        } else if (mVersion == StructureId::FuHongV2) {
            if (!root.is_object() || !root.contains("FuHongBuild_FinalFormat") ||
                !root["FuHongBuild_FinalFormat"].is_array() || root["FuHongBuild_FinalFormat"].empty()) {
                throw std::runtime_error("FuHongBuild_FinalFormat is not a non-empty array");
            }
            const auto& chunks = root["FuHongBuild_FinalFormat"];
            for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
                try {
                    const auto entries = chunks[chunk_index].find("block");
                    if (entries == chunks[chunk_index].end() || !entries->is_array()) {
                        throw std::runtime_error("block list is missing");
                    }
                    for (std::size_t entry_index = 0; entry_index < entries->size(); ++entry_index) {
                        const auto& entry = (*entries)[entry_index];
                        if (!entry.is_object() || !entry.contains("n")) continue;
                        try {
                            const auto block_name = trim(json_string(entry["n"]));
                            if (block_name.empty()) throw std::runtime_error("block name is empty");
                            const auto xs = coordinates(entry.at("x"), "x");
                            const auto ys = coordinates(entry.at("y"), "y");
                            const auto zs = coordinates(entry.at("z"), "z");
                            if (xs.size() != ys.size() || xs.size() != zs.size()) {
                                throw std::runtime_error("coordinate array lengths differ");
                            }
                            std::vector<std::int64_t> aux_values(xs.size(), 0);
                            if (const auto aux = entry.find("a"); aux != entry.end() && !aux->is_null()) {
                                if (aux->is_array()) {
                                    std::int64_t last = 0;
                                    for (std::size_t i = 0; i < xs.size(); ++i) {
                                        if (i < aux->size()) {
                                            try { last = integer((*aux)[i], "a"); } catch (...) { last = 0; }
                                        }
                                        aux_values[i] = last;
                                    }
                                } else {
                                    try { std::ranges::fill(aux_values, integer(*aux, "a")); } catch (...) {}
                                }
                            }
                            std::map<std::string, std::string> states;
                            if (const auto raw_states = entry.find("state"); raw_states != entry.end() && raw_states->is_array()) {
                                for (const auto& raw_state : *raw_states) {
                                    if (!raw_state.is_string()) continue;
                                    const auto pair = raw_state.get<std::string>();
                                    if (const auto equals = pair.find('='); equals != std::string::npos) {
                                        states[trim(pair.substr(0, equals))] = trim(pair.substr(equals + 1));
                                    }
                                }
                            }
                            std::vector<std::unique_ptr<nbt::tag_compound>> command_values(xs.size());
                            if (const auto command = entry.find("c"); command != entry.end() && command->is_object()) {
                                const auto select = [&](std::string_view field, std::size_t index) -> nlohmann::json {
                                    const auto found = command->find(std::string(field));
                                    if (found == command->end() || !found->is_array() || found->empty()) return nullptr;
                                    return (*found)[std::min(index, found->size() - 1)];
                                };
                                for (std::size_t i = 0; i < xs.size(); ++i) {
                                    command_values[i] = command_nbt(nlohmann::json::array({
                                        select("c", i), select("t", i), select("a", i), select("n", i) }));
                                }
                            }
                            std::vector<std::unique_ptr<nbt::tag_compound>> container_values(xs.size());
                            if (const auto data = entry.find("d"); data != entry.end() && data->is_array()) {
                                for (std::size_t i = 0; i < xs.size() && i < data->size(); ++i) {
                                    const auto& value = (*data)[i];
                                    if (!value.is_object()) continue;
                                    if (const auto encoded = value.find("e"); encoded != value.end() && encoded->is_string()) {
                                        const auto parsed = parse_mianyang_nbt(encoded->get<std::string>());
                                        if (parsed && !parsed.value().empty()) container_values[i] = decode_compound(parsed.value());
                                    } else if (const auto items = value.find("d"); items != value.end()) {
                                        container_values[i] = container_nbt_v2(block_name, *items);
                                    }
                                }
                            }
                            for (std::size_t i = 0; i < xs.size(); ++i) {
                                const BlockPos world{ xs[i], ys[i], zs[i] };
                                auto& pending = blocks[world];
                                pending.world = world;
                                pending.runtime_id = runtime_id(block_name, aux_values[i], states);
                                std::unique_ptr<nbt::tag_compound> entity;
                                if (command_values[i]) entity = std::move(command_values[i]);
                                if (container_values[i]) {
                                    if (!entity) entity = std::move(container_values[i]);
                                    else {
                                        for (const auto& [key, tag] : *container_values[i]) {
                                            (*entity)[key] = nbt::value(tag);
                                        }
                                    }
                                }
                                if (entity) {
                                    if (const auto conditional = states.find("conditional_bit");
                                        conditional != states.end()) {
                                        auto value = conditional->second;
                                        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                                            return static_cast<char>(std::tolower(c));
                                        });
                                        entity->emplace<nbt::tag_byte>("conditionalMode", value == "true" ? 1 : 0);
                                    }
                                    if (entity->has_key("CustomName", nbt::tag_type::String) &&
                                        entity->at("CustomName").as<nbt::tag_string>().get() == "") {
                                        entity->erase("CustomName");
                                    }
                                    pending.nbt = serialize_nbt(*entity);
                                }
                                bounds.add(world);
                            }
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " + std::to_string(entry_index) + ": " + error.what());
                        }
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error("chunk index " + std::to_string(chunk_index) + ": " + error.what());
                }
            }
        } else {
            if (!root.is_object() || !root.contains("BlocksList") || !root["BlocksList"].is_array() ||
                root["BlocksList"].empty()) throw std::runtime_error("BlocksList is not a non-empty array");
            if (!root.contains("FuHongBuild") || !root["FuHongBuild"].is_array()) {
                throw std::runtime_error("FuHongBuild is not an array");
            }
            const auto palette = root["BlocksList"].get<std::vector<std::string>>();
            const auto& chunks = root["FuHongBuild"];
            for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
                try {
                    const auto& chunk = chunks[chunk_index];
                    const auto start_x = mVersion == StructureId::FuHongV3 ? i32(chunk.at("startX"), "startX") : 0;
                    const auto start_z = mVersion == StructureId::FuHongV3 ? i32(chunk.at("startZ"), "startZ") : 0;
                    const auto entries = chunk.find("block");
                    if (entries == chunk.end() || !entries->is_array()) throw std::runtime_error("block list is missing");
                    for (std::size_t entry_index = 0; entry_index < entries->size(); ++entry_index) {
                        try {
                            const auto& tuple = (*entries)[entry_index];
                            if (!tuple.is_array() || tuple.size() < 5) throw std::runtime_error("tuple is invalid");
                            if (tuple[0].is_string()) continue;
                            const auto palette_index = integer(tuple[0], "palette index");
                            if (palette_index < 0 || static_cast<std::size_t>(palette_index) >= palette.size()) {
                                throw std::runtime_error("palette index " + std::to_string(palette_index) + " is out of range");
                            }
                            const auto aux = integer(tuple[1], "aux");
                            const auto xs = coordinates(tuple[2], "xs");
                            const auto ys = coordinates(tuple[3], "ys");
                            const auto zs = coordinates(tuple[4], "zs");
                            if (xs.size() != ys.size() || xs.size() != zs.size()) {
                                throw std::runtime_error("coordinate array lengths differ");
                            }
                            const auto& block_name = palette[static_cast<std::size_t>(palette_index)];
                            const auto runtime = runtime_id(block_name, aux);
                            for (std::size_t i = 0; i < xs.size(); ++i) {
                                const BlockPos world{ checked_add(start_x, xs[i], "x"), ys[i],
                                    checked_add(start_z, zs[i], "z") };
                                std::optional<NbtPayload> entity;
                                if (tuple.size() >= 6 && tuple[5].is_array() && i < tuple[5].size()) {
                                    if (auto compound = extra_nbt(block_name, tuple[5][i])) {
                                        entity = serialize_nbt(*compound);
                                    }
                                }
                                if (auto existing = blocks.find(world); existing != blocks.end()) {
                                    existing->second.runtime_id = runtime;
                                    if (!existing->second.nbt && entity) existing->second.nbt = std::move(entity);
                                } else {
                                    blocks.emplace(world, PendingBlock{ world, runtime, std::move(entity) });
                                }
                                bounds.add(world);
                            }
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " + std::to_string(entry_index) + ": " + error.what());
                        }
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error("chunk index " + std::to_string(chunk_index) + ": " + error.what());
                }
            }
        }

        if (blocks.empty() || mPaletteCache.empty()) throw std::runtime_error("structure has no valid blocks");
        mStore.set_size(bounds.size(name()));
        for (auto& [world, block] : blocks) {
            const auto local = local_position(world, bounds.minimum);
            mStore.put(local, block.runtime_id);
            if (block.nbt) mStore.put_entity(local, std::move(*block.nbt));
            if (block.runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse " + std::string(name()) + " failed: " + error.what());
    }
}

Result<void> FuHongStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> FuHongStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(
        std::string(name()) + " file writer is not implemented yet");
}

} // namespace water_structure
