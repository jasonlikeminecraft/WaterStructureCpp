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
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override;
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    RuntimeRegistry& mRegistry;
    Size mSize{};
    Size mOriginalSize{};
    BlockPos mOffset{};
    std::vector<std::int8_t> mBlocks;
    std::vector<std::int8_t> mData;
};

} // namespace water_structure
