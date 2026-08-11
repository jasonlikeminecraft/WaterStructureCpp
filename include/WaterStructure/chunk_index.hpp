#pragma once

#include "coordinates.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace water_structure {

// Lazily groups a vector-backed reader's blocks by their world chunk. The
// source vectors stay in their native order; only indices are stored here.
class ChunkBlockIndex {
public:
    void clear() noexcept
    {
        mBuckets.clear();
        mReady = false;
    }

    template <typename BlockRange, typename PositionFn>
    void ensure(const BlockRange& blocks, BlockPos offset, PositionFn position) const
    {
        if (mReady) return;
        mBuckets.clear();
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto local = position(blocks[index]);
            const auto world = BlockPos{
                local.x + offset.x,
                local.y + offset.y,
                local.z + offset.z
            };
            mBuckets[block_to_chunk(world)].push_back(index);
        }
        mReady = true;
    }

    const std::vector<std::size_t>* find(ChunkPos position) const noexcept
    {
        const auto found = mBuckets.find(position);
        return found == mBuckets.end() ? nullptr : &found->second;
    }

private:
    mutable bool mReady = false;
    mutable std::unordered_map<ChunkPos, std::vector<std::size_t>, ChunkPosHash> mBuckets;
};

} // namespace water_structure
