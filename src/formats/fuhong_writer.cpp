#include "fuhong_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/stream_reader.h>
#include <nlohmann/json.hpp>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace water_structure {
namespace {

constexpr std::string_view kV5Key = "FuHongBuild";

struct PaletteValue {
    std::string name;
    std::int32_t aux = 0;
};

struct Group {
    std::vector<std::int32_t> xs;
    std::vector<std::int32_t> ys;
    std::vector<std::int32_t> zs;
    nlohmann::json extras = nlohmann::json::array();
    bool has_extras = false;
};

std::uint32_t block_at(
    const ChunkData& chunk,
    RuntimeRegistry& registry,
    int x,
    int y,
    int z)
{
    const auto sub_y = floor_div(y - 64, 16);
    const auto sub = chunk.sub_chunks.find(sub_y);
    if (sub == chunk.sub_chunks.end()) return registry.air_runtime_id();
    const auto local_y = y - (sub_y * 16 + 64);
    const auto index = static_cast<std::size_t>(
        (local_y * 16 + floor_mod(z, 16)) * 16 + floor_mod(x, 16));
    return sub->second.layer0[index];
}

std::string state_value(const BlockStateProperty& property)
{
    if (property.type == BlockStateValueType::Byte) {
        return property.value == "0" ? "false" : "true";
    }
    return property.value;
}

PaletteValue palette_value(RuntimeRegistry& registry, std::uint32_t runtime_id)
{
    auto state = registry.state(runtime_id).value_or(BlockState{ "minecraft:unknown", {}, 0 });
    if (state.name.find(':') == std::string::npos) state.name = "minecraft:" + state.name;
    for (std::uint32_t aux = 0; aux <= 255; ++aux) {
        if (registry.legacy_runtime_id(state.name, static_cast<std::uint16_t>(aux)) == runtime_id) {
            return { state.name, static_cast<std::int32_t>(aux) };
        }
    }
    if (state.states.empty()) return { state.name, 0 };
    auto properties = state.states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::string name = state.name + '[';
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) name += ',';
        name += properties[index].name + '=' + state_value(properties[index]);
    }
    name += ']';
    return { std::move(name), 0 };
}

std::string base_name(std::string name)
{
    if (const auto bracket = name.find('['); bracket != std::string::npos) name.resize(bracket);
    return name;
}

bool needs_extra(const std::string& name)
{
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (lower.find("command_block") != std::string::npos || lower.find("sign") != std::string::npos) {
        return true;
    }
    const auto base = base_name(lower);
    return base == "minecraft:chest" || base == "minecraft:trapped_chest" ||
        base == "minecraft:barrel" || base == "minecraft:hopper" ||
        base == "minecraft:dispenser" || base == "minecraft:dropper" ||
        base == "minecraft:furnace" || base == "minecraft:lit_furnace" ||
        base == "minecraft:blast_furnace" || base == "minecraft:lit_blast_furnace" ||
        base == "minecraft:smoker" || base == "minecraft:lit_smoker" ||
        base == "minecraft:crafter" || base.find("shulker_box") != std::string::npos;
}

const nbt::value* find_value(const nbt::tag_compound& compound, const char* key)
{
    return compound.has_key(key) ? &compound.at(key) : nullptr;
}

std::string string_value(const nbt::value* value)
{
    return value && value->get_type() == nbt::tag_type::String
        ? value->as<nbt::tag_string>().get()
        : std::string{};
}

std::int64_t int_value(const nbt::value* value)
{
    if (!value) return 0;
    switch (value->get_type()) {
    case nbt::tag_type::Byte: return value->as<nbt::tag_byte>().get();
    case nbt::tag_type::Short: return value->as<nbt::tag_short>().get();
    case nbt::tag_type::Int: return value->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return value->as<nbt::tag_long>().get();
    default: return 0;
    }
}

nlohmann::json extra_payload(const std::string& block_name, const nbt::tag_compound* nbt)
{
    auto lower = block_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (lower.find("command_block") != std::string::npos) {
        if (!nbt) return nlohmann::json::array({ "", 0, 0, "" });
        return nlohmann::json::array({
            string_value(find_value(*nbt, "Command")),
            int_value(find_value(*nbt, "TickDelay")),
            int_value(find_value(*nbt, "auto")) != 0 ? 1 : 0,
            string_value(find_value(*nbt, "CustomName"))
        });
    }
    if (base_name(lower).find("sign") != std::string::npos) {
        if (!nbt) return "";
        if (const auto* front = find_value(*nbt, "FrontText");
            front && front->get_type() == nbt::tag_type::Compound) {
            return string_value(find_value(front->as<nbt::tag_compound>(), "Text"));
        }
        return string_value(find_value(*nbt, "Text"));
    }
    nlohmann::json items = nlohmann::json::array();
    if (!nbt) return items;
    const auto* raw_items = find_value(*nbt, "Items");
    if (!raw_items || raw_items->get_type() != nbt::tag_type::List) return items;
    for (const auto& raw : raw_items->as<nbt::tag_list>()) {
        if (raw.get_type() != nbt::tag_type::Compound) continue;
        const auto& item = raw.as<nbt::tag_compound>();
        const auto name = string_value(find_value(item, "Name"));
        if (name.empty()) continue;
        items.push_back(nlohmann::json::array({
            name,
            int_value(find_value(item, "Damage")),
            int_value(find_value(item, "Count")),
            int_value(find_value(item, "Slot"))
        }));
    }
    return items;
}

std::pair<std::uint32_t, std::size_t> decode_utf8(std::string_view data, std::size_t offset)
{
    const auto first = static_cast<std::uint8_t>(data[offset]);
    std::size_t length = 0;
    std::uint32_t value = 0;
    if (first < 0x80) { length = 1; value = first; }
    else if ((first & 0xe0) == 0xc0) { length = 2; value = first & 0x1f; }
    else if ((first & 0xf0) == 0xe0) { length = 3; value = first & 0x0f; }
    else if ((first & 0xf8) == 0xf0) { length = 4; value = first & 0x07; }
    else throw std::runtime_error("FuHongV5 输入包含无效 UTF-8");
    if (length > data.size() - offset) throw std::runtime_error("FuHongV5 输入 UTF-8 截断");
    for (std::size_t index = 1; index < length; ++index) {
        const auto next = static_cast<std::uint8_t>(data[offset + index]);
        if ((next & 0xc0) != 0x80) throw std::runtime_error("FuHongV5 输入包含无效 UTF-8");
        value = (value << 6) | (next & 0x3f);
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

std::string encode_v5(std::string_view plain)
{
    std::string output;
    output.reserve(plain.size());
    std::size_t offset = 0;
    std::size_t rune_index = 0;
    while (offset < plain.size()) {
        const auto [value, length] = decode_utf8(plain, offset);
        const auto encrypted = (value ^ static_cast<std::uint8_t>(kV5Key[rune_index % kV5Key.size()])) +
            static_cast<std::uint32_t>(rune_index % 3);
        if (encrypted > 0x10ffff || (encrypted >= 0xd800 && encrypted <= 0xdfff)) {
            throw std::runtime_error("FuHongV5 加密 rune 越界");
        }
        append_utf8(output, encrypted);
        offset += length;
        ++rune_index;
    }
    return output;
}

Result<void> write_zlib(std::string_view data, const std::filesystem::path& output_path)
{
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 FuHongV5: " + output_path.string());
    z_stream stream{};
    if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return Result<void>::failure("初始化 FuHongV5 zlib 失败");
    }
    struct Guard { z_stream* stream; ~Guard() { deflateEnd(stream); } } guard{ &stream };
    std::array<std::uint8_t, 64 * 1024> encoded{};
    std::size_t consumed = 0;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0 && consumed < data.size()) {
            const auto count = std::min<std::size_t>(
                data.size() - consumed, std::numeric_limits<uInt>::max());
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + consumed));
            stream.avail_in = static_cast<uInt>(count);
            consumed += count;
        }
        stream.next_out = encoded.data();
        stream.avail_out = static_cast<uInt>(encoded.size());
        const auto flush = consumed == data.size() && stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH;
        status = deflate(&stream, flush);
        if (status != Z_OK && status != Z_STREAM_END) {
            return Result<void>::failure("FuHongV5 zlib 压缩失败");
        }
        output.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size() - stream.avail_out));
    }
    return output ? Result<void>::success() : Result<void>::failure("写入 FuHongV5 失败");
}

} // namespace

Result<void> write_fuhong(
    const IStructure& structure,
    RuntimeRegistry& registry,
    StructureId format,
    const std::filesystem::path& output_path)
{
    if (format != StructureId::FuHongV4 && format != StructureId::FuHongV5) {
        return Result<void>::failure("FuHong writer 收到不支持的版本");
    }
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 || size.volume() <= 0) {
        return Result<void>::failure("FuHong 输出尺寸无效");
    }
    try {
        std::vector<ChunkPos> positions;
        for (int chunk_x = 0; chunk_x < size.chunk_x_count(); ++chunk_x) {
            for (int chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
                positions.push_back({ chunk_x, chunk_z });
            }
        }
        auto entities = structure.get_chunk_nbt(positions);
        if (!entities) throw std::runtime_error("生成 FuHong NBT 失败: " + entities.error());
        std::unordered_map<BlockPos, std::unique_ptr<nbt::tag_compound>, BlockPosHash> entity_by_pos;
        for (const auto& [chunk, values] : entities.value()) {
            for (const auto& entity : values) {
                if (entity.payload.empty()) continue;
                const std::string bytes(entity.payload.begin(), entity.payload.end());
                std::istringstream input(bytes, std::ios::binary);
                auto [_, compound] = nbt::io::read_compound(input, endian::little);
                entity_by_pos.emplace(BlockPos{
                    chunk.x * 16 + entity.pos.x,
                    entity.pos.y - kOverworldMinY,
                    chunk.z * 16 + entity.pos.z }, std::move(compound));
            }
        }

        std::vector<std::string> palette{ "minecraft:air" };
        std::unordered_map<std::string, std::int32_t> palette_indices{ { "minecraft:air", 0 } };
        std::unordered_map<std::uint32_t, PaletteValue> resolved;
        std::map<std::pair<std::string, std::int32_t>, Group> groups;
        for (const auto chunk_position : positions) {
            const std::array<ChunkPos, 1> request{ chunk_position };
            auto chunks = structure.get_chunks(request);
            if (!chunks) throw std::runtime_error("生成 FuHong chunk 失败: " + chunks.error());
            const auto chunk_it = chunks.value().find(chunk_position);
            const ChunkData empty;
            const auto& chunk = chunk_it == chunks.value().end() ? empty : chunk_it->second;
            const auto x_end = std::min(size.width, (chunk_position.x + 1) * 16);
            const auto z_end = std::min(size.length, (chunk_position.z + 1) * 16);
            for (int y = 0; y < size.height; ++y) {
                for (int z = chunk_position.z * 16; z < z_end; ++z) {
                    for (int x = chunk_position.x * 16; x < x_end; ++x) {
                        const auto runtime_id = block_at(chunk, registry, x, y, z);
                        if (runtime_id == registry.air_runtime_id()) continue;
                        auto found = resolved.find(runtime_id);
                        if (found == resolved.end()) {
                            found = resolved.emplace(runtime_id, palette_value(registry, runtime_id)).first;
                        }
                        if (!palette_indices.contains(found->second.name)) {
                            palette_indices.emplace(found->second.name,
                                static_cast<std::int32_t>(palette.size()));
                            palette.push_back(found->second.name);
                        }
                        auto& group = groups[{ found->second.name, found->second.aux }];
                        group.xs.push_back(x);
                        group.ys.push_back(y);
                        group.zs.push_back(z);
                        if (needs_extra(found->second.name)) {
                            const auto entity = entity_by_pos.find({ x, y, z });
                            group.extras.push_back(extra_payload(found->second.name,
                                entity == entity_by_pos.end() ? nullptr : entity->second.get()));
                            group.has_extras = true;
                        }
                    }
                }
            }
        }
        if (groups.empty()) return Result<void>::failure("FuHong 未导出任何方块");

        nlohmann::json block_entries = nlohmann::json::array();
        for (const auto& [key, group] : groups) {
            nlohmann::json entry = nlohmann::json::array({
                palette_indices.at(key.first), key.second, group.xs, group.ys, group.zs
            });
            if (group.has_extras) entry.push_back(group.extras);
            block_entries.push_back(std::move(entry));
        }
        nlohmann::json root{
            { "Build_Info", nlohmann::json::object() },
            { "FuHongBuild", nlohmann::json::array({ {
                { "startX", 0 }, { "startZ", 0 }, { "block", std::move(block_entries) }
            } }) },
            { "BlocksList", palette },
            { "TimeUsed", "0ms" }
        };
        const auto plain = root.dump();
        if (format == StructureId::FuHongV5) {
            return write_zlib(encode_v5(plain), output_path);
        }
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output) return Result<void>::failure("无法创建 FuHongV4: " + output_path.string());
        output.write(plain.data(), static_cast<std::streamsize>(plain.size()));
        return output ? Result<void>::success() : Result<void>::failure("写入 FuHongV4 失败");
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 FuHong 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
