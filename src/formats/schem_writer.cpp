#include "schem_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/ozlibstream.h>
#include <io/stream_writer.h>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_primitive.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace water_structure {
namespace {

constexpr std::int32_t kJavaDataVersion = 4556;

std::uint32_t block_at(
    const ChunkMap& chunks,
    RuntimeRegistry& registry,
    int x,
    int y,
    int z)
{
    const ChunkPos chunk_pos{ floor_div(x, 16), floor_div(z, 16) };
    const auto chunk = chunks.find(chunk_pos);
    if (chunk == chunks.end()) return registry.air_runtime_id();
    const auto sub_y = floor_div(y - 64, 16);
    const auto sub = chunk->second.sub_chunks.find(sub_y);
    if (sub == chunk->second.sub_chunks.end()) return registry.air_runtime_id();
    const auto local_y = y - (sub_y * 16 + 64);
    const auto index = static_cast<std::size_t>(
        (local_y * 16 + floor_mod(z, 16)) * 16 + floor_mod(x, 16));
    return sub->second.layer0[index];
}

void append_varint(std::vector<std::int8_t>& output, std::uint32_t value)
{
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
        if (value != 0) byte |= 0x80u;
        output.push_back(static_cast<std::int8_t>(byte));
    } while (value != 0);
}

std::string java_state_string(RuntimeRegistry& registry, std::uint32_t runtime_id)
{
    const auto state = registry.java_state(runtime_id);
    if (!state) return "minecraft:air";
    auto name = state->name;
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    if (state->states.empty()) return name;
    auto properties = state->states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    name += '[';
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) name += ',';
        name += properties[index].name;
        name += '=';
        if (properties[index].type == BlockStateValueType::Byte) {
            name += properties[index].value == "0" ? "false" : "true";
        } else {
            name += properties[index].value;
        }
    }
    name += ']';
    return name;
}

} // namespace

Result<void> write_schem(
    const IStructure& structure,
    RuntimeRegistry& registry,
    StructureId format,
    const std::filesystem::path& output_path)
{
    if (format != StructureId::SchemV1 && format != StructureId::SchemV2) {
        return Result<void>::failure("Schem writer 收到不支持的格式");
    }
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 ||
        size.width > std::numeric_limits<std::int16_t>::max() ||
        size.height > std::numeric_limits<std::int16_t>::max() ||
        size.length > std::numeric_limits<std::int16_t>::max() ||
        size.volume() <= 0 || size.volume() > std::numeric_limits<std::int32_t>::max()) {
        return Result<void>::failure("Schem 输出尺寸无效、超过 int16 或体积超过 int32");
    }

    std::vector<ChunkPos> positions;
    positions.reserve(static_cast<std::size_t>(size.chunk_x_count()) * size.chunk_z_count());
    for (int x = 0; x < size.chunk_x_count(); ++x) {
        for (int z = 0; z < size.chunk_z_count(); ++z) positions.push_back({ x, z });
    }
    auto chunks = structure.get_chunks(positions);
    if (!chunks) return Result<void>::failure("生成 Schem chunks 失败: " + chunks.error());

    std::unordered_map<std::uint32_t, std::int32_t> palette_indices;
    std::vector<std::uint32_t> palette;
    std::vector<std::int8_t> block_data;
    block_data.reserve(static_cast<std::size_t>(size.volume()));
    if (format == StructureId::SchemV1) {
        palette_indices.emplace(registry.air_runtime_id(), 0);
        palette.push_back(registry.air_runtime_id());
    }
    for (int y = 0; y < size.height; ++y) {
        for (int z = 0; z < size.length; ++z) {
            for (int x = 0; x < size.width; ++x) {
                const auto runtime_id = block_at(chunks.value(), registry, x, y, z);
                auto found = palette_indices.find(runtime_id);
                if (found == palette_indices.end()) {
                    const auto index = static_cast<std::int32_t>(palette.size());
                    found = palette_indices.emplace(runtime_id, index).first;
                    palette.push_back(runtime_id);
                }
                append_varint(block_data, static_cast<std::uint32_t>(found->second));
            }
        }
    }
    if (block_data.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return Result<void>::failure("Schem BlockData 超过 int32");
    }

    nbt::tag_compound encoded_palette;
    for (std::size_t index = 0; index < palette.size(); ++index) {
        encoded_palette[java_state_string(registry, palette[index])] =
            nbt::tag_int(static_cast<std::int32_t>(index));
    }

    nbt::tag_compound root;
    root["DataVersion"] = nbt::tag_int(kJavaDataVersion);
    root["Width"] = nbt::tag_short(static_cast<std::int16_t>(size.width));
    root["Height"] = nbt::tag_short(static_cast<std::int16_t>(size.height));
    root["Length"] = nbt::tag_short(static_cast<std::int16_t>(size.length));
    if (format == StructureId::SchemV1) {
        root["BlockData"] = nbt::tag_byte_array(std::move(block_data));
        root["PaletteMax"] = nbt::tag_int(static_cast<std::int32_t>(palette.size()));
        root["Palette"] = std::move(encoded_palette);
    } else {
        root["Version"] = nbt::tag_int(2);
        nbt::tag_compound blocks;
        blocks["Data"] = nbt::tag_byte_array(std::move(block_data));
        blocks["Palette"] = std::move(encoded_palette);
        root["Blocks"] = std::move(blocks);
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 Schem: " + output_path.string());
    try {
        zlib::ozlibstream compressed(output, Z_BEST_SPEED, true);
        nbt::io::write_tag("Schematic", root, compressed, endian::big);
        compressed.close();
        output.close();
        if (!output) return Result<void>::failure("写入 Schem 失败");
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 Schem 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
