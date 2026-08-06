#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace water_structure {

class McStructure final : public IStructure {
public:
    explicit McStructure(RuntimeRegistry& registry) : mRegistry(registry) {}

    StructureId id() const noexcept override { return StructureId::MCStructure; }
    std::string_view name() const noexcept override { return "MCStructure"; }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;

    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override { return Result<std::size_t>::success(mNonAirBlocks); }
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    RuntimeRegistry& mRegistry;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    std::vector<std::int32_t> mPrimaryIndices;
    std::vector<std::int32_t> mSecondaryIndices;
    std::vector<std::uint32_t> mPalette;
    std::unordered_map<std::int32_t, NbtPayload> mBlockEntities;
    std::uint32_t mUnknownRuntimeId = 0;
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
