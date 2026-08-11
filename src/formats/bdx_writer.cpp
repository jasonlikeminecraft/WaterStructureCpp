#include "bdx_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <brotli/encode.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace water_structure {
namespace {

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_cstring(std::vector<std::uint8_t>& output, std::string_view value)
{
    output.insert(output.end(), value.begin(), value.end());
    output.push_back(0);
}

void append_move_axis(
    std::vector<std::uint8_t>& output,
    std::int32_t difference,
    std::uint8_t increment,
    std::uint8_t decrement,
    std::uint8_t int8_command,
    std::uint8_t int16_command,
    std::uint8_t int32_command)
{
    if (difference == 0) return;
    if (difference == 1) {
        output.push_back(increment);
    } else if (difference == -1) {
        output.push_back(decrement);
    } else if (difference >= std::numeric_limits<std::int8_t>::min() &&
        difference <= std::numeric_limits<std::int8_t>::max()) {
        output.push_back(int8_command);
        output.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(difference)));
    } else if (difference >= std::numeric_limits<std::int16_t>::min() &&
        difference <= std::numeric_limits<std::int16_t>::max()) {
        output.push_back(int16_command);
        append_u16(output, static_cast<std::uint16_t>(static_cast<std::int16_t>(difference)));
    } else {
        output.push_back(int32_command);
        append_u32(output, static_cast<std::uint32_t>(difference));
    }
}

void append_move(std::vector<std::uint8_t>& output, BlockPos& cursor, BlockPos target)
{
    append_move_axis(output, target.x - cursor.x, 14, 15, 28, 20, 21);
    append_move_axis(output, target.y - cursor.y, 16, 17, 29, 22, 23);
    append_move_axis(output, target.z - cursor.z, 18, 19, 30, 24, 25);
    cursor = target;
}

std::string escaped(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

std::string state_string(const BlockState& state)
{
    if (state.states.empty()) return "[]";
    auto properties = state.states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::string result = "[";
    for (std::size_t i = 0; i < properties.size(); ++i) {
        if (i != 0) result.push_back(',');
        const auto& property = properties[i];
        result += "\"" + escaped(property.name) + "\"=";
        if (property.type == BlockStateValueType::String) {
            result += "\"" + escaped(property.value) + "\"";
        } else if (property.type == BlockStateValueType::Byte) {
            result += property.value == "0" ? "false" : "true";
        } else {
            result += property.value;
        }
    }
    result.push_back(']');
    return result;
}

const BlockLayer* layer_at(const ChunkMap& chunks, ChunkPos chunk, std::int32_t sub_y)
{
    const auto chunk_it = chunks.find(chunk);
    if (chunk_it == chunks.end()) return nullptr;
    const auto sub_it = chunk_it->second.sub_chunks.find(sub_y);
    return sub_it == chunk_it->second.sub_chunks.end() ? nullptr : &sub_it->second.layer0;
}

} // namespace

Result<void> write_bdx(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path)
{
    try {
        const auto size = structure.size();
        if (size.width <= 0 || size.height <= 0 || size.length <= 0) {
            return Result<void>::failure("BDX writer: 结构尺寸无效");
        }
        std::vector<ChunkPos> positions;
        positions.reserve(static_cast<std::size_t>(size.chunk_x_count()) * size.chunk_z_count());
        for (std::int32_t x = 0; x < size.chunk_x_count(); ++x) {
            for (std::int32_t z = 0; z < size.chunk_z_count(); ++z) positions.push_back({ x, z });
        }
        auto chunks = structure.get_chunks_layer0(positions);
        if (!chunks) return Result<void>::failure("BDX writer 获取 chunks 失败: " + chunks.error());

        std::vector<std::uint8_t> decoded{ 'B', 'D', 'X', 0, 0 };
        std::unordered_map<std::uint32_t, std::pair<std::uint16_t, std::uint16_t>> palette;
        BlockPos cursor{};
        auto place = [&](std::uint32_t runtime_id, BlockPos position) -> Result<void> {
            append_move(decoded, cursor, position);
            auto found = palette.find(runtime_id);
            if (found == palette.end()) {
                if (palette.size() >= (std::numeric_limits<std::uint16_t>::max() + 1ull) / 2ull) {
                    return Result<void>::failure("BDX writer palette 超过 constant string uint16 范围");
                }
                auto state = registry.state(runtime_id).value_or(BlockState{ "minecraft:unknown", {}, 0 });
                const auto name_id = static_cast<std::uint16_t>(palette.size() * 2);
                const auto states_id = static_cast<std::uint16_t>(name_id + 1);
                decoded.push_back(1);
                append_cstring(decoded, state.name);
                decoded.push_back(1);
                append_cstring(decoded, state_string(state));
                found = palette.emplace(runtime_id, std::pair{ name_id, states_id }).first;
            }
            decoded.push_back(5);
            append_u16(decoded, found->second.first);
            append_u16(decoded, found->second.second);
            return Result<void>::success();
        };

        const auto minimum_sub_y = floor_div(-64, 16);
        const auto maximum_sub_y = floor_div(size.height - 1 - 64, 16);
        for (std::int32_t chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
            for (std::int32_t chunk_x = 0; chunk_x < size.chunk_x_count(); ++chunk_x) {
                for (std::int32_t sub_y = minimum_sub_y; sub_y <= maximum_sub_y; ++sub_y) {
                    const auto* layer = layer_at(chunks.value(), { chunk_x, chunk_z }, sub_y);
                    if (!layer) continue;
                    for (std::int32_t local_x = 0; local_x < 16; ++local_x) {
                        const auto x = chunk_x * 16 + local_x;
                        if (x >= size.width) break;
                        for (std::int32_t local_y = 0; local_y < 16; ++local_y) {
                            const auto y = sub_y * 16 + 64 + local_y;
                            if (y < 0 || y >= size.height) continue;
                            for (std::int32_t local_z = 0; local_z < 16; ++local_z) {
                                const auto z = chunk_z * 16 + local_z;
                                if (z >= size.length) break;
                                const auto index = static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x);
                                const auto runtime_id = (*layer)[index];
                                if (runtime_id == registry.air_runtime_id()) continue;
                                const auto placed = place(runtime_id, { x, y, z });
                                if (!placed) return placed;
                            }
                        }
                    }
                }
            }
        }
        decoded.push_back(88);

        std::vector<std::uint8_t> compressed(BrotliEncoderMaxCompressedSize(decoded.size()));
        std::size_t compressed_size = compressed.size();
        if (BrotliEncoderCompress(
            BROTLI_DEFAULT_QUALITY,
            BROTLI_DEFAULT_WINDOW,
            BROTLI_MODE_GENERIC,
            decoded.size(),
            decoded.data(),
            &compressed_size,
            compressed.data()) != BROTLI_TRUE) {
            return Result<void>::failure("BDX writer Brotli 压缩失败");
        }
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output) return Result<void>::failure("无法创建 BDX: " + output_path.string());
        output.write("BD@", 3);
        output.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed_size));
        if (!output) return Result<void>::failure("写入 BDX 失败: " + output_path.string());
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("BDX writer 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
