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
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

void ensure_input(nbt::io::stream_reader& reader, const char* context)
{
    if (!reader.get_istr()) throw nbt::io::input_error(context);
}

template <typename T>
T read_number(nbt::io::stream_reader& reader, const char* context)
{
    T result{};
    reader.read_num(result);
    ensure_input(reader, context);
    return result;
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

constexpr std::size_t kMaxNbtDepth = 1024;

void check_depth(std::size_t depth)
{
    if (depth > kMaxNbtDepth) throw nbt::io::input_error("Too deeply nested");
}

void skip_bytes(nbt::io::stream_reader& reader, std::uint64_t byte_count)
{
    std::array<char, 64u * 1024u> buffer{};
    while (byte_count != 0) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(byte_count, buffer.size()));
        reader.get_istr().read(buffer.data(), count);
        if (reader.get_istr().gcount() != count) {
            throw nbt::io::input_error("Error skipping NBT payload");
        }
        byte_count -= static_cast<std::uint64_t>(count);
    }
}

std::int32_t read_length(nbt::io::stream_reader& reader, const char* context)
{
    const auto length = read_number<std::int32_t>(reader, context);
    if (length < 0) {
        reader.get_istr().setstate(std::ios::failbit);
        throw nbt::io::input_error(context);
    }
    return length;
}

void skip_payload(
    nbt::io::stream_reader& reader,
    nbt::tag_type type,
    std::size_t depth)
{
    check_depth(depth);
    switch (type) {
    case nbt::tag_type::Byte:
        (void)read_number<std::int8_t>(reader, "Error reading tag_byte");
        return;
    case nbt::tag_type::Short:
        (void)read_number<std::int16_t>(reader, "Error reading tag_short");
        return;
    case nbt::tag_type::Int:
        (void)read_number<std::int32_t>(reader, "Error reading tag_int");
        return;
    case nbt::tag_type::Long:
        (void)read_number<std::int64_t>(reader, "Error reading tag_long");
        return;
    case nbt::tag_type::Float:
        (void)read_number<float>(reader, "Error reading tag_float");
        return;
    case nbt::tag_type::Double:
        (void)read_number<double>(reader, "Error reading tag_double");
        return;
    case nbt::tag_type::Byte_Array: {
        const auto length = read_length(reader, "Error reading length of tag_byte_array");
        skip_bytes(reader, static_cast<std::uint64_t>(length));
        return;
    }
    case nbt::tag_type::String:
        (void)reader.read_string();
        return;
    case nbt::tag_type::List: {
        const auto element_type = reader.read_type(true);
        const auto length = read_length(reader, "Error reading length of tag_list");
        // libnbt treats TAG_End lists as empty even when their encoded length
        // is non-zero. Preserve that compatibility quirk.
        if (element_type == nbt::tag_type::End) return;
        for (std::int32_t index = 0; index < length; ++index) {
            skip_payload(reader, element_type, depth + 1);
        }
        return;
    }
    case nbt::tag_type::Compound:
        while (true) {
            const auto child_type = reader.read_type(true);
            if (child_type == nbt::tag_type::End) return;
            (void)reader.read_string();
            skip_payload(reader, child_type, depth + 1);
        }
    case nbt::tag_type::Int_Array: {
        const auto length = read_length(reader, "Error reading length of tag_int_array");
        skip_bytes(reader, static_cast<std::uint64_t>(length) * sizeof(std::int32_t));
        return;
    }
    case nbt::tag_type::Long_Array: {
        const auto length = read_length(reader, "Error reading length of tag_long_array");
        skip_bytes(reader, static_cast<std::uint64_t>(length) * sizeof(std::int64_t));
        return;
    }
    case nbt::tag_type::End:
    case nbt::tag_type::Null:
        throw nbt::io::input_error("Invalid NBT payload type");
    }
    throw nbt::io::input_error("Unknown NBT payload type");
}

std::optional<std::int32_t> read_int_value(
    nbt::io::stream_reader& reader,
    nbt::tag_type type,
    std::size_t depth)
{
    switch (type) {
    case nbt::tag_type::Byte:
        return static_cast<std::int32_t>(
            read_number<std::int8_t>(reader, "Error reading integer byte"));
    case nbt::tag_type::Short:
        return static_cast<std::int32_t>(
            read_number<std::int16_t>(reader, "Error reading integer short"));
    case nbt::tag_type::Int:
        return read_number<std::int32_t>(reader, "Error reading integer int");
    case nbt::tag_type::Long:
        return static_cast<std::int32_t>(
            read_number<std::int64_t>(reader, "Error reading integer long"));
    default:
        skip_payload(reader, type, depth);
        return std::nullopt;
    }
}

BlockPos read_xyz_compound(nbt::io::stream_reader& reader, std::size_t depth)
{
    check_depth(depth);
    BlockPos result{};
    bool seen_x = false;
    bool seen_y = false;
    bool seen_z = false;
    while (true) {
        const auto type = reader.read_type(true);
        if (type == nbt::tag_type::End) return result;
        const auto key = reader.read_string();
        auto* seen = key == "x" ? &seen_x : key == "y" ? &seen_y : key == "z" ? &seen_z : nullptr;
        if (!seen || *seen) {
            skip_payload(reader, type, depth + 1);
            continue;
        }
        *seen = true;
        const auto value = read_int_value(reader, type, depth + 1).value_or(0);
        if (key == "x") result.x = value;
        else if (key == "y") result.y = value;
        else result.z = value;
    }
}

struct EncodedProperties {
    // A null value records a present but unsupported property. It must still
    // occupy the key so a duplicate supported tag cannot replace it; this is
    // how libnbt's std::map-backed compound behaves.
    std::map<std::string, std::optional<std::string>, std::less<>> values;
};

std::optional<std::string> read_property_value(
    nbt::io::stream_reader& reader,
    nbt::tag_type type,
    std::size_t depth)
{
    switch (type) {
    case nbt::tag_type::Byte:
        return read_number<std::int8_t>(reader, "Error reading property byte") == 0
            ? "false" : "true";
    case nbt::tag_type::Short:
        return std::to_string(
            read_number<std::int16_t>(reader, "Error reading property short"));
    case nbt::tag_type::Int:
        return std::to_string(
            read_number<std::int32_t>(reader, "Error reading property int"));
    case nbt::tag_type::Long:
        return std::to_string(
            read_number<std::int64_t>(reader, "Error reading property long"));
    case nbt::tag_type::String:
        return reader.read_string();
    default:
        skip_payload(reader, type, depth);
        return std::nullopt;
    }
}

EncodedProperties read_properties_compound(
    nbt::io::stream_reader& reader,
    std::size_t depth)
{
    check_depth(depth);
    EncodedProperties result;
    while (true) {
        const auto type = reader.read_type(true);
        if (type == nbt::tag_type::End) return result;
        auto name = reader.read_string();
        if (result.values.contains(name)) {
            skip_payload(reader, type, depth + 1);
            continue;
        }
        auto value = read_property_value(reader, type, depth + 1);
        result.values.emplace(std::move(name), std::move(value));
    }
}

std::string read_palette_entry(nbt::io::stream_reader& reader, std::size_t depth)
{
    check_depth(depth);
    std::string name = "minecraft:unknown";
    bool seen_name = false;
    bool seen_properties = false;
    std::optional<EncodedProperties> properties;
    while (true) {
        const auto type = reader.read_type(true);
        if (type == nbt::tag_type::End) break;
        const auto key = reader.read_string();
        if (key == "Name" && !seen_name) {
            seen_name = true;
            if (type == nbt::tag_type::String) name = reader.read_string();
            else skip_payload(reader, type, depth + 1);
        } else if (key == "Properties" && !seen_properties) {
            seen_properties = true;
            if (type == nbt::tag_type::Compound) {
                properties = read_properties_compound(reader, depth + 1);
            } else {
                skip_payload(reader, type, depth + 1);
            }
        } else {
            skip_payload(reader, type, depth + 1);
        }
    }
    if (properties && !properties->values.empty()) {
        name.push_back('[');
        bool first = true;
        for (const auto& [property_name, value] : properties->values) {
            if (!value) continue;
            if (!first) name.push_back(',');
            first = false;
            name += property_name;
            name.push_back('=');
            name += *value;
        }
        name.push_back(']');
    }
    return name;
}

std::vector<std::string> read_palette_list(
    nbt::io::stream_reader& reader,
    std::size_t depth)
{
    check_depth(depth);
    const auto element_type = reader.read_type(true);
    const auto length = read_length(reader, "Error reading Litematic palette length");
    std::vector<std::string> result;
    if (element_type == nbt::tag_type::End) return result;
    result.reserve(static_cast<std::size_t>(length));
    for (std::int32_t index = 0; index < length; ++index) {
        if (element_type == nbt::tag_type::Compound) {
            result.push_back(read_palette_entry(reader, depth + 1));
        } else {
            skip_payload(reader, element_type, depth + 1);
            result.emplace_back("minecraft:unknown");
        }
    }
    return result;
}

std::vector<std::int64_t> read_long_array(nbt::io::stream_reader& reader)
{
    const auto length = read_length(reader, "Error reading length of tag_long_array");
    std::vector<std::int64_t> result;
    result.reserve(static_cast<std::size_t>(length));
    for (std::int32_t index = 0; index < length; ++index) {
        result.push_back(read_number<std::int64_t>(
            reader, "Error reading contents of tag_long_array"));
    }
    return result;
}

std::vector<BlockEntity> read_tile_entities(
    nbt::io::stream_reader& reader,
    std::size_t depth)
{
    check_depth(depth);
    const auto element_type = reader.read_type(true);
    const auto length = read_length(reader, "Error reading Litematic TileEntities length");
    std::vector<BlockEntity> result;
    if (element_type == nbt::tag_type::End) return result;
    // Do not reserve by the untrusted list length: each retained entity owns
    // an NBT payload and a malicious count should not force a large allocation
    // before bytes have actually been decoded.
    for (std::int32_t index = 0; index < length; ++index) {
        if (element_type != nbt::tag_type::Compound) {
            skip_payload(reader, element_type, depth + 1);
            continue;
        }
        auto tag = reader.read_payload(nbt::tag_type::Compound);
        const auto& entity = tag->as<nbt::tag_compound>();
        const auto x = int_value(entity, "x");
        const auto y = int_value(entity, "y");
        const auto z = int_value(entity, "z");
        if (!x || !y || !z) continue;
        result.push_back({ { *x, *y, *z }, serialize_compound(entity) });
    }
    return result;
}

enum class RequiredFieldState : std::uint8_t {
    Missing,
    WrongType,
    Valid
};

struct StreamedRegion {
    RequiredFieldState position_state = RequiredFieldState::Missing;
    RequiredFieldState size_state = RequiredFieldState::Missing;
    RequiredFieldState palette_state = RequiredFieldState::Missing;
    RequiredFieldState block_states_state = RequiredFieldState::Missing;
    BlockPos position{};
    BlockPos signed_size{};
    std::vector<std::string> palette;
    std::vector<std::int64_t> packed_states;
    std::vector<BlockEntity> block_entities;
};

void read_region_compound(
    nbt::io::stream_reader& reader,
    StreamedRegion& region,
    std::size_t depth)
{
    check_depth(depth);
    bool seen_tile_entities = false;
    while (true) {
        const auto type = reader.read_type(true);
        if (type == nbt::tag_type::End) return;
        const auto key = reader.read_string();
        if (key == "Position" && region.position_state == RequiredFieldState::Missing) {
            if (type == nbt::tag_type::Compound) {
                region.position = read_xyz_compound(reader, depth + 1);
                region.position_state = RequiredFieldState::Valid;
            } else {
                region.position_state = RequiredFieldState::WrongType;
                skip_payload(reader, type, depth + 1);
            }
        } else if (key == "Size" && region.size_state == RequiredFieldState::Missing) {
            if (type == nbt::tag_type::Compound) {
                region.signed_size = read_xyz_compound(reader, depth + 1);
                region.size_state = RequiredFieldState::Valid;
            } else {
                region.size_state = RequiredFieldState::WrongType;
                skip_payload(reader, type, depth + 1);
            }
        } else if (key == "BlockStatePalette" &&
                   region.palette_state == RequiredFieldState::Missing) {
            if (type == nbt::tag_type::List) {
                region.palette = read_palette_list(reader, depth + 1);
                region.palette_state = RequiredFieldState::Valid;
            } else {
                region.palette_state = RequiredFieldState::WrongType;
                skip_payload(reader, type, depth + 1);
            }
        } else if (key == "BlockStates" &&
                   region.block_states_state == RequiredFieldState::Missing) {
            if (type == nbt::tag_type::Long_Array) {
                region.packed_states = read_long_array(reader);
                region.block_states_state = RequiredFieldState::Valid;
            } else {
                region.block_states_state = RequiredFieldState::WrongType;
                skip_payload(reader, type, depth + 1);
            }
        } else if (key == "TileEntities" && !seen_tile_entities) {
            seen_tile_entities = true;
            if (type == nbt::tag_type::List) {
                region.block_entities = read_tile_entities(reader, depth + 1);
            } else {
                skip_payload(reader, type, depth + 1);
            }
        } else {
            skip_payload(reader, type, depth + 1);
        }
    }
}

struct StreamedRegions {
    bool has_entry = false;
    bool selected_is_compound = false;
    std::string selected_name;
    StreamedRegion selected;
};

StreamedRegions read_regions_compound(
    nbt::io::stream_reader& reader,
    std::size_t depth)
{
    check_depth(depth);
    StreamedRegions result;
    while (true) {
        const auto type = reader.read_type(true);
        if (type == nbt::tag_type::End) return result;
        auto name = reader.read_string();
        if (!result.has_entry || name < result.selected_name) {
            // tag_compound stores entries in std::map, so the old DOM reader
            // selected the lexicographically first region, not the first one
            // on disk. Release a previously selected larger region before
            // decoding the replacement so two dense BlockStates arrays never
            // coexist at the peak.
            result.has_entry = true;
            result.selected_name = std::move(name);
            result.selected_is_compound = type == nbt::tag_type::Compound;
            result.selected = StreamedRegion{};
            if (result.selected_is_compound) {
                read_region_compound(reader, result.selected, depth + 1);
            } else {
                skip_payload(reader, type, depth + 1);
            }
        } else {
            // A duplicate name is ignored by libnbt's map::emplace. Larger
            // regions can never become regions.begin(), so validate and
            // discard them directly from the decompressor.
            skip_payload(reader, type, depth + 1);
        }
    }
}

struct StreamedRoot {
    bool regions_present = false;
    bool regions_is_compound = false;
    StreamedRegions regions;
};

StreamedRoot read_litematic_root(nbt::io::stream_reader& reader)
{
    if (reader.read_type() != nbt::tag_type::Compound) {
        reader.get_istr().setstate(std::ios::failbit);
        throw nbt::io::input_error("Tag is not a compound");
    }
    (void)reader.read_string();
    StreamedRoot result;
    while (true) {
        const auto type = reader.read_type(true);
        if (type == nbt::tag_type::End) return result;
        const auto key = reader.read_string();
        if (key == "Regions" && !result.regions_present) {
            result.regions_present = true;
            result.regions_is_compound = type == nbt::tag_type::Compound;
            if (result.regions_is_compound) {
                result.regions = read_regions_compound(reader, 1);
            } else {
                skip_payload(reader, type, 1);
            }
        } else {
            // This includes duplicate Regions tags. tag_compound retains the
            // first occurrence, but the entire later payload still has to be
            // consumed and validated.
            skip_payload(reader, type, 1);
        }
    }
}

bool required_region_fields_valid(const StreamedRegion& region) noexcept
{
    return region.position_state == RequiredFieldState::Valid &&
        region.size_state == RequiredFieldState::Valid &&
        region.palette_state == RequiredFieldState::Valid &&
        region.block_states_state == RequiredFieldState::Valid;
}

std::uint32_t palette_index_at(
    const std::vector<std::int64_t>& packed_states,
    std::uint8_t bits_per_block,
    std::size_t index) noexcept
{
    const auto start_bit = static_cast<std::uint64_t>(index) * bits_per_block;
    const auto word = static_cast<std::size_t>(start_bit / 64);
    const auto offset = static_cast<unsigned>(start_bit % 64);
    if (word >= packed_states.size()) return 0;
    std::uint64_t value = static_cast<std::uint64_t>(packed_states[word]) >> offset;
    if (offset + bits_per_block > 64 && word + 1 < packed_states.size()) {
        value |= static_cast<std::uint64_t>(packed_states[word + 1]) << (64 - offset);
    }
    return static_cast<std::uint32_t>(
        value & ((std::uint64_t{1} << bits_per_block) - 1));
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
    std::uint64_t value = static_cast<std::uint64_t>(mPackedStates[word]) >> offset;
    if (offset + mBitsPerBlock > 64 && word + 1 < mPackedStates.size()) {
        value |= static_cast<std::uint64_t>(mPackedStates[word + 1]) << (64 - offset);
    }
    return static_cast<std::uint32_t>(value & ((std::uint64_t{1} << mBitsPerBlock) - 1));
}

Result<void> LitematicStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 Litematic 文件: " + path.string());
    try {
        zlib::izlibstream decompressed(input);
        nbt::io::stream_reader reader(decompressed, endian::big);
        auto root = read_litematic_root(reader);
        if (!root.regions_present || !root.regions_is_compound) {
            return Result<void>::failure("Litematic 缺少 Regions");
        }
        if (!root.regions.has_entry || !root.regions.selected_is_compound) {
            return Result<void>::failure("Litematic 没有有效 Region");
        }
        auto& region = root.regions.selected;
        if (!required_region_fields_valid(region)) {
            return Result<void>::failure("Litematic Region 缺少 Position/Size/Palette/BlockStates");
        }
        const Size original_size{
            std::abs(region.signed_size.x),
            std::abs(region.signed_size.y),
            std::abs(region.signed_size.z)
        };
        if (original_size.width <= 0 || original_size.height <= 0 || original_size.length <= 0) {
            return Result<void>::failure("Litematic Region 尺寸无效");
        }

        std::vector<std::uint32_t> palette;
        palette.reserve(region.palette.size());
        const auto known_unknown = mRegistry.find("minecraft:unknown");
        const auto unknown = known_unknown ? *known_unknown :
            mRegistry.register_state(BlockState{ "minecraft:unknown", {}, 0 });
        for (const auto& encoded : region.palette) {
            palette.push_back(mRegistry.compatible_java_runtime_id(encoded).value_or(unknown));
        }
        if (palette.empty()) return Result<void>::failure("Litematic palette 为空");
        const auto bits_per_block = bits_for_palette(palette.size());
        const auto volume = static_cast<std::uint64_t>(original_size.volume());
        if (volume > std::numeric_limits<std::uint64_t>::max() / bits_per_block) {
            return Result<void>::failure("Litematic BlockStates 长度不足");
        }
        const auto required_bits = volume * bits_per_block;
        if (region.packed_states.size() * 64ull < required_bits) {
            return Result<void>::failure("Litematic BlockStates 长度不足");
        }

        std::size_t non_air_blocks = 0;
        for (std::size_t index = 0; index < static_cast<std::size_t>(volume); ++index) {
            const auto palette_index = water_structure::palette_index_at(
                region.packed_states, bits_per_block, index);
            if (palette_index < palette.size() &&
                palette[palette_index] != mRegistry.air_runtime_id()) {
                ++non_air_blocks;
            }
        }

        mRegionOrigin = region.position;
        mOriginalSize = original_size;
        mPalette = std::move(palette);
        mBitsPerBlock = bits_per_block;
        mPackedStates = std::move(region.packed_states);
        mBlockEntities = std::move(region.block_entities);
        mNonAirBlocks = non_air_blocks;
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 Litematic 失败: ") + error.what());
    }
}

Result<ChunkMap> LitematicStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, true);
}

Result<ChunkMap> LitematicStructure::get_chunks_layer0(std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, false);
}

Result<ChunkMap> LitematicStructure::get_chunks_impl(
    std::span<const ChunkPos> positions,
    bool include_layer1) const
{
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    const auto width = mOriginalSize.width;
    const auto length = mOriginalSize.length;
    const auto layer_size = static_cast<std::size_t>(width) * length;
    const auto air_runtime_id = mRegistry.air_runtime_id();
    for (auto& [chunk_pos, chunk] : result) {
        const auto chunk_min_x = static_cast<std::int64_t>(chunk_pos.x) * 16;
        const auto chunk_min_z = static_cast<std::int64_t>(chunk_pos.z) * 16;
        const auto min_x = std::max<std::int64_t>(0, chunk_min_x - mOffset.x);
        const auto max_x = std::min<std::int64_t>(width - 1, chunk_min_x + 15 - mOffset.x);
        const auto min_z = std::max<std::int64_t>(0, chunk_min_z - mOffset.z);
        const auto max_z = std::min<std::int64_t>(length - 1, chunk_min_z + 15 - mOffset.z);
        if (min_x > max_x || min_z > max_z) continue;
        for (int y = 0; y < mOriginalSize.height; ++y) {
            const int structure_y = y + mOffset.y;
            const int sub_y = floor_div(structure_y - 64, 16);
            const int local_y = structure_y - (sub_y * 16 + 64);
            SubChunkData* sub_chunk = nullptr;
            for (int z = static_cast<int>(min_z); z <= static_cast<int>(max_z); ++z) {
                for (int x = static_cast<int>(min_x); x <= static_cast<int>(max_x); ++x) {
                    const auto index = static_cast<std::size_t>(y) * layer_size +
                        static_cast<std::size_t>(z) * width + x;
                    const auto palette_index = palette_index_at(index);
                    if (palette_index >= mPalette.size()) continue;
                    const auto runtime_id = mPalette[palette_index];
                    if (runtime_id == air_runtime_id) continue;
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
                    const int local_z = static_cast<int>(
                        static_cast<std::int64_t>(z) + mOffset.z - chunk_min_z);
                    sub_chunk->layer0[static_cast<std::size_t>(
                        (local_y * 16 + local_z) * 16 + local_x)] = runtime_id;
                }
            }
        }
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
