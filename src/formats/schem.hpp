#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace water_structure {

class SchemStructure final : public IStructure {
public:
    SchemStructure(RuntimeRegistry& registry, StructureId format) : mRegistry(registry), mFormat(format) {}
    ~SchemStructure() override;

    StructureId id() const noexcept override { return mFormat; }
    std::string_view name() const noexcept override {
        return mFormat == StructureId::SchemV1 ? "SchemV1" : "SchemV2";
    }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;

    Result<void> read(const std::filesystem::path& path) override;
    // World-import callers consume BlockData sequentially and do not need the
    // random-access row/checkpoint index.  Allow that path to defer the
    // validation/index pass so opening a large Schem does not scan it twice.
    void set_streaming_world_import(bool enabled) noexcept { mStreamingWorldImport = enabled; }
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<void> visit_chunks(std::span<const ChunkPos> positions, const ChunkVisitor& visitor) const override;
    Result<void> visit_chunk_palettes(
        std::span<const ChunkPos> positions,
        const ChunkPaletteVisitor& visitor) const override;
    std::size_t preferred_palette_batch_size() const noexcept override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override;
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    void release_block_data() noexcept;
    RuntimeRegistry& mRegistry;
    StructureId mFormat;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    std::filesystem::path mBlockDataPath;
    std::vector<std::uint64_t> mRowOffsets;
    std::vector<std::uint16_t> mChunkOffsets;
    std::size_t mBlockCount = 0;
    std::size_t mNonAirCount = 0;
    std::size_t mBlockDataBytes = 0;
    std::size_t mChunkOffsetCount = 0;
    std::uint32_t mMaxPaletteIndex = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> mPalette;
    std::vector<std::uint32_t> mDensePalette;
    mutable std::shared_ptr<const std::vector<BlockState>> mSharedPaletteStates;
    mutable std::vector<std::uint8_t> mSharedAirPaletteFlags;
    mutable std::uint16_t mSharedAirPaletteIndex = 0;
    std::uint32_t mUnknownRuntimeId = 0;
    bool mNonAirCountValid = false;
    bool mStreamingWorldImport = false;
};

} // namespace water_structure
