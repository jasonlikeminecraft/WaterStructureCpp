#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <array>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace water_structure {

class KbdxStructure final : public IStructure {
public:
    explicit KbdxStructure(RuntimeRegistry& registry) : mRegistry(registry) {}

    StructureId id() const noexcept override { return StructureId::KBDX; }
    std::string_view name() const noexcept override { return "KBDX"; }
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
    struct Block {
        int x = 0;
        int y = 0;
        int z = 0;
        std::uint32_t runtime_id = 0;
    };

    RuntimeRegistry& mRegistry;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    std::vector<Block> mBlocks;
    std::unordered_map<BlockPos, NbtPayload, BlockPosHash> mBlockEntities;
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
