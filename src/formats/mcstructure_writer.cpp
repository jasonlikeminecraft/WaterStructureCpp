#include "mcstructure_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace water_structure {

namespace {

std::uint32_t block_at(
    const ChunkMap& chunks,
    RuntimeRegistry& registry,
    int x,
    int y,
    int z,
    int layer)
{
    const ChunkPos chunk_pos{ floor_div(x, 16), floor_div(z, 16) };
    const auto chunk = chunks.find(chunk_pos);
    if (chunk == chunks.end()) return registry.air_runtime_id();
    const auto sub_y = floor_div(y - 64, 16);
    const auto sub = chunk->second.sub_chunks.find(sub_y);
    if (sub == chunk->second.sub_chunks.end()) return registry.air_runtime_id();
    const auto local_y = y - (sub_y * 16 + 64);
    const auto index = static_cast<std::size_t>((local_y * 16 + floor_mod(z, 16)) * 16 + floor_mod(x, 16));
    return layer == 0 ? sub->second.layer0[index] : sub->second.layer1[index];
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

} // namespace

Result<void> write_mcstructure(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path)
{
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 || size.volume() > INT32_MAX) {
        return Result<void>::failure("MCStructure 输出尺寸无效或体积超过 int32");
    }
    std::vector<ChunkPos> positions;
    for (int x = 0; x < size.chunk_x_count(); ++x) {
        for (int z = 0; z < size.chunk_z_count(); ++z) positions.push_back({ x, z });
    }
    auto chunks = structure.get_chunks(positions);
    if (!chunks) return Result<void>::failure("生成 MCStructure chunks 失败: " + chunks.error());
    auto entities = structure.get_chunk_nbt(positions);
    if (!entities) return Result<void>::failure("生成 MCStructure NBT 失败: " + entities.error());

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 MCStructure: " + output_path.string());
    try {
        nbt::io::stream_writer writer(output, endian::little);
        writer.write_type(nbt::tag_type::Compound);
        writer.write_string("");
        writer.write_tag("format_version", nbt::tag_int(1));
        const nbt::tag_list size_tag{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height),
            static_cast<std::int32_t>(size.length)
        };
        writer.write_tag("size", size_tag);
        const nbt::tag_list origin_tag{ std::int32_t{0}, std::int32_t{0}, std::int32_t{0} };
        writer.write_tag("structure_world_origin", origin_tag);

        writer.write_type(nbt::tag_type::Compound);
        writer.write_string("structure");
        writer.write_type(nbt::tag_type::List);
        writer.write_string("block_indices");
        writer.write_type(nbt::tag_type::List);
        writer.write_num<std::int32_t>(2);

        std::unordered_map<std::uint32_t, std::int32_t> palette_indices;
        std::vector<BlockState> palette;
        auto palette_index = [&](std::uint32_t runtime_id) {
            if (const auto it = palette_indices.find(runtime_id); it != palette_indices.end()) return it->second;
            auto state = registry.state(runtime_id).value_or(
                runtime_id == registry.air_runtime_id()
                    ? BlockState{ "minecraft:air", {}, 0 }
                    : BlockState{ "minecraft:unknown", {}, 0 });
            const auto index = static_cast<std::int32_t>(palette.size());
            palette_indices.emplace(runtime_id, index);
            palette.push_back(std::move(state));
            return index;
        };
        palette_index(registry.air_runtime_id());

        const auto volume = static_cast<std::int32_t>(size.volume());
        for (int layer = 0; layer < 2; ++layer) {
            writer.write_type(nbt::tag_type::Int);
            writer.write_num<std::int32_t>(volume);
            for (int x = 0; x < size.width; ++x) {
                for (int y = 0; y < size.height; ++y) {
                    for (int z = 0; z < size.length; ++z) {
                        const auto runtime_id = block_at(chunks.value(), registry, x, y, z, layer);
                        const auto index = layer == 1 && runtime_id == registry.air_runtime_id()
                            ? -1
                            : palette_index(runtime_id);
                        writer.write_num<std::int32_t>(index);
                    }
                }
            }
        }

        const nbt::tag_list empty_entities(nbt::tag_type::Compound);
        writer.write_tag("entities", empty_entities);

        nbt::tag_list block_palette(nbt::tag_type::Compound);
        for (const auto& state : palette) {
            nbt::tag_compound encoded;
            encoded["name"] = nbt::tag_string(state.name);
            nbt::tag_compound states;
            for (const auto& property : state.states) put_property(states, property);
            encoded["states"] = std::move(states);
            encoded["version"] = state.version;
            block_palette.push_back(std::move(encoded));
        }

        nbt::tag_compound position_data;
        for (const auto& [chunk_pos, values] : entities.value()) {
            for (const auto& entity : values) {
                const auto x = chunk_pos.x * 16 + entity.pos.x;
                const auto y = entity.pos.y - kOverworldMinY;
                const auto z = chunk_pos.z * 16 + entity.pos.z;
                if (x < 0 || x >= size.width || y < 0 || y >= size.height || z < 0 || z >= size.length ||
                    entity.payload.empty()) continue;
                const std::string bytes(entity.payload.begin(), entity.payload.end());
                std::istringstream input(bytes, std::ios::binary);
                auto [name, compound] = nbt::io::read_compound(input, endian::little);
                compound->operator[]("x") = static_cast<std::int32_t>(x);
                compound->operator[]("y") = static_cast<std::int32_t>(y);
                compound->operator[]("z") = static_cast<std::int32_t>(z);
                nbt::tag_compound entry;
                entry["block_entity_data"] = std::move(*compound);
                const auto flat_index = x * size.height * size.length + y * size.length + z;
                position_data[std::to_string(flat_index)] = std::move(entry);
            }
        }
        nbt::tag_compound default_palette;
        default_palette["block_palette"] = std::move(block_palette);
        default_palette["block_position_data"] = std::move(position_data);
        nbt::tag_compound palettes;
        palettes["default"] = std::move(default_palette);
        writer.write_tag("palette", palettes);
        writer.write_type(nbt::tag_type::End);
        writer.write_type(nbt::tag_type::End);
        if (!output) return Result<void>::failure("写入 MCStructure 失败");
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("序列化 MCStructure 失败: ") + error.what());
    }
}

} // namespace water_structure
