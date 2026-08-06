#include "construction.hpp"

#include <WaterStructure/world.hpp>

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <nbt_tags.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::string_view kMagic = "constrct";
constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::size_t kIndexEntrySize = 23;

struct SectionIndex {
    std::int32_t start_x = 0;
    std::int32_t start_y = 0;
    std::int32_t start_z = 0;
    std::uint8_t shape_x = 0;
    std::uint8_t shape_y = 0;
    std::uint8_t shape_z = 0;
    std::int32_t position = 0;
    std::int32_t length = 0;
};

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

std::uint32_t read_be_u32(std::span<const std::uint8_t, 4> bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) |
        static_cast<std::uint32_t>(bytes[3]);
}

std::int32_t read_le_i32(const std::uint8_t* bytes)
{
    const auto value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    return static_cast<std::int32_t>(value);
}

std::vector<std::uint8_t> read_exact(
    std::ifstream& input, std::uint64_t offset, std::size_t length, std::string_view context)
{
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) throw std::runtime_error(std::string(context) + " seek offset " + std::to_string(offset) + " failed");
    std::vector<std::uint8_t> bytes(length);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (input.gcount() != static_cast<std::streamsize>(length)) {
        throw std::runtime_error(std::string(context) + " truncated at file offset " +
            std::to_string(offset + static_cast<std::uint64_t>(std::max<std::streamsize>(0, input.gcount()))));
    }
    return bytes;
}

std::vector<std::uint8_t> inflate_payload(
    std::span<const std::uint8_t> input, std::string_view context)
{
    if (input.size() < 2) return { input.begin(), input.end() };
    const auto gzip = input[0] == 0x1f && input[1] == 0x8b;
    const auto zlib_stream = input[0] == 0x78;
    if (!gzip && !zlib_stream) return { input.begin(), input.end() };

    z_stream stream{};
    if (inflateInit2(&stream, gzip ? MAX_WBITS + 16 : MAX_WBITS) != Z_OK) {
        throw std::runtime_error(std::string(context) + " inflate initialization failed");
    }
    struct Guard {
        z_stream* stream;
        ~Guard() { inflateEnd(stream); }
    } guard{ &stream };

    std::vector<std::uint8_t> output;
    std::vector<std::uint8_t> buffer(kStreamChunk);
    std::size_t consumed = 0;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0 && consumed < input.size()) {
            const auto count = std::min<std::size_t>(
                input.size() - consumed, std::numeric_limits<uInt>::max());
            stream.next_in = const_cast<Bytef*>(input.data() + consumed);
            stream.avail_in = static_cast<uInt>(count);
            consumed += count;
        }
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::runtime_error(std::string(context) + " inflate failed at compressed offset " +
                std::to_string(stream.total_in) + ": " +
                (stream.msg == nullptr ? std::to_string(status) : stream.msg));
        }
        const auto produced = buffer.size() - stream.avail_out;
        if (output.size() > kMaxDecodedBytes - produced) {
            throw std::runtime_error(std::string(context) + " decoded payload exceeds 2 GiB");
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));
        if (produced == 0 && stream.avail_in == 0 && consumed == input.size() && status != Z_STREAM_END) {
            throw std::runtime_error(std::string(context) + " compressed payload is truncated at offset " +
                std::to_string(stream.total_in));
        }
    }
    return output;
}

std::unique_ptr<nbt::tag_compound> read_big_endian_compound(
    std::span<const std::uint8_t> bytes, std::string_view context)
{
    const std::string storage(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream input(storage, std::ios::binary);
    try {
        auto [_, root] = nbt::io::read_compound(input, endian::big);
        return std::move(root);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(context) + " NBT parse failed: " + error.what());
    }
}

std::vector<SectionIndex> parse_index(const nbt::tag_byte_array& raw)
{
    if (raw.size() == 0 || raw.size() % kIndexEntrySize != 0) {
        throw std::runtime_error("Construction section index length is invalid: " +
            std::to_string(raw.size()));
    }
    std::vector<SectionIndex> result;
    result.reserve(raw.size() / kIndexEntrySize);
    const auto* data = reinterpret_cast<const std::uint8_t*>(raw.get().data());
    for (std::size_t offset = 0; offset < raw.size(); offset += kIndexEntrySize) {
        result.push_back({
            read_le_i32(data + offset), read_le_i32(data + offset + 4),
            read_le_i32(data + offset + 8), data[offset + 12], data[offset + 13], data[offset + 14],
            read_le_i32(data + offset + 15), read_le_i32(data + offset + 19)
        });
    }
    return result;
}

std::unique_ptr<nbt::tag> normalize_nbt(const nbt::value& value)
{
    switch (value.get_type()) {
    case nbt::tag_type::Byte:
        return std::make_unique<nbt::tag_int>(value.as<nbt::tag_byte>().get());
    case nbt::tag_type::Short:
        return std::make_unique<nbt::tag_int>(value.as<nbt::tag_short>().get());
    case nbt::tag_type::Int:
        return std::make_unique<nbt::tag_int>(value.as<nbt::tag_int>().get());
    case nbt::tag_type::Long:
        return std::make_unique<nbt::tag_int>(static_cast<std::int32_t>(value.as<nbt::tag_long>().get()));
    case nbt::tag_type::Float:
        return std::make_unique<nbt::tag_double>(value.as<nbt::tag_float>().get());
    case nbt::tag_type::Double:
        return std::make_unique<nbt::tag_double>(value.as<nbt::tag_double>().get());
    case nbt::tag_type::Byte_Array:
        return value.get().clone();
    case nbt::tag_type::String:
        return value.get().clone();
    case nbt::tag_type::List: {
        auto result = std::make_unique<nbt::tag_list>();
        for (const auto& item : value.as<nbt::tag_list>()) {
            result->push_back(nbt::value_initializer(normalize_nbt(item)));
        }
        return result;
    }
    case nbt::tag_type::Compound: {
        auto result = std::make_unique<nbt::tag_compound>();
        for (const auto& [key, item] : value.as<nbt::tag_compound>()) {
            result->put(key, nbt::value_initializer(normalize_nbt(item)));
        }
        return result;
    }
    case nbt::tag_type::Int_Array:
    case nbt::tag_type::Long_Array:
        return value.get().clone();
    default:
        throw std::runtime_error("Construction NBT contains unsupported tag type");
    }
}

NbtPayload serialize_compound(const nbt::tag_compound& compound)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", compound, output, endian::little);
    const auto bytes = output.str();
    return NbtPayload(bytes.begin(), bytes.end());
}

std::vector<BlockStateProperty> palette_property_variants(
    std::string name, const nbt::value& value)
{
    std::vector<BlockStateProperty> result;
    auto add_numeric = [&](std::string text) {
        result.push_back({ name, BlockStateValueType::Int, std::move(text) });
    };
    switch (value.get_type()) {
    case nbt::tag_type::Byte:
        add_numeric(std::to_string(value.as<nbt::tag_byte>().get()));
        break;
    case nbt::tag_type::Short:
        add_numeric(std::to_string(value.as<nbt::tag_short>().get()));
        break;
    case nbt::tag_type::Int:
        add_numeric(std::to_string(value.as<nbt::tag_int>().get()));
        break;
    case nbt::tag_type::Long:
        add_numeric(std::to_string(static_cast<std::int32_t>(value.as<nbt::tag_long>().get())));
        break;
    case nbt::tag_type::String:
        result.push_back({ name, BlockStateValueType::String, value.as<nbt::tag_string>().get() });
        break;
    default:
        break;
    }
    return result;
}

std::uint32_t palette_runtime_id(RuntimeRegistry& registry, const nbt::tag_compound& entry)
{
    const auto name_space = string_value(find_value(entry, "namespace"));
    const auto block_name = string_value(find_value(entry, "blockname"));
    if (!name_space || !block_name) throw std::runtime_error("Construction palette entry is missing namespace/blockname");
    const auto name = *name_space + ":" + *block_name;
    const auto default_runtime = registry.find(name);
    std::vector<std::pair<std::string, std::vector<BlockStateProperty>>> property_options;
    if (const auto* raw = find_value(entry, "properties"); raw && raw->get_type() == nbt::tag_type::Compound) {
        for (const auto& [property_name, value] : raw->as<nbt::tag_compound>()) {
            if (property_name == "__version__") continue;
            auto variants = palette_property_variants(property_name, value);
            if (!variants.empty()) property_options.emplace_back(property_name, std::move(variants));
        }
    }
    std::vector<BlockStateProperty> properties;
    std::function<std::optional<std::uint32_t>(std::size_t)> try_properties = [&](std::size_t index) {
        if (index == property_options.size()) return registry.find(name, properties);
        for (const auto& property : property_options[index].second) {
            properties.push_back(property);
            if (const auto runtime = try_properties(index + 1)) return runtime;
            properties.pop_back();
        }
        return std::optional<std::uint32_t>{};
    };
    if (const auto runtime = try_properties(0)) return *runtime;
    // Go's StateToRuntimeID retries the block's registered default property set.
    if (default_runtime) return *default_runtime;
    if (const auto unknown = registry.find("minecraft:unknown")) return *unknown;
    return registry.register_state({ "minecraft:unknown", {}, 0 });
}

std::vector<std::int32_t> section_blocks(const nbt::tag_compound& section)
{
    const auto array_type = int_value(find_value(section, "blocks_array_type"));
    const auto* blocks = find_value(section, "blocks");
    if (!array_type || !blocks) return {};
    std::vector<std::int32_t> result;
    if (*array_type == 7 && blocks->get_type() == nbt::tag_type::Byte_Array) {
        const auto& values = blocks->as<nbt::tag_byte_array>();
        result.reserve(values.size());
        for (const auto value : values) result.push_back(static_cast<std::uint8_t>(value));
    } else if (*array_type == 11 && blocks->get_type() == nbt::tag_type::Int_Array) {
        result.assign(blocks->as<nbt::tag_int_array>().begin(), blocks->as<nbt::tag_int_array>().end());
    } else if (*array_type == 12 && blocks->get_type() == nbt::tag_type::Long_Array) {
        const auto& values = blocks->as<nbt::tag_long_array>();
        result.reserve(values.size());
        for (const auto value : values) result.push_back(static_cast<std::int32_t>(value));
    } else {
        throw std::runtime_error("Construction blocks type does not match blocks_array_type " +
            std::to_string(*array_type));
    }
    return result;
}

std::array<std::int32_t, 3> section_shape(const nbt::tag_compound& section)
{
    for (const auto key : { "shape", "size" }) {
        const auto* raw = find_value(section, key);
        if (!raw || raw->get_type() != nbt::tag_type::List) continue;
        const auto& values = raw->as<nbt::tag_list>();
        if (values.size() != 3) continue;
        const auto x = int_value(&values.at(0));
        const auto y = int_value(&values.at(1));
        const auto z = int_value(&values.at(2));
        if (x && y && z) return { *x, *y, *z };
    }
    return {};
}

} // namespace

Result<void> ConstructionReader::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open Construction file: " + path.string());
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < static_cast<std::streamoff>(kMagic.size() * 2 + 5)) {
            throw std::runtime_error("file is truncated at offset " + std::to_string(std::max<std::streamoff>(0, end)));
        }
        const auto file_size = static_cast<std::uint64_t>(end);
        const auto header = read_exact(input, 0, kMagic.size() + 1, "Construction header");
        if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
            throw std::runtime_error("invalid header magic at offset 0");
        }
        if (header[kMagic.size()] != 0) {
            throw std::runtime_error("unsupported format version " + std::to_string(header[kMagic.size()]) +
                " at offset " + std::to_string(kMagic.size()));
        }
        const auto footer = read_exact(input, file_size - 12, 12, "Construction footer");
        if (std::memcmp(footer.data() + 4, kMagic.data(), kMagic.size()) != 0) {
            throw std::runtime_error("invalid footer magic at offset " + std::to_string(file_size - kMagic.size()));
        }
        std::array<std::uint8_t, 4> pointer_bytes{};
        std::copy_n(footer.begin(), 4, pointer_bytes.begin());
        const auto metadata_start = static_cast<std::uint64_t>(read_be_u32(pointer_bytes));
        const auto metadata_end = file_size - 12;
        if (metadata_start < kMagic.size() + 1 || metadata_start >= metadata_end) {
            throw std::runtime_error("invalid metadata pointer " + std::to_string(metadata_start) +
                " at file offset " + std::to_string(file_size - 12));
        }
        const auto compressed_metadata = read_exact(input, metadata_start,
            static_cast<std::size_t>(metadata_end - metadata_start), "Construction metadata");
        const auto metadata_bytes = inflate_payload(compressed_metadata, "Construction metadata");
        const auto metadata = read_big_endian_compound(metadata_bytes, "Construction metadata");

        const auto* index_value = find_value(*metadata, "section_index_table");
        const auto* palette_value = find_value(*metadata, "block_palette");
        if (!index_value || index_value->get_type() != nbt::tag_type::Byte_Array) {
            throw std::runtime_error("Construction metadata section_index_table is missing or invalid");
        }
        if (!palette_value || palette_value->get_type() != nbt::tag_type::List ||
            palette_value->as<nbt::tag_list>().size() == 0) {
            throw std::runtime_error("Construction metadata block_palette is missing or empty");
        }
        const auto sections = parse_index(index_value->as<nbt::tag_byte_array>());

        std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_y = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_z = std::numeric_limits<std::int32_t>::max();
        std::int32_t max_x = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_y = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_z = std::numeric_limits<std::int32_t>::min();
        if (const auto* boxes = find_value(*metadata, "selection_boxes");
            boxes && boxes->get_type() == nbt::tag_type::Int_Array) {
            const auto& values = boxes->as<nbt::tag_int_array>();
            for (std::size_t index = 0; index + 5 < values.size(); index += 6) {
                min_x = std::min(min_x, values[index]);
                min_y = std::min(min_y, values[index + 1]);
                min_z = std::min(min_z, values[index + 2]);
                max_x = std::max(max_x, values[index + 3]);
                max_y = std::max(max_y, values[index + 4]);
                max_z = std::max(max_z, values[index + 5]);
            }
        }
        if (max_x <= min_x || max_y <= min_y || max_z <= min_z) {
            min_x = min_y = min_z = std::numeric_limits<std::int32_t>::max();
            max_x = max_y = max_z = std::numeric_limits<std::int32_t>::min();
            for (const auto& section : sections) {
                if (section.shape_x == 0 || section.shape_y == 0 || section.shape_z == 0) continue;
                const auto end_x = static_cast<std::int64_t>(section.start_x) + section.shape_x;
                const auto end_y = static_cast<std::int64_t>(section.start_y) + section.shape_y;
                const auto end_z = static_cast<std::int64_t>(section.start_z) + section.shape_z;
                if (end_x > std::numeric_limits<std::int32_t>::max() ||
                    end_y > std::numeric_limits<std::int32_t>::max() ||
                    end_z > std::numeric_limits<std::int32_t>::max()) {
                    throw std::runtime_error("Construction section bounds overflow int32");
                }
                min_x = std::min(min_x, section.start_x);
                min_y = std::min(min_y, section.start_y);
                min_z = std::min(min_z, section.start_z);
                max_x = std::max(max_x, static_cast<std::int32_t>(end_x));
                max_y = std::max(max_y, static_cast<std::int32_t>(end_y));
                max_z = std::max(max_z, static_cast<std::int32_t>(end_z));
            }
        }
        if (max_x <= min_x || max_y <= min_y || max_z <= min_z) {
            throw std::runtime_error("Construction bounds are invalid");
        }
        const Size size{ max_x - min_x, max_y - min_y, max_z - min_z };
        if (size.volume() <= 0) throw std::runtime_error("Construction size volume is invalid");
        mStore.set_size(size);

        std::vector<std::uint32_t> palette;
        palette.reserve(palette_value->as<nbt::tag_list>().size());
        for (const auto& raw : palette_value->as<nbt::tag_list>()) {
            if (raw.get_type() != nbt::tag_type::Compound) {
                throw std::runtime_error("Construction palette entry is not a compound");
            }
            palette.push_back(palette_runtime_id(mRegistry, raw.as<nbt::tag_compound>()));
        }
        const auto unknown = mRegistry.find("minecraft:unknown").value_or(
            mRegistry.register_state({ "minecraft:unknown", {}, 0 }));

        for (std::size_t section_index = 0; section_index < sections.size(); ++section_index) {
            const auto& entry = sections[section_index];
            if (entry.length <= 0 || entry.shape_x == 0 || entry.shape_y == 0 || entry.shape_z == 0) continue;
            const auto section_end = static_cast<std::uint64_t>(entry.position) +
                static_cast<std::uint64_t>(entry.length);
            if (entry.position < 0 || section_end > file_size) {
                throw std::runtime_error("Construction section #" + std::to_string(section_index) +
                    " has invalid range at file offset " + std::to_string(entry.position));
            }
            const auto compressed = read_exact(input, static_cast<std::uint64_t>(entry.position),
                static_cast<std::size_t>(entry.length), "Construction section #" + std::to_string(section_index));
            const auto bytes = inflate_payload(compressed, "Construction section #" + std::to_string(section_index));
            const auto section = read_big_endian_compound(bytes, "Construction section #" + std::to_string(section_index));
            const auto blocks = section_blocks(*section);
            const auto parsed_shape = section_shape(*section);
            if (parsed_shape[0] > 0 && parsed_shape[1] > 0 && parsed_shape[2] > 0) {
                const auto expected = static_cast<std::uint64_t>(parsed_shape[0]) * parsed_shape[1] * parsed_shape[2];
                if (expected != blocks.size()) {
                    throw std::runtime_error("Construction section #" + std::to_string(section_index) +
                        " block count mismatch");
                }
            }
            const auto indexed_volume = static_cast<std::size_t>(entry.shape_x) * entry.shape_y * entry.shape_z;
            if (!blocks.empty() && blocks.size() < indexed_volume) {
                throw std::runtime_error("Construction section #" + std::to_string(section_index) +
                    " blocks are truncated at index " + std::to_string(blocks.size()));
            }
            if (!blocks.empty()) {
                for (std::int32_t x = 0; x < entry.shape_x; ++x) {
                    for (std::int32_t y = 0; y < entry.shape_y; ++y) {
                        for (std::int32_t z = 0; z < entry.shape_z; ++z) {
                            const auto index = static_cast<std::size_t>(
                                (x * entry.shape_y + y) * entry.shape_z + z);
                            const auto palette_index = blocks[index];
                            const auto runtime = palette_index >= 0 &&
                                static_cast<std::size_t>(palette_index) < palette.size()
                                ? palette[static_cast<std::size_t>(palette_index)] : unknown;
                            if (runtime == mRegistry.air_runtime_id()) continue;
                            ++mNonAirBlocks;
                            mStore.put({ entry.start_x - min_x + x, entry.start_y - min_y + y,
                                entry.start_z - min_z + z }, runtime);
                        }
                    }
                }
            }

            const auto* entities = find_value(*section, "block_entities");
            if (!entities || entities->get_type() != nbt::tag_type::List) continue;
            for (const auto& raw : entities->as<nbt::tag_list>()) {
                if (raw.get_type() != nbt::tag_type::Compound) continue;
                const auto& entity = raw.as<nbt::tag_compound>();
                const auto x = int_value(find_value(entity, "x"));
                const auto y = int_value(find_value(entity, "y"));
                const auto z = int_value(find_value(entity, "z"));
                if (!x || !y || !z) continue;
                auto normalized = std::make_unique<nbt::tag_compound>();
                if (const auto* nbt_value = find_value(entity, "nbt");
                    nbt_value && nbt_value->get_type() == nbt::tag_type::Compound) {
                    normalized = std::unique_ptr<nbt::tag_compound>(
                        static_cast<nbt::tag_compound*>(normalize_nbt(*nbt_value).release()));
                }
                if (!normalized->has_key("id")) {
                    const auto name_space = string_value(find_value(entity, "namespace")).value_or("");
                    const auto base_name = string_value(find_value(entity, "base_name")).value_or("");
                    if (!base_name.empty()) {
                        normalized->put("id", nbt::value_initializer(nbt::tag_string(
                            name_space.empty() ? base_name : name_space + ":" + base_name)));
                    }
                }
                mStore.put_entity({ *x - min_x, *y - min_y, *z - min_z }, serialize_compound(*normalized));
            }
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse Construction failed: " + std::string(error.what()));
    }
}

Result<void> ConstructionReader::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> ConstructionReader::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("Construction has no Go FromMCWorld capability");
}

} // namespace water_structure
