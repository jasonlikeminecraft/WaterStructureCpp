#pragma once

#include "coordinates.hpp"
#include "runtime_registry.hpp"
#include "structure.hpp"

#include <map>
#include <span>

namespace water_structure {

// Shared sparse representation used by command, JSON and private binary readers.
// Coordinates are local to the decoded structure; offset is applied only when
// materializing chunks, which keeps negative-origin handling consistent.
class SparseBlockStore {
public:
    explicit SparseBlockStore(RuntimeRegistry& registry) : mRegistry(registry) {}

    void clear();
    void set_size(Size size) noexcept { mOriginalSize = size; mSize = size; }
    void set_offset(BlockPos offset) noexcept;
    Size size() const noexcept { return mSize; }
    Size original_size() const noexcept { return mOriginalSize; }
    BlockPos offset() const noexcept { return mOffset; }
    void put(BlockPos local, std::uint32_t runtime_id);
    void put_entity(BlockPos local, NbtPayload payload);
    std::size_t count_non_air() const noexcept { return mNonAirBlocks; }
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const;

private:
    RuntimeRegistry& mRegistry;
    Size mOriginalSize{};
    Size mSize{};
    BlockPos mOffset{};
    std::map<BlockPos, std::uint32_t, std::less<>> mBlocks;
    std::map<BlockPos, NbtPayload, std::less<>> mEntities;
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
