#include "kbdx_structure.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace water_structure {

namespace {

std::int32_t read_i32(const std::array<std::uint8_t, 4>& bytes)
{
    const auto value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
    return static_cast<std::int32_t>(value);
}

std::uint32_t read_u32(std::istream& input)
{
    std::array<std::uint8_t, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("KBDX 文件意外结束");
    }
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

std::uint32_t block_at(const SubChunkData& chunk, int layer, int x, int y, int z)
{
    const auto index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
    return layer == 0 ? chunk.layer0[index] : chunk.layer1[index];
}

std::uint32_t& block_at(SubChunkData& chunk, int layer, int x, int y, int z)
{
    const auto index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
    return layer == 0 ? chunk.layer0[index] : chunk.layer1[index];
}

std::optional<std::int32_t> json_int(const nlohmann::json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end()) return std::nullopt;
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return static_cast<std::int32_t>(value);
    }
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(value);
    }
    if (found->is_number_float()) {
        const auto value = found->get<double>();
        if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return static_cast<std::int32_t>(value);
    }
    return std::nullopt;
}

bool json_bool(const nlohmann::json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end()) return false;
    if (found->is_boolean()) return found->get<bool>();
    if (found->is_number_integer()) return found->get<std::int64_t>() != 0;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>() != 0;
    if (found->is_number_float()) return found->get<double>() != 0;
    if (found->is_string()) {
        auto value = found->get<std::string>();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value == "true" || value == "1";
    }
    return false;
}

std::string json_string(const nlohmann::json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return "<nil>";
    if (found->is_string()) return found->get<std::string>();
    if (found->is_boolean()) return found->get<bool>() ? "true" : "false";
    return found->dump();
}

NbtPayload command_block_nbt(const nlohmann::json& entity)
{
    static const std::regex execute_regex(
        R"([ ]*?/?[ ]*?execute[ ]*?(as|at|align|anchored|facing|in|positioned|rotated|if|unless|run))");

    const auto command = json_string(entity, "Command");
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("CommandBlock");
    root["Command"] = nbt::tag_string(command);
    root["CustomName"] = nbt::tag_string(json_string(entity, "CustomName"));
    root["ExecuteOnFirstTick"] = nbt::tag_byte(json_bool(entity, "ExecuteOnFirstTick") ? 1 : 0);
    root["TrackOutput"] = nbt::tag_byte(json_bool(entity, "TrackOutput") ? 1 : 0);
    root["conditionalMode"] = nbt::tag_byte(json_bool(entity, "isConditional") ? 1 : 0);
    root["auto"] = nbt::tag_byte(json_bool(entity, "redstone") ? 0 : 1);
    root["TickDelay"] = nbt::tag_int(json_int(entity, "TickDelay").value_or(0));
    root["Powered"] = nbt::tag_byte(0);
    root["LPCommandMode"] = nbt::tag_int(json_int(entity, "Mode").value_or(0));
    root["LastOutput"] = nbt::tag_string(json_string(entity, "LastOutput"));
    root["Version"] = nbt::tag_int(std::regex_search(command, execute_regex) ? 38 : 19);

    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", root, output, endian::little);
    const auto bytes = output.str();
    return { bytes.begin(), bytes.end() };
}

} // namespace

void KbdxStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mChunkIndex.clear();
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> KbdxStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::failure("无法打开 KBDX 文件: " + path.string());
    }

    try {
        const auto block_count = read_u32(input);
        if (block_count == 0 || block_count > 100000000) {
            return Result<void>::failure("KBDX 方块数量无效");
        }

        struct RawBlock {
            std::int32_t x;
            std::int32_t y;
            std::int32_t z;
            std::uint32_t index;
            std::uint32_t aux;
        };
        std::vector<RawBlock> raw;
        raw.reserve(block_count);
        for (std::uint32_t i = 0; i < block_count; ++i) {
            RawBlock block{};
            std::array<std::uint8_t, 4> bytes{};
            input.read(reinterpret_cast<char*>(bytes.data()), 4);
            if (input.gcount() != 4) throw std::runtime_error("读取 KBDX X 失败");
            block.x = read_i32(bytes);
            input.read(reinterpret_cast<char*>(bytes.data()), 4);
            if (input.gcount() != 4) throw std::runtime_error("读取 KBDX Y 失败");
            block.y = read_i32(bytes);
            input.read(reinterpret_cast<char*>(bytes.data()), 4);
            if (input.gcount() != 4) throw std::runtime_error("读取 KBDX Z 失败");
            block.z = read_i32(bytes);
            block.index = read_u32(input);
            block.aux = read_u32(input);
            raw.push_back(block);
        }

        std::string metadata((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (metadata.empty()) {
            return Result<void>::failure("KBDX 缺少元数据 JSON");
        }
        const auto root = nlohmann::json::parse(metadata);
        if (!root.is_object()) {
            return Result<void>::failure("KBDX 元数据根节点不是对象");
        }

        std::unordered_map<BlockPos, NbtPayload, BlockPosHash> block_entities;
        if (const auto entities = root.find("BlockEntityData");
            entities != root.end() && entities->is_array()) {
            for (const auto& entity : *entities) {
                if (!entity.is_object()) continue;
                const auto x = json_int(entity, "x");
                const auto y = json_int(entity, "y");
                const auto z = json_int(entity, "z");
                if (!x || !y || !z) continue;
                const auto id = json_string(entity, "id");
                if (id.ends_with("command_block")) {
                    block_entities[{ *x, *y, *z }] = command_block_nbt(entity);
                }
            }
        }

        std::unordered_map<std::uint32_t, std::string> palette;
        for (const auto& [name, value] : root.items()) {
            if (value.is_number_integer()) {
                palette[static_cast<std::uint32_t>(value.get<std::int64_t>())] = name;
            }
        }
        if (palette.empty()) {
            return Result<void>::failure("KBDX 元数据没有 palette");
        }

        int min_x = std::numeric_limits<int>::max();
        int min_y = std::numeric_limits<int>::max();
        int min_z = std::numeric_limits<int>::max();
        int max_x = std::numeric_limits<int>::min();
        int max_y = std::numeric_limits<int>::min();
        int max_z = std::numeric_limits<int>::min();

        mBlocks.clear();
        mBlockEntities.clear();
        mNonAirBlocks = 0;
        for (const auto& item : raw) {
            const auto palette_it = palette.find(item.index);
            auto name = palette_it == palette.end() ? std::string{} : palette_it->second;
            name.erase(name.begin(), std::find_if(name.begin(), name.end(), [](unsigned char c) {
                return !std::isspace(c);
            }));
            name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char c) {
                return !std::isspace(c);
            }).base(), name.end());
            if (!name.empty() && name.find(':') == std::string::npos) name = "minecraft:" + name;
            const auto runtime_id = mRegistry.legacy_runtime_id(name, static_cast<std::uint16_t>(item.aux))
                .or_else([&] { return mRegistry.compatible_java_runtime_id(name); })
                .value_or(mRegistry.find("minecraft:unknown").value_or(mRegistry.air_runtime_id()));
            min_x = std::min(min_x, static_cast<int>(item.x));
            min_y = std::min(min_y, static_cast<int>(item.y));
            min_z = std::min(min_z, static_cast<int>(item.z));
            max_x = std::max(max_x, static_cast<int>(item.x));
            max_y = std::max(max_y, static_cast<int>(item.y));
            max_z = std::max(max_z, static_cast<int>(item.z));
            mBlocks.push_back({ item.x, item.y, item.z, runtime_id });
            if (runtime_id != mRegistry.air_runtime_id()) {
                ++mNonAirBlocks;
            }
        }

        if (mBlocks.empty()) {
            return Result<void>::failure("KBDX 没有方块");
        }
        for (auto& block : mBlocks) {
            block.x -= min_x;
            block.y -= min_y;
            block.z -= min_z;
        }
        for (auto& [position, payload] : block_entities) {
            mBlockEntities.emplace(
                BlockPos{ position.x - min_x, position.y - min_y, position.z - min_z },
                std::move(payload));
        }
        mOriginalSize = {
            max_x - min_x + 1,
            max_y - min_y + 1,
            max_z - min_z + 1
        };
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 KBDX 失败: ") + error.what());
    }
}

Result<ChunkMap> KbdxStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, ChunkData{});
    }

    if (!mChunkIndex.ensure(mBlocks, mOffset, [](const Block& block) {
        return BlockPos{ block.x, block.y, block.z };
    })) return Result<ChunkMap>::failure("KBDX chunk index 超过 uint32 容量");
    for (auto& [chunk_pos, chunk] : result) {
        const auto* indexed = mChunkIndex.find(chunk_pos);
        if (!indexed) continue;
        for (const auto index : *indexed) {
            const auto& block = mBlocks[index];
            const int x = block.x + mOffset.x;
            const int y = block.y + mOffset.y;
            const int z = block.z + mOffset.z;
            const int sub_y = floor_div(y - 64, 16);
            const int local_x = x - chunk_pos.x * 16;
            const int local_y = y - (sub_y * 16 + 64);
            const int local_z = z - (chunk_pos.z * 16);
            auto [sub_it, inserted] = chunk.sub_chunks.try_emplace(sub_y);
            if (inserted) {
                sub_it->second.layer0.fill(mRegistry.air_runtime_id());
                sub_it->second.layer1.fill(mRegistry.air_runtime_id());
            }
            block_at(sub_it->second, 0, local_x, local_y, local_z) = block.runtime_id;
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> KbdxStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, std::vector<BlockEntity>{});
    }
    for (const auto& [source, payload] : mBlockEntities) {
        const BlockPos position{
            source.x + mOffset.x,
            source.y + mOffset.y,
            source.z + mOffset.z
        };
        const auto chunk = block_to_chunk(position);
        const auto found = result.find(chunk);
        if (found == result.end()) continue;
        found->second.push_back({
            {
                floor_mod(position.x, 16),
                structure_y_to_chunk_local(position.y),
                floor_mod(position.z, 16)
            },
            payload
        });
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> KbdxStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> KbdxStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("KBDX 导出尚未迁移");
}

} // namespace water_structure
