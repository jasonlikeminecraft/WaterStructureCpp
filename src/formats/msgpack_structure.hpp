#pragma once

#include <WaterStructure/chunk_index.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace water_structure {

class MsgpackStructure final : public IStructure {
public:
    MsgpackStructure(RuntimeRegistry& registry, StructureId format)
        : mRegistry(registry), mFormat(format) {}

    StructureId id() const noexcept override { return mFormat; }
    std::string_view name() const noexcept override;
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;
    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override {
        return Result<std::size_t>::success(mNonAirBlocks);
    }
    Result<void> write_to_world(
        WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    struct Block {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        std::uint32_t runtime = 0;
    };

    Result<ChunkMap> get_chunks_impl(
        std::span<const ChunkPos> positions, bool include_layer1) const;

    RuntimeRegistry& mRegistry;
    StructureId mFormat;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    std::vector<Block> mBlocks;
    mutable ChunkBlockIndex mChunkIndex;
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
