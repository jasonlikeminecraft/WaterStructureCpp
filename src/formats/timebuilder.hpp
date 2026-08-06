#pragma once

#include <WaterStructure/decoded_structure.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace water_structure {

class TimeBuilderStructure final : public IStructure {
public:
    explicit TimeBuilderStructure(RuntimeRegistry& registry) : mRegistry(registry), mStore(registry) {}

    StructureId id() const noexcept override { return StructureId::TimeBuilderV1; }
    std::string_view name() const noexcept override { return "TimeBuilderV1"; }
    Size size() const noexcept override { return mStore.size(); }
    BlockPos offset() const noexcept override { return mStore.offset(); }
    void set_offset(BlockPos offset) noexcept override { mStore.set_offset(offset); }
    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override {
        return mStore.get_chunks(positions);
    }
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override {
        return mStore.get_chunk_nbt(positions);
    }
    Result<std::size_t> count_non_air_blocks() const override {
        return Result<std::size_t>::success(mNonAirBlocks);
    }
    Result<void> write_to_world(
        WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    std::uint32_t runtime_id(std::string name, std::int64_t aux);

    RuntimeRegistry& mRegistry;
    SparseBlockStore mStore;
    std::unordered_map<std::string, std::uint32_t> mPaletteCache;
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
