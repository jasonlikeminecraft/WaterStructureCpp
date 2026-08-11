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
#include <array>
#include <utility>

namespace water_structure {

namespace {

const nbt::value* find_value(const nbt::tag_compound& compound, const char* key)
{
    return compound.has_key(key) ? &compound.at(key) : nullptr;
}

nbt::value* find_value(nbt::tag_compound& compound, const char* key)
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
        auto* blocks = find_value(*root, "Blocks");
        auto* data = find_value(*root, "Data");
        if (!blocks || !data || blocks->get_type() != nbt::tag_type::Byte_Array ||
            data->get_type() != nbt::tag_type::Byte_Array) {
            return Result<void>::failure("Schematic 缺少 Blocks 或 Data 字节数组");
        }
        // Move the large NBT arrays out of the temporary document instead of
        // keeping a second copy until the root compound is destroyed.
        mBlocks = std::move(blocks->as<nbt::tag_byte_array>().get());
        mData = std::move(data->as<nbt::tag_byte_array>().get());
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
    return get_chunks_impl(positions, true);
}

Result<ChunkMap> SchematicStructure::get_chunks_layer0(std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, false);
}

Result<ChunkMap> SchematicStructure::get_chunks_impl(
    std::span<const ChunkPos> positions,
    bool include_layer1) const
{
    ChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, ChunkData{});
    }

    // Resolve each legacy block/data pair once per batch. The old path also
    // scanned the complete source array for every requested chunk batch.
    std::array<std::uint32_t, 256 * 256> runtime_cache{};
    std::array<bool, 256 * 256> runtime_cached{};
    const auto runtime_for = [&](std::uint8_t block_id, std::uint8_t data)
        -> Result<std::uint32_t> {
        const auto cache_index = static_cast<std::size_t>(block_id) * 256 + data;
        if (runtime_cached[cache_index]) {
            return Result<std::uint32_t>::success(runtime_cache[cache_index]);
        }
        const auto runtime_id = mRegistry.schematic_runtime_id(block_id, data);
        if (!runtime_id) {
            return Result<std::uint32_t>::failure(
                "Schematic 方块映射缺失: id=" + std::to_string(block_id) +
                ", data=" + std::to_string(data));
        }
        runtime_cache[cache_index] = *runtime_id;
        runtime_cached[cache_index] = true;
        return Result<std::uint32_t>::success(*runtime_id);
    };

    const auto width = mOriginalSize.width;
    const auto length = mOriginalSize.length;
    const auto layer_stride = static_cast<std::size_t>(width) * length;
    const auto air_runtime_id = mRegistry.air_runtime_id();
    for (auto& [chunk_pos, chunk] : result) {
        const auto chunk_min_x = static_cast<std::int64_t>(chunk_pos.x) * 16;
        const auto chunk_min_z = static_cast<std::int64_t>(chunk_pos.z) * 16;
        const auto source_min_x = std::max<std::int64_t>(0, chunk_min_x - mOffset.x);
        const auto source_max_x = std::min<std::int64_t>(
            static_cast<std::int64_t>(width) - 1,
            chunk_min_x + 15 - mOffset.x);
        const auto source_min_z = std::max<std::int64_t>(0, chunk_min_z - mOffset.z);
        const auto source_max_z = std::min<std::int64_t>(
            static_cast<std::int64_t>(length) - 1,
            chunk_min_z + 15 - mOffset.z);
        if (source_min_x > source_max_x || source_min_z > source_max_z) continue;

        for (int y = 0; y < mOriginalSize.height; ++y) {
            const int structure_y = y + mOffset.y;
            const int sub_y = floor_div(structure_y - 64, 16);
            const int local_y = structure_y - (sub_y * 16 + 64);
            SubChunkData* sub_chunk = nullptr;
            for (int z = static_cast<int>(source_min_z);
                 z <= static_cast<int>(source_max_z); ++z) {
                const int local_z = static_cast<int>(
                    static_cast<std::int64_t>(z) + mOffset.z - chunk_min_z);
                const auto row_start = static_cast<std::size_t>(y) * layer_stride +
                    static_cast<std::size_t>(z) * width;
                for (int x = static_cast<int>(source_min_x);
                     x <= static_cast<int>(source_max_x); ++x) {
                    const auto runtime_id = runtime_for(
                        static_cast<std::uint8_t>(mBlocks[row_start + x]),
                        static_cast<std::uint8_t>(mData[row_start + x]));
                    if (!runtime_id) return Result<ChunkMap>::failure(runtime_id.error());
                    if (runtime_id.value() == air_runtime_id) continue;
                    if (!sub_chunk) {
                        auto [sub_it, inserted] = chunk.sub_chunks.try_emplace(sub_y);
                        if (inserted) {
                            sub_it->second.layer0.fill(air_runtime_id);
                            if (include_layer1) sub_it->second.layer1.fill(air_runtime_id);
                        }
                        sub_chunk = &sub_it->second;
                    }
                    const int local_x = static_cast<int>(
                        static_cast<std::int64_t>(x) + mOffset.x - chunk_min_x);
                    sub_chunk->layer0[static_cast<std::size_t>(
                        (local_y * 16 + local_z) * 16 + local_x)] = runtime_id.value();
                }
            }
        }
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
