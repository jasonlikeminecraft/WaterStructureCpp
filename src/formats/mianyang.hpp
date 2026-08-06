#pragma once

#include <WaterStructure/decoded_structure.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>

namespace water_structure {

class MianYangStructure final : public IStructure {
public:
    MianYangStructure(RuntimeRegistry& registry, StructureId version)
        : mRegistry(registry), mStore(registry), mVersion(version) {}

    StructureId id() const noexcept override { return mVersion; }
    std::string_view name() const noexcept override;
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
    Result<void> read_json(const std::filesystem::path& path, bool compressed);
    Result<void> read_binary_v4(const std::filesystem::path& path);
    std::uint32_t runtime_id(std::string_view name, std::uint16_t data);

    RuntimeRegistry& mRegistry;
    SparseBlockStore mStore;
    StructureId mVersion;
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
