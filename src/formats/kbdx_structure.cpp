#include "kbdx_structure.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
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

} // namespace

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
        mNonAirBlocks = 0;
        for (const auto& item : raw) {
            const auto palette_it = palette.find(item.index);
            const auto name = palette_it == palette.end() ? "minecraft:unknown" : palette_it->second;
            const auto existing_runtime = mRegistry.find(name);
            const auto runtime_id = existing_runtime ? *existing_runtime :
                mRegistry.register_state(BlockState{ name, {}, 0 });
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
        mSize = {
            max_x - min_x + 1,
            max_y - min_y + 1,
            max_z - min_z + 1
        };
        mOffset = {};
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

    for (const auto& block : mBlocks) {
        const int x = block.x + mOffset.x;
        const int y = block.y + mOffset.y;
        const int z = block.z + mOffset.z;
        const ChunkPos chunk_pos{ floor_div(x, 16), floor_div(z, 16) };
        const auto chunk_it = result.find(chunk_pos);
        if (chunk_it == result.end()) {
            continue;
        }
        const int sub_y = floor_div(y - 64, 16);
        const int local_x = x - chunk_pos.x * 16;
        const int local_y = y - (sub_y * 16 + 64);
        const int local_z = z - chunk_pos.z * 16;
        auto [sub_it, inserted] = chunk_it->second.sub_chunks.try_emplace(sub_y);
        if (inserted) {
            sub_it->second.layer0.fill(mRegistry.air_runtime_id());
            sub_it->second.layer1.fill(mRegistry.air_runtime_id());
        }
        block_at(sub_it->second, 0, local_x, local_y, local_z) = block.runtime_id;
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> KbdxStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) {
        result.emplace(pos, std::vector<BlockEntity>{});
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
