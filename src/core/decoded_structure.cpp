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
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    for (const auto& [local, runtime_id] : mBlocks) {
        const auto x = local.x + mOffset.x;
        const auto y = local.y + mOffset.y;
        const auto z = local.z + mOffset.z;
        const ChunkPos chunk_pos{ floor_div(x, 16), floor_div(z, 16) };
        const auto chunk = result.find(chunk_pos);
        if (chunk == result.end()) continue;
        const auto sub_y = floor_div(y - 64, 16);
        auto [sub, inserted] = chunk->second.sub_chunks.try_emplace(sub_y);
        if (inserted) {
            sub->second.layer0.fill(mRegistry.air_runtime_id());
            sub->second.layer1.fill(mRegistry.air_runtime_id());
        }
        const auto lx = x - chunk_pos.x * 16;
        const auto ly = y - (sub_y * 16 + 64);
        const auto lz = z - chunk_pos.z * 16;
        if (lx < 0 || lx >= 16 || ly < 0 || ly >= 16 || lz < 0 || lz >= 16) {
            return Result<ChunkMap>::failure("稀疏方块坐标 materialize 越界");
        }
        sub->second.layer0[static_cast<std::size_t>((ly * 16 + lz) * 16 + lx)] = runtime_id;
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
