#include "mcstructure.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace water_structure {

namespace {

const nbt::value* find_value(const nbt::tag_compound& compound, const char* key)
{
    if (!compound.has_key(key)) {
        return nullptr;
    }
    return &compound.at(key);
}

std::optional<std::int32_t> int_value(const nbt::value& value)
{
    try {
        switch (value.get_type()) {
        case nbt::tag_type::Byte: return static_cast<std::int32_t>(value.as<nbt::tag_byte>().get());
        case nbt::tag_type::Short: return static_cast<std::int32_t>(value.as<nbt::tag_short>().get());
        case nbt::tag_type::Int: return value.as<nbt::tag_int>().get();
        case nbt::tag_type::Long: return static_cast<std::int32_t>(value.as<nbt::tag_long>().get());
        default: return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> string_value(const nbt::value& value)
{
    try {
        if (value.get_type() != nbt::tag_type::String) {
            return std::nullopt;
        }
        return static_cast<const std::string&>(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<BlockStateProperty> state_property(std::string name, const nbt::value& value)
{
    BlockStateProperty property;
    property.name = std::move(name);
    switch (value.get_type()) {
    case nbt::tag_type::Byte:
        property.type = BlockStateValueType::Byte;
        property.value = std::to_string(value.as<nbt::tag_byte>().get());
        break;
    case nbt::tag_type::Short:
        property.type = BlockStateValueType::Short;
        property.value = std::to_string(value.as<nbt::tag_short>().get());
        break;
    case nbt::tag_type::Int:
        property.type = BlockStateValueType::Int;
        property.value = std::to_string(value.as<nbt::tag_int>().get());
        break;
    case nbt::tag_type::Long:
        property.type = BlockStateValueType::Long;
        property.value = std::to_string(value.as<nbt::tag_long>().get());
        break;
    case nbt::tag_type::String:
        property.type = BlockStateValueType::String;
        property.value = static_cast<const std::string&>(value);
        break;
    default:
        return std::nullopt;
    }
    return property;
}

NbtPayload serialize_compound(const nbt::tag_compound& compound)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", compound, output, endian::little);
    const auto bytes = output.str();
    return NbtPayload(bytes.begin(), bytes.end());
}

} // namespace

void McStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> McStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::failure("无法打开 MCStructure 文件: " + path.string());
    }

    try {
        const auto [root_name, root] = nbt::io::read_compound(input, endian::little);
        if (!root_name.empty()) {
            return Result<void>::failure("MCStructure 根标签名称必须为空");
        }

        const auto* size_value = find_value(*root, "size");
        const auto* structure_value = find_value(*root, "structure");
        if (!size_value || !structure_value || size_value->get_type() != nbt::tag_type::List ||
            structure_value->get_type() != nbt::tag_type::Compound) {
            return Result<void>::failure("MCStructure 缺少 size 或 structure");
        }

        const auto& size_list = size_value->as<nbt::tag_list>();
        if (size_list.size() < 3) {
            return Result<void>::failure("MCStructure size 长度不足");
        }
        mOriginalSize.width = int_value(size_list.at(0)).value_or(0);
        mOriginalSize.height = int_value(size_list.at(1)).value_or(0);
        mOriginalSize.length = int_value(size_list.at(2)).value_or(0);
        if (mOriginalSize.width <= 0 || mOriginalSize.height <= 0 || mOriginalSize.length <= 0) {
            return Result<void>::failure("MCStructure size 无效");
        }
        mSize = mOriginalSize;

        const auto& structure = structure_value->as<nbt::tag_compound>();
        const auto* indices_value = find_value(structure, "block_indices");
        const auto* palette_value = find_value(structure, "palette");
        if (!indices_value || !palette_value || indices_value->get_type() != nbt::tag_type::List ||
            palette_value->get_type() != nbt::tag_type::Compound) {
            return Result<void>::failure("MCStructure 缺少 block_indices 或 palette");
        }

        const auto& indices = indices_value->as<nbt::tag_list>();
        if (indices.size() != 2 || indices.at(0).get_type() != nbt::tag_type::List ||
            indices.at(1).get_type() != nbt::tag_type::List) {
            return Result<void>::failure("MCStructure block_indices 格式无效");
        }
        const auto volume = static_cast<std::size_t>(mOriginalSize.volume());
        auto read_indices = [volume](const nbt::tag_list& layer, std::vector<std::int32_t>& target) -> Result<void> {
            if (layer.size() != volume) {
                return Result<void>::failure("MCStructure block_indices 长度与 size 不一致");
            }
            target.clear();
            target.reserve(layer.size());
            for (const auto& value : layer) {
                const auto index = int_value(value);
                if (!index) {
                    return Result<void>::failure("MCStructure block index 不是整数");
                }
                target.push_back(*index);
            }
            return Result<void>::success();
        };
        auto primary_result = read_indices(indices.at(0).as<nbt::tag_list>(), mPrimaryIndices);
        if (!primary_result) {
            return primary_result;
        }
        auto secondary_result = read_indices(indices.at(1).as<nbt::tag_list>(), mSecondaryIndices);
        if (!secondary_result) {
            return secondary_result;
        }

        const auto* default_value = find_value(palette_value->as<nbt::tag_compound>(), "default");
        if (!default_value || default_value->get_type() != nbt::tag_type::Compound) {
            return Result<void>::failure("MCStructure 缺少默认 palette");
        }
        const auto& palette = default_value->as<nbt::tag_compound>();
        const auto* blocks_value = find_value(palette, "block_palette");
        if (!blocks_value || blocks_value->get_type() != nbt::tag_type::List) {
            return Result<void>::failure("MCStructure 缺少 block_palette");
        }

        const auto& blocks = blocks_value->as<nbt::tag_list>();
        mPalette.clear();
        mPalette.reserve(blocks.size());
        const auto known_unknown = mRegistry.find("minecraft:unknown");
        mUnknownRuntimeId = known_unknown ? *known_unknown :
            mRegistry.register_state(BlockState{ "minecraft:unknown", {}, 0 });
        for (const auto& value : blocks) {
            if (value.get_type() != nbt::tag_type::Compound) {
                mPalette.push_back(mUnknownRuntimeId);
                continue;
            }
            const auto& block = value.as<nbt::tag_compound>();
            const auto* name = find_value(block, "name");
            const auto block_name = name ? string_value(*name).value_or("minecraft:unknown") : "minecraft:unknown";
            BlockState state{ block_name, {}, 0 };
            if (const auto* states = find_value(block, "states");
                states && states->get_type() == nbt::tag_type::Compound) {
                for (const auto& [property_name, property_value] : states->as<nbt::tag_compound>()) {
                    if (auto property = state_property(property_name, property_value)) {
                        state.states.push_back(std::move(*property));
                    }
                }
            }
            if (const auto* version = find_value(block, "version")) {
                state.version = int_value(*version).value_or(0);
            }
            if (state.name == "minecraft:air" && state.states.empty()) {
                mPalette.push_back(mRegistry.air_runtime_id());
            } else {
                // Go's StateToRuntimeID resolves against the current palette and falls
                // back to the block's default state when old properties are unknown.
                const auto runtime_id = mRegistry.find(state.name, state.states)
                    .or_else([&] { return mRegistry.find(state.name); })
                    .value_or(mUnknownRuntimeId);
                mPalette.push_back(runtime_id);
            }
        }

        mBlockEntities.clear();
        if (const auto* position_data = find_value(palette, "block_position_data");
            position_data && position_data->get_type() == nbt::tag_type::Compound) {
            for (const auto& [index_text, position_value] : position_data->as<nbt::tag_compound>()) {
                std::int32_t index = 0;
                const auto parsed = std::from_chars(index_text.data(), index_text.data() + index_text.size(), index);
                if (parsed.ec != std::errc{} || parsed.ptr != index_text.data() + index_text.size() ||
                    position_value.get_type() != nbt::tag_type::Compound) {
                    continue;
                }
                const auto* entity = find_value(position_value.as<nbt::tag_compound>(), "block_entity_data");
                if (!entity || entity->get_type() != nbt::tag_type::Compound) {
                    continue;
                }
                mBlockEntities.emplace(index, serialize_compound(entity->as<nbt::tag_compound>()));
            }
        }

        mNonAirBlocks = 0;
        for (const auto index : mPrimaryIndices) {
            if (index < 0 || static_cast<std::size_t>(index) >= mPalette.size()) {
                continue;
            }
            if (mPalette[static_cast<std::size_t>(index)] != mRegistry.air_runtime_id()) {
                ++mNonAirBlocks;
            }
        }
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 MCStructure 失败: ") + error.what());
    }
}

Result<ChunkMap> McStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, ChunkData{});
    }

    auto write_layer = [&](const std::vector<std::int32_t>& indices, int layer) {
        for (std::size_t index = 0; index < indices.size(); ++index) {
            const auto palette_index = indices[index];
            if (palette_index < 0 || static_cast<std::size_t>(palette_index) >= mPalette.size()) {
                continue;
            }
            const auto runtime_id = mPalette[static_cast<std::size_t>(palette_index)];
            if (runtime_id == mRegistry.air_runtime_id()) {
                continue;
            }
            const int z = static_cast<int>(index % mOriginalSize.length);
            const int y = static_cast<int>((index / mOriginalSize.length) % mOriginalSize.height);
            const int x = static_cast<int>(index /
                (static_cast<std::size_t>(mOriginalSize.height) * mOriginalSize.length));
            const int world_x = x + mOffset.x;
            const int world_y = y + mOffset.y;
            const int world_z = z + mOffset.z;
            const ChunkPos chunk_pos{ floor_div(world_x, 16), floor_div(world_z, 16) };
            const auto chunk_it = result.find(chunk_pos);
            if (chunk_it == result.end()) {
                continue;
            }
            const int sub_y = floor_div(world_y - 64, 16);
            auto [sub_it, inserted] = chunk_it->second.sub_chunks.try_emplace(sub_y);
            if (inserted) {
                sub_it->second.layer0.fill(mRegistry.air_runtime_id());
                sub_it->second.layer1.fill(mRegistry.air_runtime_id());
            }
            const int local_x = world_x - chunk_pos.x * 16;
            const int local_y = world_y - (sub_y * 16 + 64);
            const int local_z = world_z - chunk_pos.z * 16;
            auto& target = layer == 0 ? sub_it->second.layer0 : sub_it->second.layer1;
            target[static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x)] = runtime_id;
        }
    };

    write_layer(mPrimaryIndices, 0);
    write_layer(mSecondaryIndices, 1);
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> McStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, std::vector<BlockEntity>{});
    }
    for (const auto& [flat_index, payload] : mBlockEntities) {
        if (flat_index < 0 || static_cast<std::size_t>(flat_index) >= mPrimaryIndices.size()) {
            continue;
        }
        const auto index = static_cast<std::size_t>(flat_index);
        const int z = static_cast<int>(index % mOriginalSize.length);
        const int y = static_cast<int>((index / mOriginalSize.length) % mOriginalSize.height);
        const int x = static_cast<int>(index /
            (static_cast<std::size_t>(mOriginalSize.height) * mOriginalSize.length));
        const int structure_x = x + mOffset.x;
        const int structure_y = y + mOffset.y;
        const int structure_z = z + mOffset.z;
        const ChunkPos chunk_pos{ floor_div(structure_x, 16), floor_div(structure_z, 16) };
        const auto it = result.find(chunk_pos);
        if (it == result.end()) {
            continue;
        }
        it->second.push_back({
            {
                structure_x - chunk_pos.x * 16,
                structure_y_to_chunk_local(structure_y),
                structure_z - chunk_pos.z * 16
            },
            payload
        });
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> McStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> McStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("MCStructure 导出尚未迁移");
}

} // namespace water_structure
