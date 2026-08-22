#pragma once

#include <WaterStructure/decoded_structure.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace water_structure {

struct ConstructionReaderState {
    struct SectionIndex {
        std::int32_t start_x = 0;
        std::int32_t start_y = 0;
        std::int32_t start_z = 0;
        std::uint8_t shape_x = 0;
        std::uint8_t shape_y = 0;
        std::uint8_t shape_z = 0;
        std::int32_t position = 0;
        std::int32_t length = 0;
    };

    std::filesystem::path source_path;
    Size original_size{};
    BlockPos minimum{};
    std::vector<SectionIndex> sections;
    std::vector<std::uint32_t> palette;
    std::uint32_t unknown_runtime = 0;
};

class ConstructionReader final : public IStructure {
public:
    explicit ConstructionReader(RuntimeRegistry& registry) : mRegistry(registry), mStore(registry) {}

    StructureId id() const noexcept override { return StructureId::Construction; }
    std::string_view name() const noexcept override { return "Construction"; }
    Size size() const noexcept override { return mStore.size(); }
    BlockPos offset() const noexcept override { return mStore.offset(); }
    void set_offset(BlockPos offset) noexcept override { mStore.set_offset(offset); }
    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const override;
    Result<void> visit_chunks(
        std::span<const ChunkPos> positions, const ChunkVisitor& visitor) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override {
        return mStore.get_chunk_nbt(positions);
    }
    Result<std::size_t> count_non_air_blocks() const override {
        return Result<std::size_t>::success(mNonAirBlocks);
    }
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start,
        ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    Result<ChunkMap> materialize_chunks(
        std::span<const ChunkPos> positions, bool include_layer1) const;

    RuntimeRegistry& mRegistry;
    SparseBlockStore mStore;
    ConstructionReaderState mState;
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
