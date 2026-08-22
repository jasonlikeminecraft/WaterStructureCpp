#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <vector>

namespace water_structure {

class SchematicStructure final : public IStructure {
public:
    explicit SchematicStructure(RuntimeRegistry& registry) : mRegistry(registry) {}

    StructureId id() const noexcept override { return StructureId::Schematic; }
    std::string_view name() const noexcept override { return "Schematic"; }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;

    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override;
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    Result<ChunkMap> get_chunks_impl(
        std::span<const ChunkPos> positions,
        bool include_layer1) const;

    RuntimeRegistry& mRegistry;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    std::vector<std::int8_t> mBlocks;
    std::vector<std::int8_t> mData;
    // A legacy id/data pair has only 65,536 possible values.  Keep its
    // resolved runtime id on the heap for the lifetime of the reader so
    // bounded chunk batches do not rebuild the mapping table repeatedly.
    std::vector<std::uint32_t> mRuntimeCache;
    std::vector<std::uint8_t> mRuntimeCacheState;
};

} // namespace water_structure
