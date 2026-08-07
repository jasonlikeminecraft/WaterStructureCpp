#include "schem_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/ozlibstream.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_primitive.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace water_structure {
namespace {

constexpr std::int32_t kJavaDataVersion = 4556;
constexpr std::size_t kCopyBufferSize = 1u << 20;
using Clock = std::chrono::steady_clock;

double elapsed_ms(const Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void append_varint(char* output, std::size_t& cursor, std::uint32_t value)
{
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
        if (value != 0) byte |= 0x80u;
        output[cursor++] = static_cast<char>(byte);
    } while (value != 0);
}

std::string java_state_string(RuntimeRegistry& registry, std::uint32_t runtime_id)
{
    const auto state = registry.java_state(runtime_id);
    if (!state) return "minecraft:air";
    auto name = state->name;
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    if (state->states.empty()) return name;
    auto properties = state->states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    name += '[';
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) name += ',';
        name += properties[index].name;
        name += '=';
        if (properties[index].type == BlockStateValueType::Byte) {
            name += properties[index].value == "0" ? "false" : "true";
        } else {
            name += properties[index].value;
        }
    }
    name += ']';
    return name;
}

struct StripeFile {
    std::filesystem::path path;
    std::vector<std::uint64_t> row_offsets;
};

struct TemporaryStripeFiles {
    std::vector<StripeFile> files;
    ~TemporaryStripeFiles()
    {
        for (const auto& file : files) {
            std::error_code error;
            std::filesystem::remove(file.path, error);
        }
    }
};

std::filesystem::path make_temp_path()
{
    static std::atomic<std::uint64_t> sequence{ 0 };
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::temp_directory_path() /
        ("water_structure_schem_" + std::to_string(now) + "_" + std::to_string(id) + ".tmp");
}

Result<void> write_byte_array(
    nbt::io::stream_writer& writer,
    const std::string& name,
    const std::vector<StripeFile>& stripes,
    int height,
    std::uint64_t total_size)
{
    if (total_size > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        return Result<void>::failure("Schem BlockData 超过 int32");
    }
    writer.write_type(nbt::tag_type::Byte_Array);
    writer.write_string(name);
    writer.write_num<std::int32_t>(static_cast<std::int32_t>(total_size));

    std::vector<std::ifstream> inputs;
    inputs.reserve(stripes.size());
    for (const auto& stripe : stripes) {
        inputs.emplace_back(stripe.path, std::ios::binary);
        if (!inputs.back()) return Result<void>::failure("无法读取 Schem 临时 BlockData");
    }
    std::vector<char> buffer(kCopyBufferSize);
    for (int y = 0; y < height; ++y) {
        for (std::size_t stripe_index = 0; stripe_index < stripes.size(); ++stripe_index) {
            auto& input = inputs[stripe_index];
            auto remaining = stripes[stripe_index].row_offsets[static_cast<std::size_t>(y) + 1] -
                stripes[stripe_index].row_offsets[static_cast<std::size_t>(y)];
            while (remaining != 0) {
                const auto count = static_cast<std::streamsize>(
                    std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(buffer.data(), count);
                if (input.gcount() != count) {
                    return Result<void>::failure("读取 Schem 临时 BlockData 时文件被截断");
                }
                writer.get_ostr().write(buffer.data(), count);
                if (!writer.get_ostr()) return Result<void>::failure("写入 Schem BlockData 失败");
                remaining -= static_cast<std::uint64_t>(count);
            }
        }
    }
    return Result<void>::success();
}

} // namespace

Result<void> write_schem(
    const IStructure& structure,
    RuntimeRegistry& registry,
    StructureId format,
    const std::filesystem::path& output_path)
{
    if (format != StructureId::SchemV1 && format != StructureId::SchemV2) {
        return Result<void>::failure("Schem writer 收到不支持的格式");
    }
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 ||
        size.width > std::numeric_limits<std::int16_t>::max() ||
        size.height > std::numeric_limits<std::int16_t>::max() ||
        size.length > std::numeric_limits<std::int16_t>::max() ||
        size.volume() <= 0 || size.volume() > std::numeric_limits<std::int32_t>::max()) {
        return Result<void>::failure("Schem 输出尺寸无效、超过 int16 或体积超过 int32");
    }

    std::unordered_map<std::uint32_t, std::int32_t> palette_indices;
    constexpr std::size_t kRuntimePaletteCacheSize = 1u << 16;
    std::vector<std::int32_t> runtime_palette_cache(
        kRuntimePaletteCacheSize, static_cast<std::int32_t>(-1));
    std::unordered_map<std::string, std::int32_t> palette_state_indices;
    std::vector<std::uint32_t> palette;
    std::vector<std::string> palette_state_strings;
    palette.reserve(1024);
    if (format == StructureId::SchemV1) {
        palette_indices.emplace(registry.air_runtime_id(), 0);
        if (registry.air_runtime_id() < runtime_palette_cache.size()) {
            runtime_palette_cache[registry.air_runtime_id()] = 0;
        }
        palette.push_back(registry.air_runtime_id());
        palette_state_indices.emplace("minecraft:air", 0);
        palette_state_strings.emplace_back("minecraft:air");
    }

    TemporaryStripeFiles temporary;
    const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr;
    double get_chunks_ms = 0.0;
    double encode_ms = 0.0;
    const auto chunk_z_count = size.chunk_z_count();
    const auto chunk_x_count = size.chunk_x_count();
    std::uint64_t total_block_data = 0;
    for (int chunk_z = 0; chunk_z < chunk_z_count; ++chunk_z) {
        std::vector<ChunkPos> positions;
        positions.reserve(static_cast<std::size_t>(chunk_x_count));
        for (int chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) {
            positions.push_back({ chunk_x, chunk_z });
        }
        const auto get_chunks_start = Clock::now();
        auto chunks = structure.get_chunks_layer0(positions);
        get_chunks_ms += elapsed_ms(get_chunks_start);
        if (!chunks) return Result<void>::failure("生成 Schem chunks 失败: " + chunks.error());

        const auto encode_start = Clock::now();
        StripeFile stripe{ make_temp_path(), std::vector<std::uint64_t>(
            static_cast<std::size_t>(size.height) + 1, 0) };
        std::ofstream output(stripe.path, std::ios::binary | std::ios::trunc);
        if (!output) return Result<void>::failure("无法创建 Schem 临时 BlockData");
        const auto z_begin = chunk_z * 16;
        const auto z_end = std::min(size.length, z_begin + 16);
        std::uint64_t stripe_size = 0;
        std::vector<const BlockLayer*> layers(static_cast<std::size_t>(chunk_x_count));
        std::vector<char> row_buffer;
        row_buffer.resize(static_cast<std::size_t>(size.width) *
            static_cast<std::size_t>(z_end - z_begin) * 5);
        const auto palette_index_for = [&](const std::uint32_t runtime_id) {
            if (runtime_id < runtime_palette_cache.size()) {
                const auto cached = runtime_palette_cache[runtime_id];
                if (cached >= 0) return cached;
            }
            auto found = palette_indices.find(runtime_id);
            if (found != palette_indices.end()) {
                if (runtime_id < runtime_palette_cache.size()) {
                    runtime_palette_cache[runtime_id] = found->second;
                }
                return found->second;
            }
            const auto state_string = java_state_string(registry, runtime_id);
            const auto state_found = palette_state_indices.find(state_string);
            const auto index = state_found == palette_state_indices.end()
                ? static_cast<std::int32_t>(palette.size()) : state_found->second;
            if (state_found == palette_state_indices.end()) {
                palette_state_indices.emplace(state_string, index);
                palette_state_strings.push_back(state_string);
                palette.push_back(runtime_id);
            }
            palette_indices.emplace(runtime_id, index);
            if (runtime_id < runtime_palette_cache.size()) {
                runtime_palette_cache[runtime_id] = index;
            }
            return index;
        };
        for (int y = 0; y < size.height; ++y) {
            stripe.row_offsets[static_cast<std::size_t>(y)] = stripe_size;
            const auto sub_y = floor_div(y - 64, 16);
            const auto local_y = y - (sub_y * 16 + 64);
            for (int chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) {
                layers[static_cast<std::size_t>(chunk_x)] = nullptr;
                const auto chunk = chunks.value().find({ chunk_x, chunk_z });
                if (chunk == chunks.value().end()) continue;
                const auto sub = chunk->second.sub_chunks.find(sub_y);
                if (sub != chunk->second.sub_chunks.end()) {
                    layers[static_cast<std::size_t>(chunk_x)] = &sub->second.layer0;
                }
            }
            std::size_t row_size = 0;
            for (int z = z_begin; z < z_end; ++z) {
                const auto local_z = floor_mod(z, 16);
                for (int chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) {
                    const auto x_begin = chunk_x * 16;
                    const auto x_end = std::min(size.width, x_begin + 16);
                    const auto* layer = layers[static_cast<std::size_t>(chunk_x)];
                    if (layer == nullptr) {
                        const auto air_index = palette_index_for(registry.air_runtime_id());
                        for (int x = x_begin; x < x_end; ++x) {
                            static_cast<void>(x);
                            append_varint(row_buffer.data(), row_size, static_cast<std::uint32_t>(air_index));
                        }
                        continue;
                    }
                    const auto row_start = static_cast<std::size_t>(local_y * 16 + local_z) * 16;
                    for (int x = x_begin; x < x_end; ++x) {
                        const auto runtime_id = (*layer)[row_start + static_cast<std::size_t>(x - x_begin)];
                        append_varint(row_buffer.data(), row_size,
                            static_cast<std::uint32_t>(palette_index_for(runtime_id)));
                    }
                }
            }
            stripe_size += row_size;
            output.write(row_buffer.data(), static_cast<std::streamsize>(row_size));
            if (!output) return Result<void>::failure("写入 Schem 临时 BlockData 失败");
        }
        stripe.row_offsets[static_cast<std::size_t>(size.height)] = stripe_size;
        output.close();
        if (!output) return Result<void>::failure("写入 Schem 临时 BlockData 失败");
        total_block_data += stripe_size;
        temporary.files.push_back(std::move(stripe));
        encode_ms += elapsed_ms(encode_start);
        structure.release_cached_chunks();
        if (total_block_data > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return Result<void>::failure("Schem BlockData 超过 int32");
        }
    }

    nbt::tag_compound encoded_palette;
    for (std::size_t index = 0; index < palette.size(); ++index) {
        encoded_palette[palette_state_strings[index]] =
            nbt::tag_int(static_cast<std::int32_t>(index));
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 Schem: " + output_path.string());
    try {
        const auto merge_start = Clock::now();
        zlib::ozlibstream compressed(output, Z_BEST_SPEED, true);
        nbt::io::stream_writer writer(compressed, endian::big);
        writer.write_type(nbt::tag_type::Compound);
        writer.write_string("Schematic");
        writer.write_tag("DataVersion", nbt::tag_int(kJavaDataVersion));
        writer.write_tag("Width", nbt::tag_short(static_cast<std::int16_t>(size.width)));
        writer.write_tag("Height", nbt::tag_short(static_cast<std::int16_t>(size.height)));
        writer.write_tag("Length", nbt::tag_short(static_cast<std::int16_t>(size.length)));
        if (format == StructureId::SchemV1) {
            auto written = write_byte_array(writer, "BlockData", temporary.files, size.height, total_block_data);
            if (!written) return written;
            writer.write_tag("PaletteMax", nbt::tag_int(static_cast<std::int32_t>(palette.size())));
            writer.write_tag("Palette", encoded_palette);
        } else {
            writer.write_tag("Version", nbt::tag_int(2));
            writer.write_type(nbt::tag_type::Compound);
            writer.write_string("Blocks");
            auto written = write_byte_array(writer, "Data", temporary.files, size.height, total_block_data);
            if (!written) return written;
            writer.write_tag("Palette", encoded_palette);
            writer.write_type(nbt::tag_type::End);
        }
        const auto merge_ms = elapsed_ms(merge_start);
        const auto close_start = Clock::now();
        writer.write_type(nbt::tag_type::End);
        compressed.close();
        const auto close_ms = elapsed_ms(close_start);
        output.close();
        if (!output) return Result<void>::failure("写入 Schem 失败");
        if (profile) {
            std::cerr << "schem_profile get_chunks_ms=" << get_chunks_ms
                      << " encode_ms=" << encode_ms
                      << " merge_ms=" << merge_ms
                      << " gzip_close_ms=" << close_ms
                      << " stripes=" << temporary.files.size()
                      << " block_data_bytes=" << total_block_data << '\n';
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 Schem 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
