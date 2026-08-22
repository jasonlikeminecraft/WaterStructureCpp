#include "kbdx_structure.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace water_structure {

namespace {

// KBDX records precede their trailing JSON palette. The parser seeks to the
// bounded palette first and then decodes records directly into final storage,
// avoiding a second record-sized allocation.
constexpr std::uint32_t kMaxRawBlocks = 8'000'000;
constexpr std::size_t kMaxMetadataBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxBlockEntities = 1'000'000;
constexpr std::size_t kMaxJsonDepth = 128;
constexpr std::size_t kMaxRuntimeCacheEntries = 65'536;

void validate_json_depth(std::string_view input)
{
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const auto character : input) {
        if (quoted && escaped) {
            escaped = false;
            continue;
        }
        if (quoted && character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted) continue;
        if (character == '{' || character == '[') {
            if (++depth > kMaxJsonDepth) {
                throw std::runtime_error("KBDX JSON nesting exceeds limit");
            }
        } else if (character == '}' || character == ']') {
            if (depth != 0) --depth;
        }
    }
}

std::int32_t read_i32(const std::array<std::uint8_t, 4>& bytes)
{
    const auto value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
    return static_cast<std::int32_t>(value);
}

std::uint32_t read_u32(std::istream& input)
{
    std::array<std::uint8_t, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("KBDX 文件意外结束");
    }
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

std::uint32_t block_at(const SubChunkData& chunk, int layer, int x, int y, int z)
{
    const auto index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
    return layer == 0 ? chunk.layer0[index] : chunk.layer1[index];
}

std::uint32_t& block_at(SubChunkData& chunk, int layer, int x, int y, int z)
{
    const auto index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
    return layer == 0 ? chunk.layer0[index] : chunk.layer1[index];
}

std::optional<std::int32_t> json_int(const nlohmann::json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end()) return std::nullopt;
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return static_cast<std::int32_t>(value);
    }
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(value);
    }
    if (found->is_number_float()) {
        const auto value = found->get<double>();
        if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return static_cast<std::int32_t>(value);
    }
    return std::nullopt;
}

bool json_bool(const nlohmann::json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end()) return false;
    if (found->is_boolean()) return found->get<bool>();
    if (found->is_number_integer()) return found->get<std::int64_t>() != 0;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>() != 0;
    if (found->is_number_float()) return found->get<double>() != 0;
    if (found->is_string()) {
        auto value = found->get<std::string>();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value == "true" || value == "1";
    }
    return false;
}

std::string json_string(const nlohmann::json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return "<nil>";
    if (found->is_string()) return found->get<std::string>();
    if (found->is_boolean()) return found->get<bool>() ? "true" : "false";
    return found->dump();
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string kbdx_entity_id(std::string value)
{
    value = lowercase(std::move(value));
    if (value.starts_with("minecraft:")) value.erase(0, std::string("minecraft:").size());
    if (value == "command_block" || value == "repeating_command_block" ||
        value == "chain_command_block") return "CommandBlock";
    if (value == "chest" || value == "trapped_chest") return "Chest";
    if (value == "barrel") return "Barrel";
    if (value == "hopper") return "Hopper";
    if (value == "dispenser") return "Dispenser";
    if (value == "dropper") return "Dropper";
    if (value == "blast_furnace") return "BlastFurnace";
    if (value == "furnace") return "Furnace";
    if (value == "smoker") return "Smoker";
    if (value == "crafter") return "Crafter";
    if (value.find("shulker_box") != std::string::npos) return "ShulkerBox";
    return {};
}

std::string item_name(const nlohmann::json& item)
{
    std::string name;
    for (const auto key : { "Name", "name", "ns" }) {
        const auto found = item.find(key);
        if (found != item.end() && found->is_string()) {
            name = found->get<std::string>();
            break;
        }
    }
    const auto begin = name.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = name.find_last_not_of(" \t\r\n");
    name = name.substr(begin, end - begin + 1);
    if (!name.empty() && name.find(':') == std::string::npos) name = "minecraft:" + name;
    return name;
}

std::int64_t item_integer(const nlohmann::json& item,
    std::initializer_list<const char*> keys)
{
    for (const auto key : keys) {
        const auto found = item.find(key);
        if (found == item.end()) continue;
        if (found->is_number_integer()) return found->get<std::int64_t>();
        if (found->is_number_unsigned()) return static_cast<std::int64_t>(found->get<std::uint64_t>());
        if (found->is_number_float()) return static_cast<std::int64_t>(found->get<double>());
        if (found->is_string()) {
            std::int64_t value = 0;
            const auto text = found->get<std::string>();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return value;
        }
    }
    return 0;
}

NbtPayload serialize_entity(const nbt::tag_compound& root)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", root, output, endian::little);
    const auto bytes = output.str();
    return { bytes.begin(), bytes.end() };
}

NbtPayload command_block_nbt(const nlohmann::json& entity)
{
    const auto command = json_string(entity, "Command");
    const auto uses_modern_execute = [](std::string_view text) noexcept {
        static constexpr std::array<std::string_view, 11> subcommands{
            "as", "at", "align", "anchored", "facing", "in",
            "positioned", "rotated", "if", "unless", "run"
        };
        std::size_t search = 0;
        while (search < text.size()) {
            const auto execute = text.find("execute", search);
            if (execute == std::string_view::npos) return false;
            auto next = execute + std::string_view("execute").size();
            while (next < text.size() && text[next] == ' ') ++next;
            const auto tail = text.substr(next);
            if (std::any_of(subcommands.begin(), subcommands.end(),
                [tail](std::string_view subcommand) {
                    return tail.starts_with(subcommand);
                })) return true;
            search = execute + 1;
        }
        return false;
    };
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("CommandBlock");
    root["Command"] = nbt::tag_string(command);
    root["CustomName"] = nbt::tag_string(json_string(entity, "CustomName"));
    root["ExecuteOnFirstTick"] = nbt::tag_byte(json_bool(entity, "ExecuteOnFirstTick") ? 1 : 0);
    root["TrackOutput"] = nbt::tag_byte(json_bool(entity, "TrackOutput") ? 1 : 0);
    root["conditionalMode"] = nbt::tag_byte(json_bool(entity, "isConditional") ? 1 : 0);
    root["auto"] = nbt::tag_byte(json_bool(entity, "redstone") ? 0 : 1);
    root["TickDelay"] = nbt::tag_int(json_int(entity, "TickDelay").value_or(0));
    root["Powered"] = nbt::tag_byte(0);
    root["LPCommandMode"] = nbt::tag_int(json_int(entity, "Mode").value_or(0));
    root["LastOutput"] = nbt::tag_string(json_string(entity, "LastOutput"));
    root["Version"] = nbt::tag_int(uses_modern_execute(command) ? 38 : 19);

    return serialize_entity(root);
}

NbtPayload container_nbt(const nlohmann::json& entity)
{
    std::string id_value;
    if (const auto found = entity.find("id");
        found != entity.end() && found->is_string()) {
        id_value = found->get<std::string>();
    }
    const auto items = entity.find("Items");
    const auto has_items = items != entity.end() && items->is_array();
    if (id_value.empty() && !has_items) return {};

    nbt::tag_compound root;
    const auto mapped_id = kbdx_entity_id(id_value);
    if (!mapped_id.empty()) root["id"] = nbt::tag_string(mapped_id);
    else if (!id_value.empty()) root["id"] = nbt::tag_string(id_value);

    nbt::tag_list encoded_items(nbt::tag_type::Compound);
    if (has_items) {
        for (const auto& raw : *items) {
            if (!raw.is_object()) continue;
            const auto name = item_name(raw);
            if (name.empty()) continue;
            nbt::tag_compound item;
            item["Name"] = nbt::tag_string(name);
            item.emplace<nbt::tag_short>("Damage", static_cast<std::int16_t>(
                item_integer(raw, { "Damage", "damage", "aux" })));
            item.emplace<nbt::tag_byte>("Count", static_cast<std::int8_t>(
                item_integer(raw, { "Count", "count", "num" })));
            item.emplace<nbt::tag_byte>("Slot", static_cast<std::int8_t>(
                item_integer(raw, { "Slot", "slot" })));
            encoded_items.push_back(std::move(item));
        }
    }
    root["Items"] = std::move(encoded_items);
    for (const auto key : { "CustomName", "Lock" }) {
        const auto found = entity.find(key);
        if (found != entity.end() && found->is_string()) {
            root[key] = nbt::tag_string(found->get<std::string>());
        }
    }
    return serialize_entity(root);
}

NbtPayload sign_nbt(const nlohmann::json& entity)
{
    std::string id;
    if (const auto found = entity.find("id");
        found != entity.end() && found->is_string()) {
        id = lowercase(found->get<std::string>());
    }
    if (id.find("sign") == std::string::npos) return {};
    std::string text;
    if (const auto found = entity.find("Text"); found != entity.end()) {
        if (found->is_string()) text = found->get<std::string>();
        else if (found->is_array()) {
            for (std::size_t index = 0; index < found->size(); ++index) {
                if (index != 0) text.push_back('\n');
                text += found->at(index).is_string()
                    ? found->at(index).get<std::string>() : found->at(index).dump();
            }
        }
    }
    if (text.empty()) {
        if (const auto front = entity.find("FrontText");
            front != entity.end() && front->is_object()) {
            const auto found = front->find("Text");
            if (found != front->end() && found->is_string()) text = found->get<std::string>();
        }
    }
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("Sign");
    if (!text.empty()) root["Text"] = nbt::tag_string(std::move(text));
    if (const auto color = entity.find("Color");
        color != entity.end() && color->is_string()) {
        root["Color"] = nbt::tag_string(color->get<std::string>());
    }
    if (const auto glowing = entity.find("GlowingText"); glowing != entity.end()) {
        bool value = glowing->is_boolean() ? glowing->get<bool>() :
            (glowing->is_number() && glowing->get<double>() != 0.0);
        root["GlowingText"] = nbt::tag_byte(value ? 1 : 0);
    }
    return serialize_entity(root);
}

} // namespace

void KbdxStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mChunkIndex.clear();
    const auto expanded = [](std::int32_t base, std::int32_t delta) noexcept {
        const auto magnitude = delta < 0
            ? -static_cast<std::int64_t>(delta)
            : static_cast<std::int64_t>(delta);
        const auto value = static_cast<std::int64_t>(base) + magnitude;
        return value > std::numeric_limits<std::int32_t>::max()
            ? std::numeric_limits<std::int32_t>::max()
            : static_cast<std::int32_t>(value);
    };
    mSize = {
        expanded(mOriginalSize.width, offset.x),
        expanded(mOriginalSize.height, offset.y),
        expanded(mOriginalSize.length, offset.z)
    };
}

Result<void> KbdxStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::failure("无法打开 KBDX 文件: " + path.string());
    }

    try {
        std::error_code size_error;
        const auto file_size = std::filesystem::file_size(path, size_error);
        if (size_error || file_size < sizeof(std::uint32_t)) {
            return Result<void>::failure(
                "无法确定 KBDX 文件大小: " +
                (size_error ? size_error.message() : std::string("文件过短")));
        }
        const auto block_count = read_u32(input);
        constexpr std::uintmax_t kRecordBytes = 20;
        const auto records_end = sizeof(std::uint32_t) +
            static_cast<std::uintmax_t>(block_count) * kRecordBytes;
        if (block_count == 0 || block_count > kMaxRawBlocks) {
            return Result<void>::failure("KBDX 方块数量无效");
        }
        if (records_end > file_size) {
            return Result<void>::failure(
                "KBDX 方块记录截断，期望偏移 " + std::to_string(records_end));
        }

        // The palette is stored after the records.  Read and decode that
        // trailing metadata first, then seek back and resolve records directly
        // into the final block vector.  Keeping a second `RawBlock` vector
        // doubled the peak memory for large KBDX files (roughly 160 MiB at
        // eight million records) and made the format violate the bounded
        // streaming contract even though the input is seekable.
        const auto metadata_start = records_end;
        const auto metadata_bytes = file_size - metadata_start;
        if (metadata_bytes == 0 || metadata_bytes > kMaxMetadataBytes) {
            return Result<void>::failure(
                "KBDX 元数据大小超过限制 " + std::to_string(kMaxMetadataBytes));
        }
        input.seekg(static_cast<std::streamoff>(metadata_start), std::ios::beg);
        if (!input) return Result<void>::failure("KBDX 元数据偏移无效");
        std::string metadata;
        metadata.resize(static_cast<std::size_t>(metadata_bytes));
        input.read(metadata.data(), static_cast<std::streamsize>(metadata.size()));
        if (input.gcount() != static_cast<std::streamsize>(metadata.size())) {
            return Result<void>::failure("KBDX 元数据截断");
        }
        if (metadata.empty()) {
            return Result<void>::failure("KBDX 缺少元数据 JSON");
        }
        validate_json_depth(metadata);
        auto root = nlohmann::json::parse(metadata);
        if (!root.is_object()) {
            return Result<void>::failure("KBDX 元数据根节点不是对象");
        }

        std::unordered_map<BlockPos, NbtPayload, BlockPosHash> block_entities;
        if (const auto entities = root.find("BlockEntityData");
            entities != root.end() && entities->is_array()) {
            for (const auto& entity : *entities) {
                if (block_entities.size() >= kMaxBlockEntities) {
                    throw std::runtime_error(
                        "KBDX 方块实体数量超过限制 " +
                        std::to_string(kMaxBlockEntities));
                }
                if (!entity.is_object()) continue;
                const auto x = json_int(entity, "x");
                const auto y = json_int(entity, "y");
                const auto z = json_int(entity, "z");
                if (!x || !y || !z) continue;
                std::string id;
                if (const auto id_value = entity.find("id");
                    id_value != entity.end() && id_value->is_string()) {
                    id = id_value->get<std::string>();
                }
                NbtPayload payload;
                if (lowercase(id).ends_with("command_block")) {
                    payload = command_block_nbt(entity);
                } else if (const auto container = container_nbt(entity); !container.empty()) {
                    payload = container;
                } else if (const auto sign = sign_nbt(entity); !sign.empty()) {
                    payload = sign;
                } else {
                    // Preserve scalar fields for unknown actors. Coordinates are
                    // structure metadata, not part of the Bedrock actor NBT.
                    nbt::tag_compound generic;
                    bool has_generic_field = false;
                    const auto mapped = kbdx_entity_id(id);
                    if (!mapped.empty()) {
                        generic["id"] = nbt::tag_string(mapped);
                        has_generic_field = true;
                    } else if (!id.empty()) {
                        generic["id"] = nbt::tag_string(id);
                        has_generic_field = true;
                    }
                    for (const auto& [key, value] : entity.items()) {
                        if (key == "x" || key == "y" || key == "z" || key == "id") continue;
                        if (value.is_string()) {
                            generic[key] = nbt::tag_string(value.get<std::string>());
                            has_generic_field = true;
                        } else if (value.is_boolean()) {
                            generic.emplace<nbt::tag_byte>(key, value.get<bool>() ? 1 : 0);
                            has_generic_field = true;
                        } else if (value.is_number_integer()) {
                            generic.emplace<nbt::tag_int>(key,
                                static_cast<std::int32_t>(value.get<std::int64_t>()));
                            has_generic_field = true;
                        } else if (value.is_number_unsigned()) {
                            generic.emplace<nbt::tag_int>(key,
                                static_cast<std::int32_t>(value.get<std::uint64_t>()));
                            has_generic_field = true;
                        } else if (value.is_number_float()) {
                            generic.emplace<nbt::tag_double>(key, value.get<double>());
                            has_generic_field = true;
                        }
                    }
                    if (has_generic_field) payload = serialize_entity(generic);
                }
                if (!payload.empty()) block_entities[{ *x, *y, *z }] = std::move(payload);
            }
        }

        std::unordered_map<std::uint32_t, std::string> palette;
        for (const auto& [name, value] : root.items()) {
            if (value.is_number_integer()) {
                const auto index = value.get<std::int64_t>();
                if (index < 0 || index > std::numeric_limits<std::uint32_t>::max()) {
                    continue;
                }
                palette[static_cast<std::uint32_t>(index)] = name;
            }
        }
        if (palette.empty()) {
            return Result<void>::failure("KBDX 元数据没有 palette");
        }
        // Palette strings and supported block-entity payloads are now owned by
        // their compact containers. Release the potentially much larger JSON
        // DOM before allocating the block vector.
        root = nlohmann::json{};
        metadata.clear();
        metadata.shrink_to_fit();

        std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_y = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_z = std::numeric_limits<std::int32_t>::max();
        std::int32_t max_x = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_y = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_z = std::numeric_limits<std::int32_t>::min();

        mBlocks.clear();
        mBlocks.reserve(block_count);
        mBlockEntities.clear();
        mNonAirBlocks = 0;
        std::unordered_map<std::uint64_t, std::uint32_t> runtime_cache;
        runtime_cache.reserve(std::min<std::size_t>(palette.size() * 4, 4096));
        input.clear();
        input.seekg(static_cast<std::streamoff>(sizeof(std::uint32_t)), std::ios::beg);
        if (!input) throw std::runtime_error("KBDX 无法回到方块记录起点");
        for (std::uint32_t i = 0; i < block_count; ++i) {
            std::array<std::uint8_t, 4> bytes{};
            input.read(reinterpret_cast<char*>(bytes.data()), 4);
            if (input.gcount() != 4) throw std::runtime_error("读取 KBDX X 失败");
            const auto x = read_i32(bytes);
            input.read(reinterpret_cast<char*>(bytes.data()), 4);
            if (input.gcount() != 4) throw std::runtime_error("读取 KBDX Y 失败");
            const auto y = read_i32(bytes);
            input.read(reinterpret_cast<char*>(bytes.data()), 4);
            if (input.gcount() != 4) throw std::runtime_error("读取 KBDX Z 失败");
            const auto z = read_i32(bytes);
            const auto palette_index = read_u32(input);
            const auto aux = read_u32(input);
            const auto runtime_key = static_cast<std::uint64_t>(palette_index) << 32u | aux;
            std::uint32_t runtime_id = 0;
            if (const auto cached = runtime_cache.find(runtime_key);
                cached != runtime_cache.end()) {
                runtime_id = cached->second;
            } else {
                const auto palette_it = palette.find(palette_index);
                auto name = palette_it == palette.end() ? std::string{} : palette_it->second;
                name.erase(name.begin(), std::find_if(name.begin(), name.end(), [](unsigned char c) {
                    return !std::isspace(c);
                }));
                name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char c) {
                    return !std::isspace(c);
                }).base(), name.end());
                if (!name.empty() && name.find(':') == std::string::npos) {
                    name = "minecraft:" + name;
                }
                runtime_id = mRegistry.legacy_runtime_id(
                        name, static_cast<std::uint16_t>(aux))
                    .or_else([&] { return mRegistry.compatible_java_runtime_id(name); })
                    .value_or(mRegistry.find("minecraft:unknown")
                        .value_or(mRegistry.air_runtime_id()));
                if (runtime_cache.size() < kMaxRuntimeCacheEntries) {
                    runtime_cache.emplace(runtime_key, runtime_id);
                }
            }
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            min_z = std::min(min_z, z);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
            max_z = std::max(max_z, z);
            mBlocks.push_back({ x, y, z, runtime_id });
        }

        if (mBlocks.empty()) {
            return Result<void>::failure("KBDX 没有方块");
        }

        // Go accumulates records by absolute coordinate: the last runtime ID
        // wins and an entity is retained only when a block exists at the same
        // coordinate. Stable sorting provides exactly those semantics without
        // a second hash table whose nodes would dominate peak memory.
        std::stable_sort(mBlocks.begin(), mBlocks.end(), [](const Block& left, const Block& right) {
            if (left.x != right.x) return left.x < right.x;
            if (left.y != right.y) return left.y < right.y;
            return left.z < right.z;
        });
        std::size_t compacted = 0;
        for (std::size_t begin = 0; begin < mBlocks.size();) {
            auto end = begin + 1;
            while (end < mBlocks.size() &&
                mBlocks[end].x == mBlocks[begin].x &&
                mBlocks[end].y == mBlocks[begin].y &&
                mBlocks[end].z == mBlocks[begin].z) ++end;
            mBlocks[compacted++] = std::move(mBlocks[end - 1]);
            begin = end;
        }
        mBlocks.resize(compacted);

        mNonAirBlocks = 0;
        for (auto& block : mBlocks) {
            const BlockPos absolute{ block.x, block.y, block.z };
            if (auto entity = block_entities.find(absolute);
                entity != block_entities.end()) {
                mBlockEntities.emplace(
                    BlockPos{ block.x - min_x, block.y - min_y, block.z - min_z },
                    std::move(entity->second));
            }
            block.x -= min_x;
            block.y -= min_y;
            block.z -= min_z;
            if (block.runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
        }
        std::sort(mBlocks.begin(), mBlocks.end(), [](const Block& left, const Block& right) {
            if (left.y != right.y) return left.y < right.y;
            if (left.z != right.z) return left.z < right.z;
            return left.x < right.x;
        });
        const auto dimension = [](std::int32_t minimum,
                                  std::int32_t maximum,
                                  char axis) -> std::int32_t {
            const auto span = static_cast<std::int64_t>(maximum) - minimum + 1;
            if (span <= 0 || span > std::numeric_limits<std::int32_t>::max()) {
                throw std::runtime_error(
                    std::string("KBDX ") + axis + " 尺寸超出 int32 范围");
            }
            return static_cast<std::int32_t>(span);
        };
        mOriginalSize = {
            dimension(min_x, max_x, 'X'),
            dimension(min_y, max_y, 'Y'),
            dimension(min_z, max_z, 'Z')
        };
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 KBDX 失败: ") + error.what());
    }
}

Result<ChunkMap> KbdxStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, ChunkData{});
    }

    for (const auto& block : mBlocks) {
        const auto x = static_cast<std::int64_t>(block.x) + mOffset.x;
        const auto y = static_cast<std::int64_t>(block.y) + mOffset.y;
        const auto z = static_cast<std::int64_t>(block.z) + mOffset.z;
        if (x < std::numeric_limits<std::int32_t>::min() ||
            x > std::numeric_limits<std::int32_t>::max() ||
            y < std::numeric_limits<std::int32_t>::min() ||
            y > std::numeric_limits<std::int32_t>::max() ||
            z < std::numeric_limits<std::int32_t>::min() ||
            z > std::numeric_limits<std::int32_t>::max()) {
            return Result<ChunkMap>::failure("KBDX offset moves block outside int32 range");
        }
    }

    if (!mChunkIndex.ensure(mBlocks, mOffset, [](const Block& block) {
        return BlockPos{ block.x, block.y, block.z };
    })) return Result<ChunkMap>::failure("KBDX chunk index 超过 uint32 容量");
    for (auto& [chunk_pos, chunk] : result) {
        const auto* indexed = mChunkIndex.find(chunk_pos);
        if (!indexed) continue;
        for (const auto index : *indexed) {
            const auto& block = mBlocks[index];
            const auto x = static_cast<std::int64_t>(block.x) + mOffset.x;
            const auto y = static_cast<std::int64_t>(block.y) + mOffset.y;
            const auto z = static_cast<std::int64_t>(block.z) + mOffset.z;
            const auto storage_y = y + kOverworldMinY;
            const auto sub_y_64 = floor_div64(storage_y, 16);
            if (sub_y_64 < std::numeric_limits<std::int32_t>::min() ||
                sub_y_64 > std::numeric_limits<std::int32_t>::max()) {
                return Result<ChunkMap>::failure("KBDX subchunk Y exceeds int32 range");
            }
            const auto sub_y = static_cast<std::int32_t>(sub_y_64);
            const auto chunk_min_x = static_cast<std::int64_t>(chunk_pos.x) * 16;
            const auto chunk_min_z = static_cast<std::int64_t>(chunk_pos.z) * 16;
            const auto local_x = x - chunk_min_x;
            const auto local_y = storage_y - sub_y_64 * 16;
            const auto local_z = z - chunk_min_z;
            if (local_x < 0 || local_x >= 16 || local_y < 0 || local_y >= 16 ||
                local_z < 0 || local_z >= 16) {
                return Result<ChunkMap>::failure("KBDX block materializes outside subchunk");
            }
            auto [sub_it, inserted] = chunk.sub_chunks.try_emplace(sub_y);
            if (inserted) {
                sub_it->second.layer0.fill(mRegistry.air_runtime_id());
                sub_it->second.layer1.fill(mRegistry.air_runtime_id());
            }
            block_at(sub_it->second, 0, static_cast<int>(local_x),
                static_cast<int>(local_y), static_cast<int>(local_z)) = block.runtime_id;
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> KbdxStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, std::vector<BlockEntity>{});
    }
    for (const auto& [source, payload] : mBlockEntities) {
        const auto x = static_cast<std::int64_t>(source.x) + mOffset.x;
        const auto y = static_cast<std::int64_t>(source.y) + mOffset.y;
        const auto z = static_cast<std::int64_t>(source.z) + mOffset.z;
        if (x < std::numeric_limits<std::int32_t>::min() ||
            x > std::numeric_limits<std::int32_t>::max() ||
            y < std::numeric_limits<std::int32_t>::min() ||
            y > std::numeric_limits<std::int32_t>::max() ||
            z < std::numeric_limits<std::int32_t>::min() ||
            z > std::numeric_limits<std::int32_t>::max()) {
            return Result<NbtChunkMap>::failure("KBDX offset moves entity outside int32 range");
        }
        const BlockPos position{
            static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)
        };
        const auto chunk = block_to_chunk(position);
        const auto found = result.find(chunk);
        if (found == result.end()) continue;
        found->second.push_back({
            {
                floor_mod(position.x, 16),
                structure_y_to_chunk_local(position.y),
                floor_mod(position.z, 16)
            },
            payload
        });
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> KbdxStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> KbdxStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("KBDX 导出尚未迁移");
}

} // namespace water_structure
