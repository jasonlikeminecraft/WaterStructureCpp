#include "litematic_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/ozlibstream.h>
#include <io/stream_writer.h>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>
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

std::uint8_t bits_for_palette(std::size_t size)
{
    std::uint8_t bits = 0;
    for (auto value = size > 0 ? size - 1 : 0; value != 0; value >>= 1u) ++bits;
    return std::max<std::uint8_t>(2, bits);
}

void put_property(nbt::tag_compound& states, const BlockStateProperty& property)
{
    switch (property.type) {
    case BlockStateValueType::Byte:
        states[property.name] = static_cast<std::int8_t>(std::stoi(property.value));
        break;
    case BlockStateValueType::Short:
        states[property.name] = static_cast<std::int16_t>(std::stoi(property.value));
        break;
    case BlockStateValueType::Int:
        states[property.name] = static_cast<std::int32_t>(std::stol(property.value));
        break;
    case BlockStateValueType::Long:
        states[property.name] = static_cast<std::int64_t>(std::stoll(property.value));
        break;
    case BlockStateValueType::String:
        states[property.name] = nbt::tag_string(property.value);
        break;
    }
}

nbt::tag_compound palette_entry(RuntimeRegistry& registry, std::uint32_t runtime_id)
{
    auto state = registry.java_state(runtime_id).value_or(BlockState{ "minecraft:air", {}, 0 });
    if (state.name.find(':') == std::string::npos) state.name = "minecraft:" + state.name;
    nbt::tag_compound result;
    result["Name"] = nbt::tag_string(state.name);
    if (!state.states.empty()) {
        nbt::tag_compound properties;
        for (const auto& property : state.states) put_property(properties, property);
        result["Properties"] = std::move(properties);
    }
    return result;
}

} // namespace

Result<void> write_litematic(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path)
{
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 ||
        size.volume() <= 0 || size.volume() > std::numeric_limits<std::int32_t>::max()) {
        return Result<void>::failure("Litematic 输出尺寸无效或体积超过 int32");
    }
    std::vector<ChunkPos> positions;
    positions.reserve(static_cast<std::size_t>(size.chunk_x_count()) * size.chunk_z_count());
    for (int x = 0; x < size.chunk_x_count(); ++x) {
        for (int z = 0; z < size.chunk_z_count(); ++z) positions.push_back({ x, z });
    }
    auto chunks = structure.get_chunks(positions);
    if (!chunks) return Result<void>::failure("生成 Litematic chunks 失败: " + chunks.error());

    std::unordered_map<std::uint32_t, std::int32_t> palette_indices;
    std::vector<std::uint32_t> palette{ registry.air_runtime_id() };
    palette_indices.emplace(registry.air_runtime_id(), 0);
    for (int y = 0; y < size.height; ++y) {
        for (int z = 0; z < size.length; ++z) {
            for (int x = 0; x < size.width; ++x) {
                const auto runtime_id = block_at(chunks.value(), registry, x, y, z);
                if (!palette_indices.contains(runtime_id)) {
                    const auto index = static_cast<std::int32_t>(palette.size());
                    palette_indices.emplace(runtime_id, index);
                    palette.push_back(runtime_id);
                }
            }
        }
    }
    const auto bits = bits_for_palette(palette.size());
    const auto bit_count = static_cast<std::uint64_t>(size.volume()) * bits;
    std::vector<std::int64_t> packed(static_cast<std::size_t>((bit_count + 63) / 64), 0);
    std::size_t block_index = 0;
    for (int y = 0; y < size.height; ++y) {
        for (int z = 0; z < size.length; ++z) {
            for (int x = 0; x < size.width; ++x, ++block_index) {
                const auto runtime_id = block_at(chunks.value(), registry, x, y, z);
                const auto value = static_cast<std::uint64_t>(palette_indices.at(runtime_id));
                const auto start_bit = static_cast<std::uint64_t>(block_index) * bits;
                const auto word = static_cast<std::size_t>(start_bit / 64);
                const auto offset = static_cast<unsigned>(start_bit % 64);
                auto current = static_cast<std::uint64_t>(packed[word]);
                current |= value << offset;
                packed[word] = static_cast<std::int64_t>(current);
                if (offset + bits > 64) {
                    auto next = static_cast<std::uint64_t>(packed[word + 1]);
                    next |= value >> (64 - offset);
                    packed[word + 1] = static_cast<std::int64_t>(next);
                }
            }
        }
    }

    nbt::tag_compound enclosing_size;
    enclosing_size["x"] = nbt::tag_int(size.width);
    enclosing_size["y"] = nbt::tag_int(size.height);
    enclosing_size["z"] = nbt::tag_int(size.length);
    nbt::tag_compound metadata;
    metadata["Name"] = nbt::tag_string("WaterStructure Export");
    metadata["Author"] = nbt::tag_string("WaterStructure");
    metadata["Description"] = nbt::tag_string(
        "(0,0,0) -> (" + std::to_string(size.width - 1) + "," +
        std::to_string(size.height - 1) + "," + std::to_string(size.length - 1) + ")");
    metadata["RegionCount"] = nbt::tag_int(1);
    metadata["TotalVolume"] = nbt::tag_long(size.volume());
    metadata["EnclosingSize"] = std::move(enclosing_size);

    nbt::tag_compound position;
    position["x"] = nbt::tag_int(0);
    position["y"] = nbt::tag_int(0);
    position["z"] = nbt::tag_int(0);
    nbt::tag_compound region_size;
    region_size["x"] = nbt::tag_int(size.width);
    region_size["y"] = nbt::tag_int(size.height);
    region_size["z"] = nbt::tag_int(size.length);
    nbt::tag_list encoded_palette(nbt::tag_type::Compound);
    for (const auto runtime_id : palette) {
        encoded_palette.push_back(nbt::value_initializer(palette_entry(registry, runtime_id)));
    }
    nbt::tag_compound region;
    region["Position"] = std::move(position);
    region["Size"] = std::move(region_size);
    region["BlockStatePalette"] = std::move(encoded_palette);
    region["BlockStates"] = nbt::tag_long_array(std::move(packed));
    region["Entities"] = nbt::tag_list(nbt::tag_type::Compound);
    region["TileEntities"] = nbt::tag_list(nbt::tag_type::Compound);
    nbt::tag_compound regions;
    regions["region"] = std::move(region);

    nbt::tag_compound root;
    root["Version"] = nbt::tag_int(6);
    root["MinecraftDataVersion"] = nbt::tag_int(kJavaDataVersion);
    root["SubVersion"] = nbt::tag_int(1);
    root["Metadata"] = std::move(metadata);
    root["Regions"] = std::move(regions);

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 Litematic: " + output_path.string());
    try {
        zlib::ozlibstream compressed(output, Z_BEST_SPEED, true);
        nbt::io::write_tag("Litematic", root, compressed, endian::big);
        compressed.close();
        output.close();
        if (!output) return Result<void>::failure("写入 Litematic 失败");
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 Litematic 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
