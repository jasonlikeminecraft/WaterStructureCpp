#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>
#include <WaterStructure/world.hpp>

#include <filesystem>
#include <optional>
#include <unordered_map>

namespace water_structure {

class McWorldStructure final : public IStructure {
public:
    explicit McWorldStructure(RuntimeRegistry& registry) : mRegistry(registry) {}

    StructureId id() const noexcept override { return StructureId::MCWorld; }
    std::string_view name() const noexcept override { return "MCWorld"; }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;

    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const override;
    Result<void> visit_chunks(std::span<const ChunkPos> positions, const ChunkVisitor& visitor) const override;
    Result<void> visit_chunk_palettes(
        std::span<const ChunkPos> positions,
        const ChunkPaletteVisitor& visitor) const override;
    void release_cached_chunks() const noexcept override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override;
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    Result<ChunkMap> get_chunks_impl(
        std::span<const ChunkPos> positions,
        bool include_layer1) const;
    Result<const ChunkData*> source_chunk(ChunkPos pos, bool include_layer1) const;

    RuntimeRegistry& mRegistry;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    BlockPos mMin{};
    BlockPos mMax{};
    std::optional<BedrockWorldAdapter> mWorld;
    mutable std::unordered_map<ChunkPos, ChunkData, ChunkPosHash> mChunkCache;
    mutable bool mChunkCacheHasLayer1 = false;
};

} // namespace water_structure
