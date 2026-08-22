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
    // Some legacy readers (notably GangBanV3) preserve records just outside
    // the declared header bounds.  Keep the declared Size unchanged while
    // allowing those sparse records to be exposed to chunk consumers.
    void set_include_out_of_bounds(bool enabled) noexcept { mIncludeOutOfBounds = enabled; }
    Size size() const noexcept { return mSize; }
    Size original_size() const noexcept { return mOriginalSize; }
    BlockPos offset() const noexcept { return mOffset; }
    void put(BlockPos local, std::uint32_t runtime_id);
    void put_entity(BlockPos local, NbtPayload payload);
    std::size_t count_non_air() const noexcept { return mNonAirBlocks; }
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const;
    Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const;

private:
    Result<ChunkMap> get_chunks_impl(
        std::span<const ChunkPos> positions,
        bool include_layer1) const;

    RuntimeRegistry& mRegistry;
    Size mOriginalSize{};
    Size mSize{};
    BlockPos mOffset{};
    std::map<BlockPos, std::uint32_t, std::less<>> mBlocks;
    std::map<BlockPos, std::uint32_t, std::less<>> mOutOfBoundsBlocks;
    std::map<BlockPos, NbtPayload, std::less<>> mEntities;
    std::size_t mNonAirBlocks = 0;
    bool mIncludeOutOfBounds = false;
};

} // namespace water_structure
