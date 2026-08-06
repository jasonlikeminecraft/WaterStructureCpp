#include "schem.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <io/izlibstream.h>
#include <io/stream_reader.h>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_primitive.h>

#include <algorithm>
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

struct PackedIndices {
    std::vector<std::uint64_t> words;
    std::size_t count = 0;
    std::uint8_t bits = 1;
};

class PackedIndexCursor {
public:
    PackedIndexCursor(
        std::span<const std::uint64_t> words,
        std::uint8_t bits,
        std::size_t index) noexcept
        : mWords(words),
          mBits(bits),
          mMask((std::uint64_t{1} << bits) - 1)
    {
        const auto bit_position = static_cast<std::uint64_t>(index) * bits;
        mWordIndex = static_cast<std::size_t>(bit_position / 64);
        mBitOffset = static_cast<unsigned>(bit_position % 64);
    }

    std::uint32_t next() noexcept
    {
        std::uint64_t value = mWords[mWordIndex] >> mBitOffset;
        if (mBitOffset + mBits > 64) {
            value |= mWords[mWordIndex + 1] << (64 - mBitOffset);
        }
        mBitOffset += mBits;
        if (mBitOffset >= 64) {
            mBitOffset -= 64;
            ++mWordIndex;
        }
        return static_cast<std::uint32_t>(value & mMask);
    }

private:
    std::span<const std::uint64_t> mWords;
    std::size_t mWordIndex = 0;
    std::uint64_t mMask = 0;
    unsigned mBitOffset = 0;
    unsigned mBits = 1;
};

constexpr std::uint32_t kMaxDensePaletteIndex = (1u << 20) - 1;

std::uint8_t bits_required(std::uint32_t value)
{
    std::uint8_t bits = 1;
    while ((value >>= 1u) != 0) ++bits;
    return bits;
}

Result<PackedIndices> decode_varints(
    const nbt::tag_byte_array& encoded,
    std::size_t expected_count,
    std::uint32_t max_palette_index)
{
    PackedIndices result;
    result.count = expected_count;
    result.bits = bits_required(max_palette_index);
    const auto total_bits = static_cast<std::uint64_t>(expected_count) * result.bits;
    result.words.resize(static_cast<std::size_t>((total_bits + 63) / 64), 0);
    std::size_t count = 0;
    std::uint64_t value = 0;
    int shift = 0;
    for (const auto signed_byte : encoded) {
        const auto byte = static_cast<std::uint8_t>(signed_byte);
        value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            if (value > UINT32_MAX) {
                return Result<PackedIndices>::failure("Schem varint 超出 uint32 范围");
            }
            if (value > max_palette_index) {
                return Result<PackedIndices>::failure("Schem BlockData 引用了 palette 范围外的索引");
            }
            if (count >= expected_count) {
                return Result<PackedIndices>::failure("Schem BlockData 方块数超过 size");
            }
            const auto bit_position = static_cast<std::uint64_t>(count) * result.bits;
            const auto word_index = static_cast<std::size_t>(bit_position / 64);
            const auto bit_offset = static_cast<unsigned>(bit_position % 64);
            result.words[word_index] |= value << bit_offset;
            if (bit_offset + result.bits > 64) {
                result.words[word_index + 1] |= value >> (64 - bit_offset);
            }
            ++count;
            value = 0;
            shift = 0;
        } else {
            shift += 7;
            if (shift >= 35) {
                return Result<PackedIndices>::failure("Schem varint 过长");
            }
        }
    }
    if (shift != 0) {
        return Result<PackedIndices>::failure("Schem BlockData 以截断 varint 结束");
    }
    if (count != expected_count) {
        return Result<PackedIndices>::failure("Schem BlockData 方块数与 size 不一致");
    }
    return Result<PackedIndices>::success(std::move(result));
}

} // namespace

void SchemStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> SchemStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::failure("无法打开 Schem 文件: " + path.string());
    }
    try {
        zlib::izlibstream decompressed(input);
        auto [root_name, root] = nbt::io::read_compound(decompressed, endian::big);
        const nbt::tag_compound* document = root.get();
        if (root_name.empty()) {
            if (const auto* nested = find_value(*root, "Schematic");
                nested && nested->get_type() == nbt::tag_type::Compound) {
                document = &nested->as<nbt::tag_compound>();
                root_name = "Schematic";
            }
        }

        mOriginalSize = {
            int_value(find_value(*document, "Width")).value_or(0),
            int_value(find_value(*document, "Height")).value_or(0),
            int_value(find_value(*document, "Length")).value_or(0)
        };
        if (mOriginalSize.width <= 0 || mOriginalSize.height <= 0 || mOriginalSize.length <= 0) {
            return Result<void>::failure("Schem 尺寸无效");
        }

        const nbt::tag_compound* palette = nullptr;
        const nbt::tag_byte_array* data = nullptr;
        if (mFormat == StructureId::SchemV1) {
            const auto* palette_value = find_value(*document, "Palette");
            const auto* data_value = find_value(*document, "BlockData");
            if (!palette_value || !data_value || palette_value->get_type() != nbt::tag_type::Compound ||
                data_value->get_type() != nbt::tag_type::Byte_Array) {
                return Result<void>::failure("不是 SchemV1: 缺少 Palette/BlockData");
            }
            palette = &palette_value->as<nbt::tag_compound>();
            data = &data_value->as<nbt::tag_byte_array>();
        } else {
            const auto* blocks_value = find_value(*document, "Blocks");
            if (!blocks_value || blocks_value->get_type() != nbt::tag_type::Compound) {
                return Result<void>::failure("不是 SchemV2: 缺少 Blocks");
            }
            const auto& blocks = blocks_value->as<nbt::tag_compound>();
            const auto* palette_value = find_value(blocks, "Palette");
            const auto* data_value = find_value(blocks, "Data");
            if (!palette_value || !data_value || palette_value->get_type() != nbt::tag_type::Compound ||
                data_value->get_type() != nbt::tag_type::Byte_Array) {
                return Result<void>::failure("不是 SchemV2: Blocks 缺少 Palette/Data");
            }
            palette = &palette_value->as<nbt::tag_compound>();
            data = &data_value->as<nbt::tag_byte_array>();
        }

        const auto known_unknown = mRegistry.find("minecraft:unknown");
        mUnknownRuntimeId = known_unknown ? *known_unknown :
            mRegistry.register_state(BlockState{ "minecraft:unknown", {}, 0 });
        mPalette.clear();
        mDensePalette.clear();
        std::uint32_t max_palette_index = 0;
        for (const auto& [java_state, palette_value] : *palette) {
            const auto index = int_value(&palette_value);
            if (!index || *index < 0) {
                return Result<void>::failure("Schem palette 索引不是非负整数");
            }
            mPalette[static_cast<std::uint32_t>(*index)] =
                mRegistry.compatible_java_runtime_id(java_state).value_or(mUnknownRuntimeId);
            max_palette_index = std::max(max_palette_index, static_cast<std::uint32_t>(*index));
        }
        if (max_palette_index <= kMaxDensePaletteIndex) {
            mDensePalette.assign(
                static_cast<std::size_t>(max_palette_index) + 1,
                mUnknownRuntimeId);
            for (const auto& [index, runtime_id] : mPalette) {
                mDensePalette[index] = runtime_id;
            }
        }
        auto decoded = decode_varints(
            *data,
            static_cast<std::size_t>(mOriginalSize.volume()),
            max_palette_index);
        if (!decoded) {
            return Result<void>::failure(decoded.error());
        }
        auto packed = std::move(decoded).value();
        mPackedIndices = std::move(packed.words);
        mBlockCount = packed.count;
        mBitsPerIndex = packed.bits;
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 ") + std::string(name()) + " 失败: " + error.what());
    }
}

Result<ChunkMap> SchemStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, ChunkData{});
    }

    const auto width = mOriginalSize.width;
    const auto height = mOriginalSize.height;
    const auto length = mOriginalSize.length;
    const auto layer_stride = static_cast<std::size_t>(width) * length;
    const auto air_runtime_id = mRegistry.air_runtime_id();
    const auto* dense_palette = mDensePalette.empty() ? nullptr : mDensePalette.data();
    const auto runtime_id_for = [this, dense_palette](std::uint32_t palette_index) noexcept {
        if (dense_palette) return dense_palette[palette_index];
        const auto found = mPalette.find(palette_index);
        return found == mPalette.end() ? mUnknownRuntimeId : found->second;
    };

    // Materialize only the source X/Z range covered by each requested chunk.
    // Iterating the deduplicated result map preserves the previous behaviour for
    // repeated positions while avoiding a full structure scan for every batch.
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
        if (source_min_x > source_max_x || source_min_z > source_max_z) {
            continue;
        }

        const auto min_x = static_cast<int>(source_min_x);
        const auto max_x = static_cast<int>(source_max_x);
        const auto min_z = static_cast<int>(source_min_z);
        const auto max_z = static_cast<int>(source_max_z);
        for (int y = 0; y < height; ++y) {
            const int structure_y = y + mOffset.y;
            const int sub_y = floor_div(structure_y - 64, 16);
            const int local_y = structure_y - (sub_y * 16 + 64);
            SubChunkData* sub_chunk = nullptr;
            for (int z = min_z; z <= max_z; ++z) {
                const auto local_z = static_cast<int>(
                    static_cast<std::int64_t>(z) + mOffset.z - chunk_min_z);
                const auto row_start = static_cast<std::size_t>(y) * layer_stride +
                    static_cast<std::size_t>(z) * width;
                PackedIndexCursor indices(
                    mPackedIndices,
                    mBitsPerIndex,
                    row_start + static_cast<std::size_t>(min_x));
                for (int x = min_x; x <= max_x; ++x) {
                    const auto runtime_id = runtime_id_for(indices.next());
                    if (runtime_id == air_runtime_id) {
                        continue;
                    }
                    if (!sub_chunk) {
                        auto [sub_it, inserted] = chunk.sub_chunks.try_emplace(sub_y);
                        if (inserted) {
                            sub_it->second.layer0.fill(air_runtime_id);
                            sub_it->second.layer1.fill(air_runtime_id);
                        }
                        sub_chunk = &sub_it->second;
                    }
                    const auto local_x = static_cast<int>(
                        static_cast<std::int64_t>(x) + mOffset.x - chunk_min_x);
                    sub_chunk->layer0[static_cast<std::size_t>(
                        (local_y * 16 + local_z) * 16 + local_x)] = runtime_id;
                }
            }
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> SchemStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<std::size_t> SchemStructure::count_non_air_blocks() const
{
    std::size_t result = 0;
    PackedIndexCursor indices(mPackedIndices, mBitsPerIndex, 0);
    const auto* dense_palette = mDensePalette.empty() ? nullptr : mDensePalette.data();
    const auto air_runtime_id = mRegistry.air_runtime_id();
    for (std::size_t position = 0; position < mBlockCount; ++position) {
        const auto palette_index = indices.next();
        if (dense_palette) {
            if (dense_palette[palette_index] != air_runtime_id) ++result;
            continue;
        }
        const auto it = mPalette.find(palette_index);
        if (it == mPalette.end() || it->second != air_runtime_id) ++result;
    }
    return Result<std::size_t>::success(result);
}

Result<void> SchemStructure::write_to_world(
    WorldTarget& world,
    SubChunkPos start,
    ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> SchemStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(std::string(name()) + " 导出尚未迁移");
}

} // namespace water_structure
