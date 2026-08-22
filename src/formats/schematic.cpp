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
#include <limits>
#include <utility>

namespace water_structure {

namespace {

using NbtReader = nbt::io::stream_reader;

void read_bytes(NbtReader& reader, std::int32_t count, std::int8_t* output = nullptr)
{
    if (count < 0) {
        throw nbt::io::input_error("Invalid Schematic NBT payload length");
    }
    auto& input = reader.get_istr();
    constexpr std::streamsize buffer_size = 64 * 1024;
    auto remaining = static_cast<std::streamsize>(count);
    while (remaining > 0) {
        const auto current = std::min(remaining, buffer_size);
        if (output) {
            input.read(reinterpret_cast<char*>(output), current);
            output += current;
        } else {
            input.ignore(current);
        }
        if (!input) throw nbt::io::input_error("Unexpected end of Schematic NBT payload");
        remaining -= current;
    }
}

void skip_bytes(NbtReader& reader, std::int32_t count, std::size_t width)
{
    if (count < 0 || static_cast<std::uint64_t>(count) >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) / width) {
        throw nbt::io::input_error("Invalid Schematic NBT payload length");
    }
    auto bytes = static_cast<std::uint64_t>(count) * width;
    while (bytes != 0) {
        const auto current = static_cast<std::int32_t>(std::min<std::uint64_t>(
            bytes, static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
        read_bytes(reader, current);
        bytes -= static_cast<std::uint32_t>(current);
    }
}

void skip_payload(NbtReader& reader, nbt::tag_type type)
{
    switch (type) {
    case nbt::tag_type::Byte: skip_bytes(reader, 1, 1); break;
    case nbt::tag_type::Short: skip_bytes(reader, 1, 2); break;
    case nbt::tag_type::Int: skip_bytes(reader, 1, 4); break;
    case nbt::tag_type::Long: skip_bytes(reader, 1, 8); break;
    case nbt::tag_type::Float: skip_bytes(reader, 1, 4); break;
    case nbt::tag_type::Double: skip_bytes(reader, 1, 8); break;
    case nbt::tag_type::Byte_Array: {
        std::int32_t count = 0;
        reader.read_num(count);
        skip_bytes(reader, count, 1);
        break;
    }
    case nbt::tag_type::String:
        (void)reader.read_string();
        break;
    case nbt::tag_type::List: {
        const auto element_type = reader.read_type(true);
        std::int32_t count = 0;
        reader.read_num(count);
        if (count < 0) throw nbt::io::input_error("Invalid Schematic list length");
        for (std::int32_t index = 0; index < count; ++index) {
            skip_payload(reader, element_type);
        }
        break;
    }
    case nbt::tag_type::Compound:
        while (true) {
            const auto child_type = reader.read_type(true);
            if (child_type == nbt::tag_type::End) break;
            (void)reader.read_string();
            skip_payload(reader, child_type);
        }
        break;
    case nbt::tag_type::Int_Array: {
        std::int32_t count = 0;
        reader.read_num(count);
        skip_bytes(reader, count, 4);
        break;
    }
    case nbt::tag_type::Long_Array: {
        std::int32_t count = 0;
        reader.read_num(count);
        skip_bytes(reader, count, 8);
        break;
    }
    default:
        throw nbt::io::input_error("Invalid Schematic NBT payload type");
    }
}

bool is_integer_type(nbt::tag_type type) noexcept
{
    return type == nbt::tag_type::Byte || type == nbt::tag_type::Short ||
        type == nbt::tag_type::Int || type == nbt::tag_type::Long;
}

std::int32_t read_integer(NbtReader& reader, nbt::tag_type type)
{
    switch (type) {
    case nbt::tag_type::Byte: {
        std::int8_t value = 0;
        reader.read_num(value);
        return value;
    }
    case nbt::tag_type::Short: {
        std::int16_t value = 0;
        reader.read_num(value);
        return value;
    }
    case nbt::tag_type::Int: {
        std::int32_t value = 0;
        reader.read_num(value);
        return value;
    }
    case nbt::tag_type::Long: {
        std::int64_t value = 0;
        reader.read_num(value);
        return static_cast<std::int32_t>(value);
    }
    default:
        throw nbt::io::input_error("Schematic NBT value is not an integer");
    }
}

void read_byte_array(NbtReader& reader, std::vector<std::int8_t>& output)
{
    std::int32_t count = 0;
    reader.read_num(count);
    if (count < 0) throw nbt::io::input_error("Invalid Schematic byte-array length");
    output.resize(static_cast<std::size_t>(count));
    read_bytes(reader, count, output.data());
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
        NbtReader reader(decompressed, endian::big);
        if (reader.read_type() != nbt::tag_type::Compound || reader.read_string() != "Schematic") {
            return Result<void>::failure("Schematic 根标签名称不是 Schematic");
        }

        mOriginalSize = {};
        mBlocks.clear();
        mData.clear();
        bool saw_width = false;
        bool saw_height = false;
        bool saw_length = false;
        bool saw_blocks = false;
        bool saw_data = false;
        bool has_blocks = false;
        bool has_data = false;
        while (true) {
            const auto type = reader.read_type(true);
            if (type == nbt::tag_type::End) break;
            const auto key = reader.read_string();
            if (key == "Width" && !std::exchange(saw_width, true)) {
                if (is_integer_type(type)) mOriginalSize.width = read_integer(reader, type);
                else skip_payload(reader, type);
            } else if (key == "Height" && !std::exchange(saw_height, true)) {
                if (is_integer_type(type)) mOriginalSize.height = read_integer(reader, type);
                else skip_payload(reader, type);
            } else if (key == "Length" && !std::exchange(saw_length, true)) {
                if (is_integer_type(type)) mOriginalSize.length = read_integer(reader, type);
                else skip_payload(reader, type);
            } else if (key == "Blocks" && !std::exchange(saw_blocks, true)) {
                if (type == nbt::tag_type::Byte_Array) {
                    read_byte_array(reader, mBlocks);
                    has_blocks = true;
                } else {
                    skip_payload(reader, type);
                }
            } else if (key == "Data" && !std::exchange(saw_data, true)) {
                if (type == nbt::tag_type::Byte_Array) {
                    read_byte_array(reader, mData);
                    has_data = true;
                } else {
                    skip_payload(reader, type);
                }
            } else {
                skip_payload(reader, type);
            }
        }
        if (mOriginalSize.width <= 0 || mOriginalSize.height <= 0 || mOriginalSize.length <= 0) {
            return Result<void>::failure("Schematic 尺寸无效");
        }
        if (!has_blocks || !has_data) {
            return Result<void>::failure("Schematic 缺少 Blocks 或 Data 字节数组");
        }
        const auto volume = static_cast<std::size_t>(mOriginalSize.volume());
        if (mBlocks.size() != volume || mData.size() != volume) {
            return Result<void>::failure("Schematic Blocks/Data 长度与尺寸不一致");
        }
        if (!mRegistry.schematic_runtime_id(0, 0)) {
            return Result<void>::failure("尚未加载 assets/block_mappings_v1.json 中的 Schematic 映射");
        }
        constexpr std::size_t legacy_pair_count = 256 * 256;
        mRuntimeCache.assign(legacy_pair_count, 0);
        mRuntimeCacheState.assign(legacy_pair_count, 0);
        for (std::size_t index = 0; index < volume; ++index) {
            const auto block_id = static_cast<std::uint8_t>(mBlocks[index]);
            const auto data = static_cast<std::uint8_t>(mData[index]);
            const auto cache_index = static_cast<std::size_t>(block_id) * 256 + data;
            if (mRuntimeCacheState[cache_index] != 0) continue;
            if (const auto runtime_id = mRegistry.schematic_runtime_id(block_id, data)) {
                mRuntimeCache[cache_index] = *runtime_id;
                mRuntimeCacheState[cache_index] = 1;
            } else {
                mRuntimeCacheState[cache_index] = 2;
            }
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

    const auto runtime_for = [&](std::uint8_t block_id, std::uint8_t data)
        -> Result<std::uint32_t> {
        const auto cache_index = static_cast<std::size_t>(block_id) * 256 + data;
        if (cache_index >= mRuntimeCacheState.size() || mRuntimeCacheState[cache_index] != 1) {
            return Result<std::uint32_t>::failure(
                "Schematic 方块映射缺失: id=" + std::to_string(block_id) +
                ", data=" + std::to_string(data));
        }
        return Result<std::uint32_t>::success(mRuntimeCache[cache_index]);
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
