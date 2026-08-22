#include "axiom_bp.hpp"

#include <WaterStructure/world.hpp>

#include <io/stream_reader.h>
#include <nbt_tags.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::int32_t kMagic = 0x0AE5BB36;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;

const nbt::value* find_value(const nbt::tag_compound& compound, std::string_view key)
{
    const auto text = std::string(key);
    return compound.has_key(text) ? &compound.at(text) : nullptr;
}

std::optional<std::int32_t> int_value(const nbt::value* value)
{
    if (!value) return std::nullopt;
    switch (value->get_type()) {
    case nbt::tag_type::Byte: return value->as<nbt::tag_byte>().get();
    case nbt::tag_type::Short: return value->as<nbt::tag_short>().get();
    case nbt::tag_type::Int: return value->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return static_cast<std::int32_t>(value->as<nbt::tag_long>().get());
    default: return std::nullopt;
    }
}

std::optional<std::string> string_value(const nbt::value* value)
{
    if (!value || value->get_type() != nbt::tag_type::String) return std::nullopt;
    return value->as<nbt::tag_string>().get();
}

std::vector<std::uint8_t> read_exact(std::ifstream& input, std::size_t length, std::string_view context)
{
    std::vector<std::uint8_t> bytes(length);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (input.gcount() != static_cast<std::streamsize>(length)) {
        throw std::runtime_error(std::string(context) + " truncated at file offset " +
            std::to_string(static_cast<std::uint64_t>(input.tellg())));
    }
    return bytes;
}

std::vector<std::uint8_t> inflate_gzip(std::span<const std::uint8_t> input)
{
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        throw std::runtime_error("AxiomBP gzip initialization failed");
    }
    struct Guard { z_stream* value; ~Guard() { inflateEnd(value); } } guard{ &stream };
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(std::min<std::size_t>(input.size(), std::numeric_limits<uInt>::max()));
    std::size_t consumed = stream.avail_in;
    std::vector<std::uint8_t> output;
    std::array<std::uint8_t, 64 * 1024> chunk{};
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0 && consumed < input.size()) {
            const auto count = std::min<std::size_t>(input.size() - consumed,
                std::numeric_limits<uInt>::max());
            stream.next_in = const_cast<Bytef*>(input.data() + consumed);
            stream.avail_in = static_cast<uInt>(count);
            consumed += count;
        }
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::runtime_error("AxiomBP gzip failed at compressed offset " +
                std::to_string(stream.total_in));
        }
        const auto produced = chunk.size() - stream.avail_out;
        if (output.size() > kMaxDecodedBytes - produced) {
            throw std::runtime_error("AxiomBP gzip output exceeds 2 GiB");
        }
        output.insert(output.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));
        if (produced == 0 && stream.avail_in == 0 && consumed == input.size() && status != Z_STREAM_END) {
            throw std::runtime_error("AxiomBP gzip is truncated at compressed offset " +
                std::to_string(stream.total_in));
        }
    }
    return output;
}

std::unique_ptr<nbt::tag_compound> read_compound(std::span<const std::uint8_t> bytes)
{
    const std::string storage(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream input(storage, std::ios::binary);
    auto [_, root] = nbt::io::read_compound(input, endian::big);
    return std::move(root);
}

std::optional<BlockStateProperty> state_property(std::string name, const nbt::value& value)
{
    BlockStateProperty result;
    result.name = std::move(name);
    switch (value.get_type()) {
    case nbt::tag_type::Byte:
        result.type = BlockStateValueType::Byte;
        result.value = std::to_string(value.as<nbt::tag_byte>().get());
        break;
    case nbt::tag_type::Short:
        result.type = BlockStateValueType::Short;
        result.value = std::to_string(value.as<nbt::tag_short>().get());
        break;
    case nbt::tag_type::Int:
        result.type = BlockStateValueType::Int;
        result.value = std::to_string(value.as<nbt::tag_int>().get());
        break;
    case nbt::tag_type::Long:
        result.type = BlockStateValueType::Long;
        result.value = std::to_string(value.as<nbt::tag_long>().get());
        break;
    case nbt::tag_type::String:
        result.type = BlockStateValueType::String;
        result.value = value.as<nbt::tag_string>().get();
        break;
    default: return std::nullopt;
    }
    return result;
}

std::uint32_t palette_runtime(RuntimeRegistry& registry, const nbt::tag_compound& entry)
{
    const auto name = string_value(find_value(entry, "Name")).value_or("minecraft:unknown");
    std::vector<BlockStateProperty> properties;
    std::string java_state = name;
    if (const auto* raw = find_value(entry, "Properties");
        raw && raw->get_type() == nbt::tag_type::Compound) {
        java_state += '[';
        bool first = true;
        for (const auto& [key, value] : raw->as<nbt::tag_compound>()) {
            if (auto property = state_property(key, value)) {
                if (key != "waterlogged") {
                    if (!first) java_state += ',';
                    first = false;
                    java_state += key + '=' + property->value;
                }
                properties.push_back(std::move(*property));
            }
        }
        java_state += ']';
    }
    if (const auto runtime = registry.java_runtime_id(java_state)) return *runtime;
    if (const auto runtime = registry.compatible_java_runtime_id(java_state)) return *runtime;
    if (const auto runtime = registry.find(name, properties)) return *runtime;
    if (const auto runtime = registry.find(name)) return *runtime;
    if (const auto unknown = registry.find("minecraft:unknown")) return *unknown;
    return registry.register_state({ "minecraft:unknown", {}, 0 });
}

std::size_t bits_for_palette(std::size_t size)
{
    std::size_t bits = 0;
    std::size_t value = size > 0 ? size - 1 : 0;
    while (value != 0) { ++bits; value >>= 1u; }
    return std::max<std::size_t>(4, bits);
}

} // namespace

Result<void> AxiomBpReader::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open AxiomBP file: " + path.string());
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0) throw std::runtime_error("cannot determine AxiomBP input size");
        const auto file_size = static_cast<std::uint64_t>(end);
        input.seekg(0, std::ios::beg);
        const auto read_i32 = [&]() {
            const auto bytes = read_exact(input, 4, "AxiomBP header");
            const auto value = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                (static_cast<std::uint32_t>(bytes[1]) << 16) |
                (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
            return static_cast<std::int32_t>(value);
        };
        if (read_i32() != kMagic) throw std::runtime_error("invalid AxiomBP magic at offset 0");
        const auto skip_array = [&](std::string_view context) {
            const auto length = read_i32();
            if (length < 0 || static_cast<std::uint64_t>(length) > 2ull * 1024 * 1024 * 1024) {
                throw std::runtime_error(std::string(context) + " length is invalid at header offset " +
                    std::to_string(static_cast<std::uint64_t>(input.tellg()) - 4));
            }
            const auto start = input.tellg();
            if (start < 0 || static_cast<std::uint64_t>(start) > file_size ||
                static_cast<std::uint64_t>(length) > file_size - static_cast<std::uint64_t>(start)) {
                throw std::runtime_error(std::string(context) + " truncated at file offset " +
                    std::to_string(std::max<std::streamoff>(0, start)));
            }
            input.seekg(static_cast<std::streamoff>(length), std::ios::cur);
            if (!input) throw std::runtime_error(std::string(context) + " seek failed");
        };
        skip_array("AxiomBP metadata");
        skip_array("AxiomBP thumbnail");
        const auto data_length = read_i32();
        const auto data_start = input.tellg();
        if (data_length < 0 || data_start < 0 ||
            static_cast<std::uint64_t>(data_start) > file_size ||
            static_cast<std::uint64_t>(data_length) > file_size - static_cast<std::uint64_t>(data_start)) {
            throw std::runtime_error("AxiomBP block data length is invalid at header offset " +
                std::to_string(data_start < 4 ? 0 : static_cast<std::uint64_t>(data_start) - 4));
        }
        const auto compressed = read_exact(input, static_cast<std::size_t>(data_length), "AxiomBP block data");
        const auto decoded = inflate_gzip(compressed);
        const auto root = read_compound(decoded);
        const auto* regions_value = find_value(*root, "BlockRegion");
        if (!regions_value || regions_value->get_type() != nbt::tag_type::List) {
            throw std::runtime_error("AxiomBP BlockRegion list is missing");
        }
        const auto& regions = regions_value->as<nbt::tag_list>();
        if (regions.size() == 0) throw std::runtime_error("AxiomBP has no regions");

        std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_y = min_x, min_z = min_x;
        std::int32_t max_x = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_y = max_x, max_z = max_x;
        for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
            const auto& raw = regions.at(region_index);
            if (raw.get_type() != nbt::tag_type::Compound) {
                throw std::runtime_error("AxiomBP region #" + std::to_string(region_index) + " is not a compound");
            }
            const auto& region = raw.as<nbt::tag_compound>();
            const auto x = int_value(find_value(region, "X"));
            const auto y = int_value(find_value(region, "Y"));
            const auto z = int_value(find_value(region, "Z"));
            const auto* states = find_value(region, "BlockStates");
            if (!x || !y || !z || !states || states->get_type() != nbt::tag_type::Compound) {
                throw std::runtime_error("AxiomBP region #" + std::to_string(region_index) + " lacks X/Y/Z/BlockStates");
            }
            const auto* palette_value = find_value(states->as<nbt::tag_compound>(), "palette");
            const auto* data_value = find_value(states->as<nbt::tag_compound>(), "data");
            if (!palette_value || palette_value->get_type() != nbt::tag_type::List ||
                !data_value || data_value->get_type() != nbt::tag_type::Long_Array) {
                throw std::runtime_error("AxiomBP region #" + std::to_string(region_index) +
                    " lacks BlockStates palette/data");
            }
            min_x = std::min(min_x, *x); min_y = std::min(min_y, *y); min_z = std::min(min_z, *z);
            max_x = std::max(max_x, *x); max_y = std::max(max_y, *y); max_z = std::max(max_z, *z);
        }
        const auto provisional_width = (static_cast<std::int64_t>(max_x) - min_x + 1) * 16;
        const auto provisional_height = (static_cast<std::int64_t>(max_y) - min_y + 1) * 16;
        const auto provisional_length = (static_cast<std::int64_t>(max_z) - min_z + 1) * 16;
        if (provisional_width <= 0 || provisional_height <= 0 || provisional_length <= 0 ||
            provisional_width > std::numeric_limits<std::int32_t>::max() ||
            provisional_height > std::numeric_limits<std::int32_t>::max() ||
            provisional_length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("AxiomBP region bounds exceed int32");
        }
        mStore.set_size({ static_cast<std::int32_t>(provisional_width),
            static_cast<std::int32_t>(provisional_height),
            static_cast<std::int32_t>(provisional_length) });

        std::int32_t max_block_x = 0, max_block_y = 0, max_block_z = 0;
        for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
            const auto& region = regions.at(region_index).as<nbt::tag_compound>();
            const auto x = *int_value(find_value(region, "X"));
            const auto y = *int_value(find_value(region, "Y"));
            const auto z = *int_value(find_value(region, "Z"));
            const auto& states = find_value(region, "BlockStates")->as<nbt::tag_compound>();
            const auto& palette_values = find_value(states, "palette")->as<nbt::tag_list>();
            const auto& packed = find_value(states, "data")->as<nbt::tag_long_array>();
            std::vector<std::uint32_t> palette;
            palette.reserve(palette_values.size());
            for (const auto& palette_entry : palette_values) {
                if (palette_entry.get_type() != nbt::tag_type::Compound) {
                    palette.push_back(mRegistry.air_runtime_id());
                } else {
                    palette.push_back(palette_runtime(mRegistry,
                        palette_entry.as<nbt::tag_compound>()));
                }
            }
            const auto bits = bits_for_palette(palette.size());
            const auto per_long = 64 / bits;
            const auto mask = (std::uint64_t{ 1 } << bits) - 1;
            for (std::size_t index = 0; index < 4096; ++index) {
                const auto long_index = index / per_long;
                if (long_index >= packed.size()) break;
                const auto palette_index = static_cast<std::size_t>(
                    (static_cast<std::uint64_t>(packed.get()[long_index]) >>
                        ((index % per_long) * bits)) & mask);
                if (palette_index >= palette.size()) continue;
                const auto runtime = palette[palette_index];
                if (runtime == mRegistry.air_runtime_id()) continue;
                ++mNonAirBlocks;
                const auto local_y = static_cast<std::int32_t>(index / 256);
                const auto local_z = static_cast<std::int32_t>((index % 256) / 16);
                const auto local_x = static_cast<std::int32_t>(index % 16);
                const BlockPos position{
                    (x - min_x) * 16 + local_x,
                    (y - min_y) * 16 + local_y,
                    (z - min_z) * 16 + local_z
                };
                mStore.put(position, runtime);
                max_block_x = std::max(max_block_x, position.x + 1);
                max_block_y = std::max(max_block_y, position.y + 1);
                max_block_z = std::max(max_block_z, position.z + 1);
            }
        }
        mStore.set_size({ max_block_x, max_block_y, max_block_z });
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse AxiomBP failed: " + std::string(error.what()));
    }
}

Result<void> AxiomBpReader::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> AxiomBpReader::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("AxiomBP has no Go FromMCWorld capability");
}

} // namespace water_structure
