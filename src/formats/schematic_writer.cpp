#include "schematic_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/ozlibstream.h>
#include <io/stream_writer.h>
#include <tag_primitive.h>
#include <tag_string.h>
#include <zlib.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace water_structure {
namespace {

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

} // namespace

Result<void> write_schematic(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path)
{
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 ||
        size.width > std::numeric_limits<std::int16_t>::max() ||
        size.height > std::numeric_limits<std::int16_t>::max() ||
        size.length > std::numeric_limits<std::int16_t>::max() ||
        size.volume() <= 0 || size.volume() > std::numeric_limits<std::int32_t>::max()) {
        return Result<void>::failure("Schematic 输出尺寸无效、超过 int16 或体积超过 int32");
    }
    std::vector<ChunkPos> positions;
    positions.reserve(static_cast<std::size_t>(size.chunk_x_count()) * size.chunk_z_count());
    for (int x = 0; x < size.chunk_x_count(); ++x) {
        for (int z = 0; z < size.chunk_z_count(); ++z) positions.push_back({ x, z });
    }
    auto chunks = structure.get_chunks(positions);
    if (!chunks) return Result<void>::failure("生成 Schematic chunks 失败: " + chunks.error());

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 Schematic: " + output_path.string());
    try {
        zlib::ozlibstream compressed(output, Z_BEST_SPEED, true);
        nbt::io::stream_writer writer(compressed, endian::big);
        writer.write_type(nbt::tag_type::Compound);
        writer.write_string("Schematic");
        writer.write_tag("Length", nbt::tag_short(static_cast<std::int16_t>(size.length)));
        writer.write_tag("Height", nbt::tag_short(static_cast<std::int16_t>(size.height)));
        writer.write_tag("Width", nbt::tag_short(static_cast<std::int16_t>(size.width)));
        writer.write_tag("Materials", nbt::tag_string("Alpha"));

        const auto volume = static_cast<std::int32_t>(size.volume());
        writer.write_type(nbt::tag_type::Byte_Array);
        writer.write_string("Blocks");
        writer.write_num<std::int32_t>(volume);
        for (int y = 0; y < size.height; ++y) {
            for (int z = 0; z < size.length; ++z) {
                for (int x = 0; x < size.width; ++x) {
                    const auto runtime_id = block_at(chunks.value(), registry, x, y, z);
                    const auto legacy = registry.schematic_block(runtime_id).value_or(
                        std::pair<std::uint8_t, std::uint8_t>{ 0, 0 });
                    compressed.put(static_cast<char>(legacy.first));
                }
            }
        }

        writer.write_type(nbt::tag_type::Byte_Array);
        writer.write_string("Data");
        writer.write_num<std::int32_t>(volume);
        for (int y = 0; y < size.height; ++y) {
            for (int z = 0; z < size.length; ++z) {
                for (int x = 0; x < size.width; ++x) {
                    const auto runtime_id = block_at(chunks.value(), registry, x, y, z);
                    const auto legacy = registry.schematic_block(runtime_id).value_or(
                        std::pair<std::uint8_t, std::uint8_t>{ 0, 0 });
                    compressed.put(static_cast<char>(legacy.second));
                }
            }
        }
        writer.write_type(nbt::tag_type::End);
        compressed.close();
        output.close();
        if (!output) return Result<void>::failure("写入 Schematic 失败");
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 Schematic 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
