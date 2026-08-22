#include "runaway.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace water_structure {
namespace {
struct ParseFailure final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

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
        // Parse directly into the representation retained by the reader.  The previous
        // implementation kept a PendingBlock array and then allocated mBlocks beside it,
        // briefly requiring two full copies of every entry.  stable_sort preserves source
        // order within a coordinate, so selecting the final entry still implements JSON
        // stream "last placement wins" semantics without carrying a source-index field.
        std::vector<Block> blocks;
        bool root_is_array = false;
        std::size_t entry_count = 0;
        auto consume_entry = [&](const nlohmann::json& entry) {
            const auto index = entry_count++;
            if (index > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
                throw ParseFailure("RunAway 方块条目数超过 uint32 容量");
            }
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
                throw ParseFailure("RunAway 方块条目缺少 name: " + std::to_string(index));
            }
            const auto x = integer(entry.value("x", nlohmann::json{})), y = integer(entry.value("y", nlohmann::json{})), z = integer(entry.value("z", nlohmann::json{}));
            if (!x || !y || !z) throw ParseFailure("RunAway 坐标无效: " + std::to_string(index));
            const auto name = entry["name"].get<std::string>();
            const auto aux = entry.contains("aux") ? integer(entry["aux"]) : std::optional<std::int32_t>{ 0 };
            if (!aux) throw ParseFailure("RunAway aux 无效: " + std::to_string(index));
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
            blocks.push_back({ { *x, *y, *z }, runtime.value_or(mRegistry.air_runtime_id()) });
        };
        const auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                                  nlohmann::json& parsed) -> bool {
            if (depth == 0) {
                if (event == nlohmann::json::parse_event_t::array_start) {
                    root_is_array = true;
                    return true;
                }
                if (event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::value) {
                    // Reject a non-array root without retaining a potentially huge DOM.
                    return false;
                }
            }
            if (!root_is_array || depth != 1) return true;
            if (event == nlohmann::json::parse_event_t::value ||
                event == nlohmann::json::parse_event_t::object_end ||
                event == nlohmann::json::parse_event_t::array_end) {
                consume_entry(parsed);
                // Each top-level entry is complete and has been normalized. Do not retain it
                // in the parser's root array.
                return false;
            }
            return true;
        };
        const auto discarded_root = nlohmann::json::parse(input, callback);
        (void)discarded_root;
        if (!root_is_array || entry_count == 0) {
            return Result<void>::failure("RunAway 根节点不是非空数组");
        }
        if (blocks.empty()) return Result<void>::failure("RunAway 没有方块");
        std::stable_sort(blocks.begin(), blocks.end(), [](const Block& left, const Block& right) {
            return left.pos < right.pos;
        });
        std::array<std::int32_t, 3> min{ std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max() }, max{ std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min() };
        std::size_t unique_count = 0;
        for (std::size_t begin = 0; begin < blocks.size();) {
            std::size_t end = begin + 1;
            while (end < blocks.size() && blocks[end].pos == blocks[begin].pos) ++end;
            auto selected = std::move(blocks[end - 1]);
            min[0] = std::min(min[0], selected.pos.x);
            min[1] = std::min(min[1], selected.pos.y);
            min[2] = std::min(min[2], selected.pos.z);
            max[0] = std::max(max[0], selected.pos.x);
            max[1] = std::max(max[1], selected.pos.y);
            max[2] = std::max(max[2], selected.pos.z);
            blocks[unique_count++] = std::move(selected);
            begin = end;
        }
        blocks.resize(unique_count);
        const auto width = static_cast<std::int64_t>(max[0]) - min[0] + 1;
        const auto height = static_cast<std::int64_t>(max[1]) - min[1] + 1;
        const auto length = static_cast<std::int64_t>(max[2]) - min[2] + 1;
        if (width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) {
            return Result<void>::failure("RunAway 归一化尺寸溢出 int32");
        }
        mNonAirBlocks = 0;
        for (auto& block : blocks) {
            block.pos = {
                block.pos.x - min[0], block.pos.y - min[1], block.pos.z - min[2]
            };
            if (block.runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
        }
        mBlocks = std::move(blocks);
        mOriginalSize = { static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) };
        set_offset({});
        return Result<void>::success();
    } catch (const ParseFailure& error) {
        return Result<void>::failure(error.what());
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
