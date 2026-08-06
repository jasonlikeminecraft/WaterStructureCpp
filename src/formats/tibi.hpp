#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace water_structure {

class TibiReader final : public IStructure {
public:
    explicit TibiReader(RuntimeRegistry& registry) : mRegistry(registry) {}

    StructureId id() const noexcept override { return StructureId::TIBI; }
    std::string_view name() const noexcept override { return "TIBI"; }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;
    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override {
        return Result<std::size_t>::success(mNonAirBlocks);
    }
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start,
        ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;

private:
    struct Command {
        std::uint64_t type = 0;
        std::uint32_t runtime_id = 0;
        BlockPos first{};
        BlockPos second{};
    };

    RuntimeRegistry& mRegistry;
    std::vector<Command> mCommands;
    BlockPos mOrigin{};
    BlockPos mOffset{};
    Size mOriginalSize{};
    Size mSize{};
    std::size_t mNonAirBlocks = 0;
};

} // namespace water_structure
