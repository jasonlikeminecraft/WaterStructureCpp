#include "litematic.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <io/izlibstream.h>
#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace water_structure {

namespace {

const nbt::value* find_value(const nbt::tag_compound& compound, const char* key)
{
    return compound.has_key(key) ? &compound.at(key) : nullptr;
}

std::optional<std::int32_t> int_value(const nbt::tag_compound& compound, const char* key)
{
    const auto* value = find_value(compound, key);
    if (!value) return std::nullopt;
    switch (value->get_type()) {
    case nbt::tag_type::Byte: return static_cast<std::int32_t>(value->as<nbt::tag_byte>().get());
    case nbt::tag_type::Short: return static_cast<std::int32_t>(value->as<nbt::tag_short>().get());
    case nbt::tag_type::Int: return value->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return static_cast<std::int32_t>(value->as<nbt::tag_long>().get());
    default: return std::nullopt;
    }
}

std::optional<std::string> string_value(const nbt::value* value)
{
    if (!value || value->get_type() != nbt::tag_type::String) return std::nullopt;
    return static_cast<const std::string&>(*value);
}

std::optional<std::string> property_value(const nbt::value& value)
{
    switch (value.get_type()) {
    case nbt::tag_type::Byte:
        return value.as<nbt::tag_byte>().get() == 0 ? "false" : "true";
    case nbt::tag_type::Short:
        return std::to_string(value.as<nbt::tag_short>().get());
    case nbt::tag_type::Int:
        return std::to_string(value.as<nbt::tag_int>().get());
    case nbt::tag_type::Long:
        return std::to_string(value.as<nbt::tag_long>().get());
    case nbt::tag_type::String:
        return value.as<nbt::tag_string>().get();
    default:
        return std::nullopt;
    }
}

NbtPayload serialize_compound(const nbt::tag_compound& compound)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", compound, output, endian::little);
    const auto bytes = output.str();
    return NbtPayload(bytes.begin(), bytes.end());
}

std::uint8_t bits_for_palette(std::size_t size)
{
    std::uint8_t bits = 0;
    auto value = size > 0 ? size - 1 : 0;
    while (value != 0) {
        ++bits;
        value >>= 1u;
    }
    return std::max<std::uint8_t>(2, bits);
}

} // namespace

void LitematicStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

std::uint32_t LitematicStructure::palette_index_at(std::size_t index) const noexcept
{
    const auto start_bit = static_cast<std::uint64_t>(index) * mBitsPerBlock;
    const auto word = static_cast<std::size_t>(start_bit / 64);
    const auto offset = static_cast<unsigned>(start_bit % 64);
    if (word >= mPackedStates.size()) return 0;
    std::uint64_t value = mPackedStates[word] >> offset;
    if (offset + mBitsPerBlock > 64 && word + 1 < mPackedStates.size()) {
        value |= mPackedStates[word + 1] << (64 - offset);
    }
    return static_cast<std::uint32_t>(value & ((std::uint64_t{1} << mBitsPerBlock) - 1));
}

Result<void> LitematicStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 Litematic 文件: " + path.string());
    try {
        zlib::izlibstream decompressed(input);
        const auto [root_name, root] = nbt::io::read_compound(decompressed, endian::big);
        const auto* regions_value = find_value(*root, "Regions");
        if (!regions_value || regions_value->get_type() != nbt::tag_type::Compound) {
            return Result<void>::failure("Litematic 缺少 Regions");
        }
        const auto& regions = regions_value->as<nbt::tag_compound>();
        if (regions.size() == 0 || regions.begin()->second.get_type() != nbt::tag_type::Compound) {
            return Result<void>::failure("Litematic 没有有效 Region");
        }
        const auto& region = regions.begin()->second.as<nbt::tag_compound>();
        const auto* position_value = find_value(region, "Position");
        const auto* size_value = find_value(region, "Size");
        const auto* palette_value = find_value(region, "BlockStatePalette");
        const auto* states_value = find_value(region, "BlockStates");
        if (!position_value || !size_value || !palette_value || !states_value ||
            position_value->get_type() != nbt::tag_type::Compound ||
            size_value->get_type() != nbt::tag_type::Compound ||
            palette_value->get_type() != nbt::tag_type::List ||
            states_value->get_type() != nbt::tag_type::Long_Array) {
            return Result<void>::failure("Litematic Region 缺少 Position/Size/Palette/BlockStates");
        }
        const auto& position = position_value->as<nbt::tag_compound>();
        const auto& size = size_value->as<nbt::tag_compound>();
        mRegionOrigin = {
            int_value(position, "x").value_or(0),
            int_value(position, "y").value_or(0),
            int_value(position, "z").value_or(0)
        };
        mOriginalSize = {
            std::abs(int_value(size, "x").value_or(0)),
            std::abs(int_value(size, "y").value_or(0)),
            std::abs(int_value(size, "z").value_or(0))
        };
        if (mOriginalSize.width <= 0 || mOriginalSize.height <= 0 || mOriginalSize.length <= 0) {
            return Result<void>::failure("Litematic Region 尺寸无效");
        }

        mPalette.clear();
        const auto known_unknown = mRegistry.find("minecraft:unknown");
        const auto unknown = known_unknown ? *known_unknown :
            mRegistry.register_state(BlockState{ "minecraft:unknown", {}, 0 });
        for (const auto& entry : palette_value->as<nbt::tag_list>()) {
            if (entry.get_type() != nbt::tag_type::Compound) {
                mPalette.push_back(unknown);
                continue;
            }
            const auto& state = entry.as<nbt::tag_compound>();
            auto encoded = string_value(find_value(state, "Name")).value_or("minecraft:unknown");
            if (const auto* properties = find_value(state, "Properties");
                properties && properties->get_type() == nbt::tag_type::Compound &&
                properties->as<nbt::tag_compound>().size() != 0) {
                encoded += '[';
                bool first = true;
                for (const auto& [name, value] : properties->as<nbt::tag_compound>()) {
                    const auto property = property_value(value);
                    if (!property) continue;
                    if (!first) encoded += ',';
                    first = false;
                    encoded += name + '=' + *property;
                }
                encoded += ']';
            }
            mPalette.push_back(mRegistry.compatible_java_runtime_id(encoded).value_or(unknown));
        }
        if (mPalette.empty()) return Result<void>::failure("Litematic palette 为空");
        mBitsPerBlock = bits_for_palette(mPalette.size());
        mPackedStates.clear();
        const auto& longs = states_value->as<nbt::tag_long_array>();
        mPackedStates.reserve(longs.size());
        for (const auto value : longs) mPackedStates.push_back(static_cast<std::uint64_t>(value));
        const auto required_bits = static_cast<std::uint64_t>(mOriginalSize.volume()) * mBitsPerBlock;
        if (mPackedStates.size() * 64ull < required_bits) {
            return Result<void>::failure("Litematic BlockStates 长度不足");
        }

        mBlockEntities.clear();
        if (const auto* entities = find_value(region, "TileEntities");
            entities && entities->get_type() == nbt::tag_type::List) {
            for (const auto& entity_value : entities->as<nbt::tag_list>()) {
                if (entity_value.get_type() != nbt::tag_type::Compound) continue;
                const auto& entity = entity_value.as<nbt::tag_compound>();
                const auto x = int_value(entity, "x");
                const auto y = int_value(entity, "y");
                const auto z = int_value(entity, "z");
                if (!x || !y || !z) continue;
                mBlockEntities.push_back({ { *x, *y, *z }, serialize_compound(entity) });
            }
        }

        mNonAirBlocks = 0;
        const auto volume = static_cast<std::size_t>(mOriginalSize.volume());
        for (std::size_t i = 0; i < volume; ++i) {
            const auto palette_index = palette_index_at(i);
            if (palette_index < mPalette.size() && mPalette[palette_index] != mRegistry.air_runtime_id()) {
                ++mNonAirBlocks;
            }
        }
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 Litematic 失败: ") + error.what());
    }
}

Result<ChunkMap> LitematicStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    const auto volume = static_cast<std::size_t>(mOriginalSize.volume());
    const auto layer_size = static_cast<std::size_t>(mOriginalSize.width) * mOriginalSize.length;
    for (std::size_t index = 0; index < volume; ++index) {
        const auto palette_index = palette_index_at(index);
        if (palette_index >= mPalette.size()) continue;
        const auto runtime_id = mPalette[palette_index];
        if (runtime_id == mRegistry.air_runtime_id()) continue;
        const int y = static_cast<int>(index / layer_size);
        const auto remaining = index % layer_size;
        const int z = static_cast<int>(remaining / mOriginalSize.width);
        const int x = static_cast<int>(remaining % mOriginalSize.width);
        const int structure_x = x + mOffset.x;
        const int structure_y = y + mOffset.y;
        const int structure_z = z + mOffset.z;
        const ChunkPos chunk_pos{ floor_div(structure_x, 16), floor_div(structure_z, 16) };
        const auto chunk_it = result.find(chunk_pos);
        if (chunk_it == result.end()) continue;
        const int sub_y = floor_div(structure_y - 64, 16);
        auto [sub_it, inserted] = chunk_it->second.sub_chunks.try_emplace(sub_y);
        if (inserted) {
            sub_it->second.layer0.fill(mRegistry.air_runtime_id());
            sub_it->second.layer1.fill(mRegistry.air_runtime_id());
        }
        const int local_x = structure_x - chunk_pos.x * 16;
        const int local_y = structure_y - (sub_y * 16 + 64);
        const int local_z = structure_z - chunk_pos.z * 16;
        sub_it->second.layer0[static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x)] = runtime_id;
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> LitematicStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    for (const auto& source : mBlockEntities) {
        const auto x = source.pos.x + mOffset.x;
        const auto y = source.pos.y + mOffset.y;
        const auto z = source.pos.z + mOffset.z;
        const ChunkPos chunk_pos{ floor_div(x, 16), floor_div(z, 16) };
        const auto it = result.find(chunk_pos);
        if (it == result.end()) continue;
        it->second.push_back({
            { x - chunk_pos.x * 16, structure_y_to_chunk_local(y), z - chunk_pos.z * 16 },
            source.payload
        });
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<std::size_t> LitematicStructure::count_non_air_blocks() const
{
    return Result<std::size_t>::success(mNonAirBlocks);
}

Result<void> LitematicStructure::write_to_world(
    WorldTarget& world,
    SubChunkPos start,
    ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> LitematicStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("Litematic 导出尚未迁移");
}

} // namespace water_structure
