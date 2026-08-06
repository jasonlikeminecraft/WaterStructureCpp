#include "schematic.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <io/izlibstream.h>
#include <io/stream_reader.h>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_primitive.h>

#include <cmath>
#include <fstream>

namespace water_structure {

namespace {

const nbt::value* find_value(const nbt::tag_compound& compound, const char* key)
{
    return compound.has_key(key) ? &compound.at(key) : nullptr;
}

std::optional<std::int32_t> int_value(const nbt::value* value)
{
    if (!value) return std::nullopt;
    switch (value->get_type()) {
    case nbt::tag_type::Byte: return static_cast<std::int32_t>(value->as<nbt::tag_byte>().get());
    case nbt::tag_type::Short: return static_cast<std::int32_t>(value->as<nbt::tag_short>().get());
    case nbt::tag_type::Int: return value->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return static_cast<std::int32_t>(value->as<nbt::tag_long>().get());
    default: return std::nullopt;
    }
}

} // namespace

void SchematicStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> SchematicStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::failure("无法打开 Schematic 文件: " + path.string());
    }
    try {
        zlib::izlibstream decompressed(input);
        const auto [root_name, root] = nbt::io::read_compound(decompressed, endian::big);
        if (root_name != "Schematic") {
            return Result<void>::failure("Schematic 根标签名称不是 Schematic");
        }
        mOriginalSize = {
            int_value(find_value(*root, "Width")).value_or(0),
            int_value(find_value(*root, "Height")).value_or(0),
            int_value(find_value(*root, "Length")).value_or(0)
        };
        if (mOriginalSize.width <= 0 || mOriginalSize.height <= 0 || mOriginalSize.length <= 0) {
            return Result<void>::failure("Schematic 尺寸无效");
        }
        const auto* blocks = find_value(*root, "Blocks");
        const auto* data = find_value(*root, "Data");
        if (!blocks || !data || blocks->get_type() != nbt::tag_type::Byte_Array ||
            data->get_type() != nbt::tag_type::Byte_Array) {
            return Result<void>::failure("Schematic 缺少 Blocks 或 Data 字节数组");
        }
        mBlocks = blocks->as<nbt::tag_byte_array>().get();
        mData = data->as<nbt::tag_byte_array>().get();
        const auto volume = static_cast<std::size_t>(mOriginalSize.volume());
        if (mBlocks.size() != volume || mData.size() != volume) {
            return Result<void>::failure("Schematic Blocks/Data 长度与尺寸不一致");
        }
        if (!mRegistry.schematic_runtime_id(0, 0)) {
            return Result<void>::failure("尚未加载 assets/block_mappings_v1.json 中的 Schematic 映射");
        }
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 Schematic 失败: ") + error.what());
    }
}

Result<ChunkMap> SchematicStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, ChunkData{});
    }
    for (std::size_t index = 0; index < mBlocks.size(); ++index) {
        const auto block_id = static_cast<std::uint8_t>(mBlocks[index]);
        const auto data = static_cast<std::uint8_t>(mData[index]);
        const auto runtime_id = mRegistry.schematic_runtime_id(block_id, data);
        if (!runtime_id) {
            return Result<ChunkMap>::failure(
                "Schematic 方块映射缺失: id=" + std::to_string(block_id) + ", data=" + std::to_string(data));
        }
        if (*runtime_id == mRegistry.air_runtime_id()) {
            continue;
        }

        const int x = static_cast<int>(index % mOriginalSize.width);
        const int z = static_cast<int>((index / mOriginalSize.width) % mOriginalSize.length);
        const int y = static_cast<int>(index /
            (static_cast<std::size_t>(mOriginalSize.width) * mOriginalSize.length));
        const int structure_x = x + mOffset.x;
        const int structure_y = y + mOffset.y;
        const int structure_z = z + mOffset.z;
        const ChunkPos chunk_pos{ floor_div(structure_x, 16), floor_div(structure_z, 16) };
        const auto chunk_it = result.find(chunk_pos);
        if (chunk_it == result.end()) {
            continue;
        }
        const int sub_y = floor_div(structure_y - 64, 16);
        auto [sub_it, inserted] = chunk_it->second.sub_chunks.try_emplace(sub_y);
        if (inserted) {
            sub_it->second.layer0.fill(mRegistry.air_runtime_id());
            sub_it->second.layer1.fill(mRegistry.air_runtime_id());
        }
        const int local_x = structure_x - chunk_pos.x * 16;
        const int local_y = structure_y - (sub_y * 16 + 64);
        const int local_z = structure_z - chunk_pos.z * 16;
        sub_it->second.layer0[static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x)] = *runtime_id;
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> SchematicStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, std::vector<BlockEntity>{});
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<std::size_t> SchematicStructure::count_non_air_blocks() const
{
    std::size_t count = 0;
    for (const auto block : mBlocks) {
        if (static_cast<std::uint8_t>(block) != 0) {
            ++count;
        }
    }
    return Result<std::size_t>::success(count);
}

Result<void> SchematicStructure::write_to_world(
    WorldTarget& world,
    SubChunkPos start,
    ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> SchematicStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("Schematic 导出尚未迁移");
}

} // namespace water_structure
