#include "axiom_bp_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/ozlibstream.h>
#include <io/stream_writer.h>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace water_structure {
namespace {

constexpr std::uint32_t kMagic = 0x0AE5BB36;
constexpr std::int32_t kJavaDataVersion = 4556;

struct TemporaryFile {
    std::filesystem::path path;
    ~TemporaryFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

void write_be_i32(std::ostream& output, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value >> 24),
        static_cast<char>(value >> 16),
        static_cast<char>(value >> 8),
        static_cast<char>(value)
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t block_at(
    const ChunkMap& chunks,
    RuntimeRegistry& registry,
    const Size& size,
    int x,
    int y,
    int z)
{
    if (x < 0 || x >= size.width || y < 0 || y >= size.height || z < 0 || z >= size.length) {
        return registry.air_runtime_id();
    }
    const ChunkPos chunk_pos{ floor_div(x, 16), floor_div(z, 16) };
    const auto chunk = chunks.find(chunk_pos);
    if (chunk == chunks.end()) return registry.air_runtime_id();
    const auto sub_y = floor_div(y - 64, 16);
    const auto sub = chunk->second.sub_chunks.find(sub_y);
    if (sub == chunk->second.sub_chunks.end()) return registry.air_runtime_id();
    const auto local_y = y - (sub_y * 16 + 64);
    const auto index = static_cast<std::size_t>(
        (local_y * 16 + floor_mod(z, 16)) * 16 + floor_mod(x, 16));
    return sub->second.layer0[index];
}

void put_property(nbt::tag_compound& states, const BlockStateProperty& property)
{
    switch (property.type) {
    case BlockStateValueType::Byte:
        states[property.name] = static_cast<std::int8_t>(std::stoi(property.value));
        break;
    case BlockStateValueType::Short:
        states[property.name] = static_cast<std::int16_t>(std::stoi(property.value));
        break;
    case BlockStateValueType::Int:
        states[property.name] = static_cast<std::int32_t>(std::stol(property.value));
        break;
    case BlockStateValueType::Long:
        states[property.name] = static_cast<std::int64_t>(std::stoll(property.value));
        break;
    case BlockStateValueType::String:
        states[property.name] = nbt::tag_string(property.value);
        break;
    }
}

std::size_t bits_for_palette(std::size_t size)
{
    std::size_t bits = 0;
    for (auto value = size > 0 ? size - 1 : 0; value != 0; value >>= 1u) ++bits;
    return std::max<std::size_t>(4, bits);
}

std::vector<std::int64_t> pack_indices(
    const std::array<std::int32_t, 4096>& indices,
    std::size_t palette_size)
{
    const auto bits = bits_for_palette(palette_size);
    const auto values_per_long = std::max<std::size_t>(1, 64 / bits);
    const auto long_count = (indices.size() + values_per_long - 1) / values_per_long;
    const auto mask = (std::uint64_t{ 1 } << bits) - 1;
    std::vector<std::int64_t> result(long_count, 0);
    for (std::size_t index = 0; index < indices.size(); ++index) {
        const auto long_index = index / values_per_long;
        const auto bit_index = (index % values_per_long) * bits;
        const auto palette_index = static_cast<std::uint64_t>(std::max(indices[index], 0));
        auto packed = static_cast<std::uint64_t>(result[long_index]);
        packed |= (palette_index & mask) << bit_index;
        result[long_index] = static_cast<std::int64_t>(packed);
    }
    return result;
}

nbt::tag_compound palette_entry(RuntimeRegistry& registry, std::uint32_t runtime_id)
{
    auto state = registry.java_state(runtime_id).value_or(BlockState{ "minecraft:air", {}, 0 });
    if (state.name.find(':') == std::string::npos) state.name = "minecraft:" + state.name;
    nbt::tag_compound result;
    result["Name"] = nbt::tag_string(state.name);
    if (!state.states.empty()) {
        nbt::tag_compound properties;
        for (const auto& property : state.states) put_property(properties, property);
        result["Properties"] = std::move(properties);
    }
    return result;
}

std::string metadata_bytes(
    const std::filesystem::path& output_path,
    std::int32_t block_count,
    bool contains_air)
{
    auto name = output_path.stem().string();
    if (name.empty()) name = "AxiomStructure";
    const char* author_value = std::getenv("USER");
    if (!author_value || *author_value == '\0') author_value = std::getenv("USERNAME");
    const std::string author = author_value && *author_value != '\0'
        ? author_value
        : "WaterStructure";

    nbt::tag_compound metadata;
    metadata["ThumbnailYaw"] = nbt::tag_float(0.0f);
    metadata["ContainsAir"] = nbt::tag_byte(contains_air ? 1 : 0);
    metadata["Version"] = nbt::tag_int(2);
    metadata["LockedThumbnail"] = nbt::tag_byte(0);
    metadata["BlockCount"] = nbt::tag_int(block_count);
    metadata["Author"] = nbt::tag_string(author);
    metadata["Tags"] = nbt::tag_list(nbt::tag_type::String);
    metadata["Name"] = nbt::tag_string(name);
    metadata["ThumbnailPitch"] = nbt::tag_float(0.0f);
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", metadata, output, endian::big);
    return output.str();
}

} // namespace

Result<void> write_axiom_bp(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path,
    const ConversionOptions& options)
{
    if (!options.allow_temporary_spool) {
        return Result<void>::failure(
            "capability error: AxiomBP writer 需要临时压缩块数据，allow_temporary_spool=false");
    }
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 ||
        size.volume() <= 0 || size.volume() > std::numeric_limits<std::int32_t>::max()) {
        return Result<void>::failure("AxiomBP 输出尺寸无效或体积超过 int32");
    }

    const auto budget = options.soft_memory_budget_bytes;
    if (budget < 64u * 1024u) {
        return Result<void>::failure("AxiomBP writer soft_memory_budget 至少需要 64 KiB");
    }
    const auto chunk_count = static_cast<std::uint64_t>(size.chunk_x_count()) *
        static_cast<std::uint64_t>(size.chunk_z_count());
    if (options.max_in_flight_chunks != 0 &&
        chunk_count > options.max_in_flight_chunks) {
        return Result<void>::failure(
            "AxiomBP writer 需要读取完整 chunk 集合，超过 max_in_flight_chunks；请增大窗口或使用流式目标");
    }
    const auto subchunk_count = static_cast<std::uint64_t>((size.height + 15) / 16);
    // ChunkData stores one 4096-entry uint32 layer per subchunk.  This is a
    // conservative admission check; the writer still uses the temporary file
    // for compressed region payloads and never grows an unbounded output blob.
    const auto estimated_chunk_bytes = std::max<std::uint64_t>(64u * 1024u,
        subchunk_count * 4096u * sizeof(std::uint32_t) + 8u * 1024u);
    if (chunk_count > 0 && estimated_chunk_bytes > budget / chunk_count) {
        return Result<void>::failure(
            "AxiomBP writer 需要的 chunk 窗口超过 soft_memory_budget；无法安全物化输入");
    }
    std::vector<ChunkPos> positions;
    positions.reserve(static_cast<std::size_t>(size.chunk_x_count()) * size.chunk_z_count());
    for (int z = 0; z < size.chunk_z_count(); ++z) {
        for (int x = 0; x < size.chunk_x_count(); ++x) positions.push_back({ x, z });
    }
    auto chunks = structure.get_chunks_layer0(positions);
    if (!chunks) return Result<void>::failure("生成 AxiomBP chunks 失败: " + chunks.error());

    // This preserves the Go writer's inclusive end-subchunk calculation.
    const auto region_count = [](std::int32_t extent) {
        const auto end = extent - 1;
        return (end + floor_mod(end, 16) + 15) / 16 + 1;
    };
    const auto regions_x = region_count(size.width);
    const auto regions_y = region_count(size.height);
    const auto regions_z = region_count(size.length);
    const auto total_regions = static_cast<std::int64_t>(regions_x) * regions_y * regions_z;
    if (total_regions <= 0 || total_regions > std::numeric_limits<std::int32_t>::max()) {
        return Result<void>::failure("AxiomBP Region 数量无效或超过 int32");
    }

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryFile temporary{
        std::filesystem::temp_directory_path() /
        ("water_structure_axiom_" + std::to_string(unique) + ".tmp")
    };
    std::int64_t block_count = 0;
    try {
        std::ofstream compressed_file(temporary.path, std::ios::binary | std::ios::trunc);
        if (!compressed_file) {
            return Result<void>::failure("无法创建 AxiomBP 临时方块数据: " + temporary.path.string());
        }
        zlib::ozlibstream compressed(compressed_file, Z_BEST_SPEED, true);
        nbt::io::stream_writer writer(compressed, endian::big);
        writer.write_type(nbt::tag_type::Compound);
        writer.write_string("");
        writer.write_type(nbt::tag_type::List);
        writer.write_string("BlockRegion");
        writer.write_type(nbt::tag_type::Compound);
        writer.write_num<std::int32_t>(static_cast<std::int32_t>(total_regions));

        for (int region_y = 0; region_y < regions_y; ++region_y) {
            for (int region_z = 0; region_z < regions_z; ++region_z) {
                for (int region_x = 0; region_x < regions_x; ++region_x) {
                    std::array<std::int32_t, 4096> indices{};
                    std::unordered_map<std::uint32_t, std::int32_t> palette_indices;
                    std::vector<std::uint32_t> palette;
                    auto add_palette = [&](std::uint32_t runtime_id) {
                        if (const auto found = palette_indices.find(runtime_id);
                            found != palette_indices.end()) return found->second;
                        const auto index = static_cast<std::int32_t>(palette.size());
                        palette_indices.emplace(runtime_id, index);
                        palette.push_back(runtime_id);
                        return index;
                    };
                    add_palette(registry.air_runtime_id());
                    for (int local_y = 0; local_y < 16; ++local_y) {
                        for (int local_z = 0; local_z < 16; ++local_z) {
                            for (int local_x = 0; local_x < 16; ++local_x) {
                                const auto runtime_id = block_at(
                                    chunks.value(), registry, size,
                                    region_x * 16 + local_x,
                                    region_y * 16 + local_y,
                                    region_z * 16 + local_z);
                                const auto index = static_cast<std::size_t>(
                                    local_y * 256 + local_z * 16 + local_x);
                                indices[index] = add_palette(runtime_id);
                                if (runtime_id != registry.air_runtime_id()) ++block_count;
                            }
                        }
                    }

                    nbt::tag_list encoded_palette(nbt::tag_type::Compound);
                    for (const auto runtime_id : palette) {
                        encoded_palette.push_back(nbt::value_initializer(
                            palette_entry(registry, runtime_id)));
                    }
                    nbt::tag_compound block_states;
                    block_states["palette"] = std::move(encoded_palette);
                    block_states["data"] = nbt::tag_long_array(
                        pack_indices(indices, palette.size()));
                    nbt::tag_compound region;
                    region["BlockStates"] = std::move(block_states);
                    region["X"] = nbt::tag_int(region_x);
                    region["Y"] = nbt::tag_int(region_y);
                    region["Z"] = nbt::tag_int(region_z);
                    writer.write_payload(region);
                }
            }
        }
        writer.write_tag("DataVersion", nbt::tag_int(kJavaDataVersion));
        writer.write_type(nbt::tag_type::End);
        compressed.close();
        compressed_file.close();
        if (!compressed_file) return Result<void>::failure("写入 AxiomBP 临时方块数据失败");
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 AxiomBP 方块数据失败: " + std::string(error.what()));
    }

    if (block_count > std::numeric_limits<std::int32_t>::max()) {
        return Result<void>::failure("AxiomBP 非空气方块数量超过 int32");
    }
    std::error_code size_error;
    const auto block_data_size = std::filesystem::file_size(temporary.path, size_error);
    if (size_error || block_data_size > static_cast<std::uintmax_t>(std::numeric_limits<std::int32_t>::max())) {
        return Result<void>::failure("AxiomBP 压缩方块数据过大或无法读取大小");
    }
    const auto metadata = metadata_bytes(
        output_path,
        static_cast<std::int32_t>(block_count),
        block_count < size.volume());
    if (metadata.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return Result<void>::failure("AxiomBP metadata 超过 int32");
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 AxiomBP: " + output_path.string());
    write_be_i32(output, kMagic);
    write_be_i32(output, static_cast<std::uint32_t>(metadata.size()));
    output.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
    write_be_i32(output, 0);
    write_be_i32(output, static_cast<std::uint32_t>(block_data_size));
    std::ifstream block_data(temporary.path, std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    while (block_data) {
        block_data.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        output.write(buffer.data(), block_data.gcount());
    }
    if (!block_data.eof() || !output) return Result<void>::failure("写入 AxiomBP 文件失败");
    return Result<void>::success();
}

} // namespace water_structure
