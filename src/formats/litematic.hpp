#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <vector>

namespace water_structure {

class LitematicStructure final : public IStructure {
public:
    explicit LitematicStructure(RuntimeRegistry& registry) : mRegistry(registry) {}

    StructureId id() const noexcept override { return StructureId::Litematic; }
    std::string_view name() const noexcept override { return "Litematic"; }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;

    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override;
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    RuntimeRegistry& mRegistry;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    BlockPos mRegionOrigin{};
    std::vector<std::uint32_t> mPalette;
    std::vector<std::uint64_t> mPackedStates;
    std::vector<BlockEntity> mBlockEntities;
    std::uint8_t mBitsPerBlock = 2;
    std::size_t mNonAirBlocks = 0;

    std::uint32_t palette_index_at(std::size_t index) const noexcept;
};

} // namespace water_structure
