#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace water_structure {

class McFunctionStructure final : public IStructure {
public:
    explicit McFunctionStructure(RuntimeRegistry& registry) : mRegistry(registry) {}

    StructureId id() const noexcept override { return StructureId::MCFunction; }
    std::string_view name() const noexcept override { return "MCFunction"; }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;

    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<void> visit_chunks(
        std::span<const ChunkPos> positions,
        const ChunkVisitor& visitor) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override;
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    Result<void> read_parallel(const std::filesystem::path& path);

    // Keep commands instead of expanding fill cuboids into one entry per
    // block.  A command is tiny and can be replayed against one requested
    // chunk at a time, which keeps MCFunction import memory bounded even for
    // very large fill regions.
    struct Command {
        std::int32_t x1 = 0, x2 = 0;
        std::int32_t y1 = 0, y2 = 0;
        std::int32_t z1 = 0, z2 = 0;
        std::uint32_t runtime_id = 0;
    };
    RuntimeRegistry& mRegistry;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    std::vector<Command> mCommands;
    // Compact CSR index: each chunk owns a contiguous slice in
    // mCommandIndices. This avoids one allocation and hash node per chunk.
    mutable std::vector<std::size_t> mCommandOffsets;
    mutable std::vector<std::uint32_t> mCommandIndices;
    mutable std::vector<std::uint32_t> mBroadCommands;
    mutable std::int32_t mIndexMinX = 0;
    mutable std::int32_t mIndexMinZ = 0;
    mutable std::int32_t mIndexWidth = 0;
    mutable std::int32_t mIndexLength = 0;
    mutable bool mCommandIndexReady = false;
    mutable std::optional<std::size_t> mNonAirBlocks;
};

} // namespace water_structure
