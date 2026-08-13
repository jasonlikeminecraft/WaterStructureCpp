#include "runaway.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>

namespace water_structure {
namespace {
std::optional<std::int32_t> integer(const nlohmann::json& value)
{
    if (value.is_number_integer()) return value.get<std::int32_t>();
    if (value.is_array() && !value.empty() && value.front().is_number_integer()) return value.front().get<std::int32_t>();
    return std::nullopt;
}
}

void RunAwayStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mChunkIndex.clear();
    mSize = { mOriginalSize.width + std::abs(offset.x), mOriginalSize.height + std::abs(offset.y), mOriginalSize.length + std::abs(offset.z) };
}

Result<void> RunAwayStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 RunAway 文件: " + path.string());
    try {
        const auto entries = nlohmann::json::parse(input);
        if (!entries.is_array() || entries.empty()) return Result<void>::failure("RunAway 根节点不是非空数组");
        std::map<std::array<std::int32_t, 3>, std::uint32_t> blocks;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) return Result<void>::failure("RunAway 方块条目缺少 name: " + std::to_string(index));
            const auto x = integer(entry.value("x", nlohmann::json{})), y = integer(entry.value("y", nlohmann::json{})), z = integer(entry.value("z", nlohmann::json{}));
            if (!x || !y || !z) return Result<void>::failure("RunAway 坐标无效: " + std::to_string(index));
            const auto name = entry["name"].get<std::string>();
            const auto aux = entry.contains("aux") ? integer(entry["aux"]) : std::optional<std::int32_t>{ 0 };
            if (!aux) return Result<void>::failure("RunAway aux 无效: " + std::to_string(index));
            auto runtime = mRegistry.legacy_runtime_id(name, static_cast<std::uint16_t>(*aux));
            if (!runtime) runtime = mRegistry.java_runtime_id(name);
            if (!runtime) runtime = mRegistry.find(name);
            if (!runtime && name.find(':') == std::string::npos) {
                const auto prefixed = "minecraft:" + name;
                runtime = mRegistry.legacy_runtime_id(prefixed, static_cast<std::uint16_t>(*aux));
                if (!runtime) runtime = mRegistry.java_runtime_id(prefixed);
                if (!runtime) runtime = mRegistry.find(prefixed);
            }
            if (!runtime) runtime = mRegistry.find("minecraft:unknown");
            blocks[{ *x, *y, *z }] = runtime.value_or(mRegistry.air_runtime_id());
        }
        if (blocks.empty()) return Result<void>::failure("RunAway 没有方块");
        std::array<std::int32_t, 3> min{ std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max() }, max{ std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min() };
        for (const auto& [key, _] : blocks) for (int axis = 0; axis < 3; ++axis) { min[axis] = std::min(min[axis], key[axis]); max[axis] = std::max(max[axis], key[axis]); }
        mBlocks.clear(); mBlocks.reserve(blocks.size()); mNonAirBlocks = 0;
        for (const auto& [key, runtime] : blocks) { mBlocks.push_back({ { key[0] - min[0], key[1] - min[1], key[2] - min[2] }, runtime }); if (runtime != mRegistry.air_runtime_id()) ++mNonAirBlocks; }
        mOriginalSize = { max[0] - min[0] + 1, max[1] - min[1] + 1, max[2] - min[2] + 1 }; set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) { return Result<void>::failure(std::string("解析 RunAway 失败: ") + error.what()); }
}

Result<ChunkMap> RunAwayStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result; for (const auto pos : positions) result.emplace(pos, ChunkData{});
    if (!mChunkIndex.ensure(mBlocks, mOffset, [](const Block& block) { return block.pos; }))
        return Result<ChunkMap>::failure("RunAway chunk index 超过 uint32 容量");
    for (auto& [chunk_pos, chunk] : result) {
        const auto* indexed = mChunkIndex.find(chunk_pos);
        if (!indexed) continue;
        for (const auto index : *indexed) {
            const auto& block = mBlocks[index];
            const auto x = block.pos.x + mOffset.x;
            const auto y = block.pos.y + mOffset.y;
            const auto z = block.pos.z + mOffset.z;
            const auto sy = floor_div(y - 64, 16);
            auto [sub, inserted] = chunk.sub_chunks.try_emplace(sy);
            if (inserted) {
                sub->second.layer0.fill(mRegistry.air_runtime_id());
                sub->second.layer1.fill(mRegistry.air_runtime_id());
            }
            const auto lx = x - chunk_pos.x * 16;
            const auto ly = y - (sy * 16 + 64);
            const auto lz = z - chunk_pos.z * 16;
            sub->second.layer0[static_cast<std::size_t>((ly * 16 + lz) * 16 + lx)] = block.runtime_id;
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}
Result<NbtChunkMap> RunAwayStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const { NbtChunkMap result; for(const auto pos:positions)result.emplace(pos,std::vector<BlockEntity>{}); return Result<NbtChunkMap>::success(std::move(result)); }
Result<void> RunAwayStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const { return convert_to_world(*this,world,start,std::move(callbacks)); }
Result<void> RunAwayStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks) { return Result<void>::failure("RunAway 导出尚未迁移"); }

} // namespace water_structure
