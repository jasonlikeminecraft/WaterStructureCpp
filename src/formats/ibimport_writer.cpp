#include "ibimport_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/stream_reader.h>
#include <nlohmann/json.hpp>
#include <tag_compound.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace water_structure {
namespace {

constexpr std::uint8_t kXorKey = 193;

std::array<std::uint8_t, 5> fixed_varint(std::uint64_t value)
{
    if (value > ((std::uint64_t{ 1 } << 35) - 1)) {
        throw std::runtime_error("IBImport 段长度超过 35-bit varint");
    }
    std::array<std::uint8_t, 5> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
        if (index + 1 != result.size()) result[index] |= 0x80u;
    }
    return result;
}

class SegmentWriter {
public:
    explicit SegmentWriter(std::ofstream& output) : mOutput(output)
    {
        mLengthPosition = mOutput.tellp();
        const auto placeholder = fixed_varint(0);
        mOutput.write(reinterpret_cast<const char*>(placeholder.data()), placeholder.size());
        mOutput.put(static_cast<char>(kXorKey));
        if (!mOutput) throw std::runtime_error("写入 IBImport 段头失败");
    }

    void write(std::string_view bytes)
    {
        std::array<char, 64 * 1024> buffer{};
        std::size_t consumed = 0;
        while (consumed < bytes.size()) {
            const auto count = std::min(buffer.size(), bytes.size() - consumed);
            for (std::size_t index = 0; index < count; ++index) {
                buffer[index] = static_cast<char>(
                    static_cast<std::uint8_t>(bytes[consumed + index]) ^ kXorKey);
            }
            mOutput.write(buffer.data(), static_cast<std::streamsize>(count));
            if (!mOutput) throw std::runtime_error("写入 IBImport 段数据失败");
            consumed += count;
            mLength += count;
        }
    }

    void close()
    {
        if (mClosed) return;
        const auto end = mOutput.tellp();
        const auto length = fixed_varint(mLength);
        mOutput.seekp(mLengthPosition);
        mOutput.write(reinterpret_cast<const char*>(length.data()), length.size());
        mOutput.seekp(end);
        if (!mOutput) throw std::runtime_error("回填 IBImport 段长度失败");
        mClosed = true;
    }

    ~SegmentWriter()
    {
        if (!mClosed) {
            try { close(); } catch (...) {}
        }
    }

private:
    std::ofstream& mOutput;
    std::streampos mLengthPosition{};
    std::uint64_t mLength = 0;
    bool mClosed = false;
};

std::uint32_t block_at(
    const ChunkData& chunk,
    RuntimeRegistry& registry,
    int x,
    int y,
    int z)
{
    const auto sub_y = floor_div(y - 64, 16);
    const auto sub = chunk.sub_chunks.find(sub_y);
    if (sub == chunk.sub_chunks.end()) return registry.air_runtime_id();
    const auto local_y = y - (sub_y * 16 + 64);
    const auto index = static_cast<std::size_t>(
        (local_y * 16 + floor_mod(z, 16)) * 16 + floor_mod(x, 16));
    return sub->second.layer0[index];
}

std::string block_name(RuntimeRegistry& registry, std::uint32_t runtime_id)
{
    auto state = registry.state(runtime_id);
    if (!state || state->name.empty()) return "unknown []";
    auto name = state->name;
    if (name.starts_with("minecraft:")) name.erase(0, 10);
    if (name.empty()) name = "unknown";
    auto properties = state->states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::string encoded = name + " [";
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) encoded += ',';
        encoded += nlohmann::json(properties[index].name).dump();
        encoded += '=';
        switch (properties[index].type) {
        case BlockStateValueType::Byte:
            encoded += properties[index].value == "0" ? "false" : "true";
            break;
        case BlockStateValueType::Short:
        case BlockStateValueType::Int:
        case BlockStateValueType::Long:
            encoded += properties[index].value;
            break;
        case BlockStateValueType::String:
            encoded += nlohmann::json(properties[index].value).dump();
            break;
        }
    }
    encoded += ']';
    return encoded;
}

std::string base64(std::string_view input)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto count = std::min<std::size_t>(3, input.size() - offset);
        std::uint32_t value = static_cast<std::uint8_t>(input[offset]) << 16;
        if (count > 1) value |= static_cast<std::uint8_t>(input[offset + 1]) << 8;
        if (count > 2) value |= static_cast<std::uint8_t>(input[offset + 2]);
        output.push_back(alphabet[(value >> 18) & 63]);
        output.push_back(alphabet[(value >> 12) & 63]);
        output.push_back(count > 1 ? alphabet[(value >> 6) & 63] : '=');
        output.push_back(count > 2 ? alphabet[value & 63] : '=');
    }
    return output;
}

const nbt::value* find_value(const nbt::tag_compound& compound, const char* key)
{
    return compound.has_key(key) ? &compound.at(key) : nullptr;
}

std::string string_value(const nbt::value* value)
{
    return value && value->get_type() == nbt::tag_type::String
        ? value->as<nbt::tag_string>().get()
        : std::string{};
}

std::int32_t int_value(const nbt::value* value)
{
    if (!value) return 0;
    switch (value->get_type()) {
    case nbt::tag_type::Byte: return value->as<nbt::tag_byte>().get();
    case nbt::tag_type::Short: return value->as<nbt::tag_short>().get();
    case nbt::tag_type::Int: return value->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return static_cast<std::int32_t>(value->as<nbt::tag_long>().get());
    default: return 0;
    }
}

bool command_block(const nbt::tag_compound& compound)
{
    return string_value(find_value(compound, "id")) == "CommandBlock";
}

} // namespace

Result<void> write_ibimport(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path)
{
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 || size.volume() <= 0) {
        return Result<void>::failure("IBImport 输出尺寸无效");
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 IBImport: " + output_path.string());
    try {
        output.write("IBImport ", 9);
        SegmentWriter script(output);
        for (int chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
            for (int chunk_x = 0; chunk_x < size.chunk_x_count(); ++chunk_x) {
                const ChunkPos position{ chunk_x, chunk_z };
                const std::array<ChunkPos, 1> requested{ position };
                auto chunks = structure.get_chunks_layer0(requested);
                if (!chunks) throw std::runtime_error("生成 IBImport chunk 失败: " + chunks.error());
                const auto found = chunks.value().find(position);
                const ChunkData empty;
                const auto& chunk = found == chunks.value().end() ? empty : found->second;
                const auto x_end = std::min(size.width, (chunk_x + 1) * 16);
                const auto z_end = std::min(size.length, (chunk_z + 1) * 16);
                for (int y = 0; y < size.height; ++y) {
                    for (int z = chunk_z * 16; z < z_end; ++z) {
                        for (int x = chunk_x * 16; x < x_end; ++x) {
                            const auto runtime_id = block_at(chunk, registry, x, y, z);
                            if (runtime_id == registry.air_runtime_id()) continue;
                            const auto line = "setblock ~" + std::to_string(x) + " ~" +
                                std::to_string(y) + " ~" + std::to_string(z) + " " +
                                block_name(registry, runtime_id) + "\r\n";
                            script.write(line);
                        }
                    }
                }
            }
        }
        script.close();

        SegmentWriter json_segment(output);
        json_segment.write("[\n");
        bool wrote_any = false;
        std::vector<ChunkPos> positions;
        positions.reserve(static_cast<std::size_t>(size.chunk_x_count()) * size.chunk_z_count());
        for (int chunk_x = 0; chunk_x < size.chunk_x_count(); ++chunk_x) {
            for (int chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
                positions.push_back({ chunk_x, chunk_z });
            }
        }
        auto entities = structure.get_chunk_nbt(positions);
        if (!entities) throw std::runtime_error("生成 IBImport NBT 失败: " + entities.error());
        for (const auto& [chunk_position, values] : entities.value()) {
            for (const auto& entity : values) {
                if (entity.payload.empty()) continue;
                const std::string bytes(entity.payload.begin(), entity.payload.end());
                std::istringstream input(bytes, std::ios::binary);
                auto [_, compound] = nbt::io::read_compound(input, endian::little);
                if (!command_block(*compound)) continue;
                const auto x = chunk_position.x * 16 + entity.pos.x;
                const auto y = entity.pos.y - kOverworldMinY;
                const auto z = chunk_position.z * 16 + entity.pos.z;
                if (x < 0 || x >= size.width || y < 0 || y >= size.height ||
                    z < 0 || z >= size.length) continue;
                const auto chunk = structure.get_chunks_layer0(std::array<ChunkPos, 1>{ chunk_position });
                if (!chunk) throw std::runtime_error(chunk.error());
                const auto chunk_it = chunk.value().find(chunk_position);
                const auto runtime_id = chunk_it == chunk.value().end()
                    ? registry.air_runtime_id()
                    : block_at(chunk_it->second, registry, x, y, z);
                auto state = registry.state(runtime_id);
                auto mode = 0;
                if (state) {
                    auto name = state->name;
                    if (name.starts_with("minecraft:")) name.erase(0, 10);
                    if (name == "chain_command_block") mode = 1;
                    else if (name == "repeating_command_block") mode = 2;
                }
                nlohmann::json item{
                    { "posX", "~" + std::to_string(x) },
                    { "posY", "~" + std::to_string(y) },
                    { "posZ", "~" + std::to_string(z) },
                    { "CommandMessage", base64(string_value(find_value(*compound, "Command"))) },
                    { "Commandtitle", string_value(find_value(*compound, "CustomName")) },
                    { "mode", mode },
                    { "isTime", int_value(find_value(*compound, "TickDelay")) },
                    { "Conditional", int_value(find_value(*compound, "conditionalMode")) != 0 ? 1 : 0 }
                };
                if (int_value(find_value(*compound, "auto")) == 0) item["isRedstone"] = true;
                if (wrote_any) json_segment.write(",\n");
                json_segment.write("    " + item.dump());
                wrote_any = true;
            }
        }
        if (wrote_any) json_segment.write("\n");
        json_segment.write("]\n");
        json_segment.close();
        output.close();
        if (!output) return Result<void>::failure("写入 IBImport 失败");
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 IBImport 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
