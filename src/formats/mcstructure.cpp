#include "mcstructure.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

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

using NbtReader = nbt::io::stream_reader;

void skip_bytes(NbtReader& reader, std::int32_t count, std::size_t width)
{
    if (count < 0 || static_cast<std::uint64_t>(count) >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) / width) {
        throw nbt::io::input_error("Invalid MCStructure NBT payload length");
    }
    auto& input = reader.get_istr();
    input.ignore(static_cast<std::streamsize>(count) * static_cast<std::streamsize>(width));
    if (!input) throw nbt::io::input_error("Unexpected end of MCStructure NBT payload");
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
        if (count < 0) throw nbt::io::input_error("Invalid MCStructure list length");
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
        throw nbt::io::input_error("Invalid MCStructure NBT payload type");
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
        throw nbt::io::input_error("MCStructure NBT value is not an integer");
    }
}

} // namespace

void McStructure::BlockIndexArray::clear() noexcept
{
    std::visit([](auto& values) { values.clear(); }, mValues);
}

void McStructure::BlockIndexArray::reserve(std::size_t count)
{
    std::visit([count](auto& values) { values.reserve(count); }, mValues);
}

void McStructure::BlockIndexArray::push_back(std::int32_t value)
{
    if (auto* compact = std::get_if<CompactValues>(&mValues)) {
        if (value >= -1 && value <= 65534) {
            compact->push_back(value < 0 ? 65535u : static_cast<std::uint16_t>(value));
            return;
        }
        FullValues expanded;
        expanded.reserve(compact->size() + 1);
        for (const auto encoded : *compact) {
            expanded.push_back(encoded == 65535u ? -1 : static_cast<std::int32_t>(encoded));
        }
        expanded.push_back(value);
        mValues = std::move(expanded);
        return;
    }
    std::get<FullValues>(mValues).push_back(value);
}

std::size_t McStructure::BlockIndexArray::size() const noexcept
{
    return std::visit([](const auto& values) { return values.size(); }, mValues);
}

std::int32_t McStructure::BlockIndexArray::at(std::size_t index) const noexcept
{
    if (const auto* compact = std::get_if<CompactValues>(&mValues)) {
        const auto encoded = (*compact)[index];
        return encoded == 65535u ? -1 : static_cast<std::int32_t>(encoded);
    }
    return std::get<FullValues>(mValues)[index];
}

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

    mPrimaryIndices.clear();
    mSecondaryIndices.clear();
    mPalette.clear();
    mBlockEntities.clear();
    mNonAirBlocks = 0;

    try {
        NbtReader reader(input, endian::little);
        if (reader.read_type() != nbt::tag_type::Compound || !reader.read_string().empty()) {
            return Result<void>::failure("MCStructure 根标签名称必须为空");
        }

        bool has_size = false;
        bool has_structure = false;
        bool has_indices = false;
        bool has_palette = false;
        bool has_default_palette = false;
        bool has_block_palette = false;
        const auto known_unknown = mRegistry.find("minecraft:unknown");
        mUnknownRuntimeId = known_unknown ? *known_unknown :
            mRegistry.register_state(BlockState{ "minecraft:unknown", {}, 0 });

        auto read_size = [&]() -> Result<void> {
            const auto element_type = reader.read_type(true);
            std::int32_t count = 0;
            reader.read_num(count);
            if (count < 3 || count > 1024 || !is_integer_type(element_type)) {
                return Result<void>::failure("MCStructure size 格式无效");
            }
            std::array<std::int32_t, 3> values{};
            for (std::int32_t index = 0; index < count; ++index) {
                const auto value = read_integer(reader, element_type);
                if (index < 3) values[static_cast<std::size_t>(index)] = value;
            }
            mOriginalSize = { values[0], values[1], values[2] };
            if (mOriginalSize.width <= 0 || mOriginalSize.height <= 0 || mOriginalSize.length <= 0) {
                return Result<void>::failure("MCStructure size 无效");
            }
            mSize = mOriginalSize;
            has_size = true;
            return Result<void>::success();
        };
        auto read_indices = [&]() -> Result<void> {
            if (reader.read_type(true) != nbt::tag_type::List) {
                return Result<void>::failure("MCStructure block_indices 格式无效");
            }
            std::int32_t layer_count = 0;
            reader.read_num(layer_count);
            if (layer_count != 2) {
                return Result<void>::failure("MCStructure block_indices 层数无效");
            }
            for (std::int32_t layer = 0; layer < layer_count; ++layer) {
                const auto element_type = reader.read_type(true);
                if (!is_integer_type(element_type)) {
                    return Result<void>::failure("MCStructure block index 不是整数列表");
                }
                std::int32_t count = 0;
                reader.read_num(count);
                if (count < 0) return Result<void>::failure("MCStructure block index 数量无效");
                auto& current = layer == 0 ? mPrimaryIndices : mSecondaryIndices;
                current.clear();
                current.reserve(static_cast<std::size_t>(count));
                for (std::int32_t index = 0; index < count; ++index) {
                    const auto value = read_integer(reader, element_type);
                    current.push_back(value);
                }
            }
            has_indices = true;
            return Result<void>::success();
        };
        auto append_palette_entry = [&](const nbt::tag_compound& block) {
            const auto* name = find_value(block, "name");
            const auto block_name = name ? string_value(*name).value_or("minecraft:unknown") :
                "minecraft:unknown";
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
                return;
            }
            // Go's StateToRuntimeID resolves against the current palette and
            // falls back to the block's default state for old properties.
            mPalette.push_back(mRegistry.find(state.name, state.states)
                .or_else([&] { return mRegistry.find(state.name); })
                .value_or(mUnknownRuntimeId));
        };
        auto read_block_palette = [&]() -> Result<void> {
            const auto element_type = reader.read_type(true);
            std::int32_t count = 0;
            reader.read_num(count);
            if (count < 0) {
                return Result<void>::failure("MCStructure block_palette 数量无效");
            }
            mPalette.clear();
            mPalette.reserve(static_cast<std::size_t>(count));
            for (std::int32_t index = 0; index < count; ++index) {
                if (element_type != nbt::tag_type::Compound) {
                    skip_payload(reader, element_type);
                    mPalette.push_back(mUnknownRuntimeId);
                    continue;
                }
                // Materialize only one palette entry.  This keeps typed NBT
                // conversion semantics without retaining the complete palette
                // compound (and block-position data) in memory.
                const auto entry = reader.read_payload(element_type);
                append_palette_entry(entry->as<nbt::tag_compound>());
            }
            has_block_palette = true;
            return Result<void>::success();
        };
        auto read_position_data = [&]() -> Result<void> {
            // tag_compound is a sorted map.  Different textual keys may still
            // parse to the same numeric block index (for example "1" and
            // "01"); retain the lexicographically first entity just as the
            // former full-DOM iteration did.
            std::unordered_map<std::int32_t, std::string> entity_source_keys;
            while (true) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto index_text = reader.read_string();
                if (type != nbt::tag_type::Compound) {
                    skip_payload(reader, type);
                    continue;
                }
                std::int32_t index = 0;
                const auto parsed = std::from_chars(
                    index_text.data(), index_text.data() + index_text.size(), index);
                const bool valid_index = parsed.ec == std::errc{} &&
                    parsed.ptr == index_text.data() + index_text.size();
                std::optional<NbtPayload> entity_payload;
                while (true) {
                    const auto child_type = reader.read_type(true);
                    if (child_type == nbt::tag_type::End) break;
                    const auto child_key = reader.read_string();
                    if (valid_index && child_key == "block_entity_data" &&
                        child_type == nbt::tag_type::Compound) {
                        const auto entity = reader.read_payload(child_type);
                        entity_payload = serialize_compound(entity->as<nbt::tag_compound>());
                    } else {
                        skip_payload(reader, child_type);
                    }
                }
                if (entity_payload) {
                    const auto source = entity_source_keys.find(index);
                    if (source == entity_source_keys.end()) {
                        entity_source_keys.emplace(index, index_text);
                        mBlockEntities.emplace(index, std::move(*entity_payload));
                    } else if (index_text < source->second) {
                        source->second = index_text;
                        mBlockEntities[index] = std::move(*entity_payload);
                    }
                }
            }
            return Result<void>::success();
        };
        auto read_default_palette = [&]() -> Result<void> {
            has_default_palette = true;
            bool saw_block_palette = false;
            bool saw_position_data = false;
            while (true) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto key = reader.read_string();
                if (key == "block_palette" && !std::exchange(saw_block_palette, true)) {
                    if (type == nbt::tag_type::List) {
                        auto result = read_block_palette();
                        if (!result) return result;
                    } else {
                        skip_payload(reader, type);
                    }
                } else if (key == "block_position_data" &&
                    !std::exchange(saw_position_data, true)) {
                    if (type == nbt::tag_type::Compound) {
                        auto result = read_position_data();
                        if (!result) return result;
                    } else {
                        skip_payload(reader, type);
                    }
                } else {
                    skip_payload(reader, type);
                }
            }
            return Result<void>::success();
        };
        auto read_palette = [&]() -> Result<void> {
            has_palette = true;
            has_default_palette = false;
            has_block_palette = false;
            mPalette.clear();
            mBlockEntities.clear();
            bool saw_default = false;
            while (true) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto key = reader.read_string();
                if (key == "default" && !std::exchange(saw_default, true)) {
                    if (type == nbt::tag_type::Compound) {
                        auto result = read_default_palette();
                        if (!result) return result;
                    } else {
                        skip_payload(reader, type);
                    }
                } else {
                    skip_payload(reader, type);
                }
            }
            return Result<void>::success();
        };
        auto read_structure = [&]() -> Result<void> {
            while (true) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto key = reader.read_string();
                if (key == "block_indices" && type == nbt::tag_type::List) {
                    auto result = read_indices();
                    if (!result) return result;
                } else if (key == "palette" && type == nbt::tag_type::Compound) {
                    auto result = read_palette();
                    if (!result) return result;
                } else {
                    skip_payload(reader, type);
                }
            }
            has_structure = true;
            return Result<void>::success();
        };

        while (true) {
            const auto type = reader.read_type(true);
            if (type == nbt::tag_type::End) break;
            const auto key = reader.read_string();
            if (key == "size" && type == nbt::tag_type::List) {
                auto result = read_size();
                if (!result) return result;
            } else if (key == "structure" && type == nbt::tag_type::Compound) {
                auto result = read_structure();
                if (!result) return result;
            } else {
                skip_payload(reader, type);
            }
        }
        if (!has_size || !has_structure || !has_indices || !has_palette) {
            return Result<void>::failure("MCStructure 缺少 size、structure、block_indices 或 palette");
        }
        const auto volume = static_cast<std::size_t>(mOriginalSize.volume());
        if (mPrimaryIndices.size() != volume || mSecondaryIndices.size() != volume) {
            return Result<void>::failure("MCStructure block_indices 长度与 size 不一致");
        }

        if (!has_default_palette) {
            return Result<void>::failure("MCStructure 缺少默认 palette");
        }
        if (!has_block_palette) {
            return Result<void>::failure("MCStructure 缺少 block_palette");
        }

        mNonAirBlocks = 0;
        for (std::size_t flat_index = 0; flat_index < mPrimaryIndices.size(); ++flat_index) {
            const auto index = mPrimaryIndices.at(flat_index);
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
    return get_chunks_impl(positions, true);
}

Result<ChunkMap> McStructure::get_chunks_layer0(std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, false);
}

Result<ChunkMap> McStructure::get_chunks_impl(
    std::span<const ChunkPos> positions,
    bool include_layer1) const
{
    ChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, ChunkData{});
    }

    const auto width = mOriginalSize.width;
    const auto height = mOriginalSize.height;
    const auto length = mOriginalSize.length;
    const auto plane_size = static_cast<std::size_t>(height) * length;
    const auto air_runtime_id = mRegistry.air_runtime_id();
    auto write_layer = [&](const BlockIndexArray& indices, int layer) {
        if (indices.size() == 0) return;
        for (auto& [chunk_pos, chunk] : result) {
            const auto chunk_min_x = static_cast<std::int64_t>(chunk_pos.x) * 16;
            const auto chunk_min_z = static_cast<std::int64_t>(chunk_pos.z) * 16;
            const auto min_x = std::max<std::int64_t>(0, chunk_min_x - mOffset.x);
            const auto max_x = std::min<std::int64_t>(width - 1, chunk_min_x + 15 - mOffset.x);
            const auto min_z = std::max<std::int64_t>(0, chunk_min_z - mOffset.z);
            const auto max_z = std::min<std::int64_t>(length - 1, chunk_min_z + 15 - mOffset.z);
            if (min_x > max_x || min_z > max_z) continue;
            for (int x = static_cast<int>(min_x); x <= static_cast<int>(max_x); ++x) {
                for (int y = 0; y < height; ++y) {
                    for (int z = static_cast<int>(min_z); z <= static_cast<int>(max_z); ++z) {
                        const auto index = static_cast<std::size_t>(x) * plane_size +
                            static_cast<std::size_t>(y) * length + z;
                        if (index >= indices.size()) continue;
                        const auto palette_index = indices.at(index);
                        if (palette_index < 0 || static_cast<std::size_t>(palette_index) >= mPalette.size()) continue;
                        const auto runtime_id = mPalette[static_cast<std::size_t>(palette_index)];
                        if (runtime_id == air_runtime_id) continue;
                        const int world_x = x + mOffset.x;
                        const int world_y = y + mOffset.y;
                        const int world_z = z + mOffset.z;
                        const int sub_y = floor_div(world_y - 64, 16);
                        auto [sub_it, inserted] = chunk.sub_chunks.try_emplace(sub_y);
                        if (inserted) {
                            sub_it->second.layer0.fill(air_runtime_id);
                            if (include_layer1) sub_it->second.layer1.fill(air_runtime_id);
                        }
                        const int local_x = world_x - static_cast<int>(chunk_min_x);
                        const int local_y = world_y - (sub_y * 16 + 64);
                        const int local_z = world_z - static_cast<int>(chunk_min_z);
                        auto& target = layer == 0 ? sub_it->second.layer0 : sub_it->second.layer1;
                        target[static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x)] = runtime_id;
                    }
                }
            }
        }
    };

    write_layer(mPrimaryIndices, 0);
    if (include_layer1) write_layer(mSecondaryIndices, 1);
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
