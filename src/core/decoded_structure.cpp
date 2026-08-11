#include <WaterStructure/decoded_structure.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace water_structure {

void SparseBlockStore::clear()
{
    mBlocks.clear();
    mEntities.clear();
    mNonAirBlocks = 0;
    mOriginalSize = {};
    mSize = {};
    mOffset = {};
}

void SparseBlockStore::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

void SparseBlockStore::put(BlockPos local, std::uint32_t runtime_id)
{
    const auto previous = mBlocks.find(local);
    if (previous != mBlocks.end() && previous->second != mRegistry.air_runtime_id()) {
        --mNonAirBlocks;
    }
    mBlocks[local] = runtime_id;
    if (runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
}

void SparseBlockStore::put_entity(BlockPos local, NbtPayload payload)
{
    if (payload.empty()) mEntities.erase(local);
    else mEntities[local] = std::move(payload);
}

Result<ChunkMap> SparseBlockStore::get_chunks(std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, true);
}

Result<ChunkMap> SparseBlockStore::get_chunks_layer0(std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, false);
}

Result<ChunkMap> SparseBlockStore::get_chunks_impl(
    std::span<const ChunkPos> positions,
    bool include_layer1) const
{
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    const auto width = mOriginalSize.width;
    const auto height = mOriginalSize.height;
    const auto length = mOriginalSize.length;
    const auto air_runtime_id = mRegistry.air_runtime_id();
    for (auto& [chunk_pos, chunk] : result) {
        const auto chunk_min_x = static_cast<std::int64_t>(chunk_pos.x) * 16;
        const auto chunk_min_z = static_cast<std::int64_t>(chunk_pos.z) * 16;
        const auto source_min_x = std::max<std::int64_t>(0, chunk_min_x - mOffset.x);
        const auto source_max_x = std::min<std::int64_t>(
            static_cast<std::int64_t>(width) - 1,
            chunk_min_x + 15 - mOffset.x);
        const auto source_min_z = std::max<std::int64_t>(0, chunk_min_z - mOffset.z);
        const auto source_max_z = std::min<std::int64_t>(
            static_cast<std::int64_t>(length) - 1,
            chunk_min_z + 15 - mOffset.z);
        if (source_min_x > source_max_x || source_min_z > source_max_z) continue;

        for (auto x = source_min_x; x <= source_max_x; ++x) {
            for (std::int64_t y = 0; y < height; ++y) {
                const auto begin = mBlocks.lower_bound({
                    static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                    static_cast<std::int32_t>(source_min_z)});
                const auto end = mBlocks.upper_bound({
                    static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                    static_cast<std::int32_t>(source_max_z)});
                for (auto block = begin; block != end; ++block) {
                    const auto& local = block->first;
                    const auto world_x = local.x + mOffset.x;
                    const auto world_y = local.y + mOffset.y;
                    const auto world_z = local.z + mOffset.z;
                    const auto sub_y = floor_div(world_y - 64, 16);
                    auto [sub, inserted] = chunk.sub_chunks.try_emplace(sub_y);
                    if (inserted) {
                        sub->second.layer0.fill(air_runtime_id);
                        if (include_layer1) sub->second.layer1.fill(air_runtime_id);
                    }
                    const auto lx = world_x - static_cast<std::int32_t>(chunk_min_x);
                    const auto ly = world_y - (sub_y * 16 + 64);
                    const auto lz = world_z - static_cast<std::int32_t>(chunk_min_z);
                    if (lx < 0 || lx >= 16 || ly < 0 || ly >= 16 || lz < 0 || lz >= 16) {
                        return Result<ChunkMap>::failure("稀疏方块坐标 materialize 越界");
                    }
                    sub->second.layer0[static_cast<std::size_t>((ly * 16 + lz) * 16 + lx)] =
                        block->second;
                }
            }
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> SparseBlockStore::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    for (const auto& [local, payload] : mEntities) {
        const auto world_pos = BlockPos{ local.x + mOffset.x, local.y + mOffset.y, local.z + mOffset.z };
        const auto chunk = block_to_chunk(world_pos);
        const auto it = result.find(chunk);
        if (it == result.end()) continue;
        it->second.push_back({
            {
                world_pos.x - chunk.x * 16,
                structure_y_to_chunk_local(world_pos.y),
                world_pos.z - chunk.z * 16
            },
            payload
        });
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

} // namespace water_structure
