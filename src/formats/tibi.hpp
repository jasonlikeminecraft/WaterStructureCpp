#pragma once

#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>
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
    void set_streaming_options(
        std::size_t soft_memory_budget_bytes,
        bool allow_temporary_spool,
        std::filesystem::path temporary_directory = {},
        std::size_t temporary_file_limit_bytes = 0);
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
    std::size_t mSoftMemoryBudgetBytes = 450u * 1024u * 1024u;
    bool mAllowTemporarySpool = true;
    std::filesystem::path mTemporaryDirectory;
    std::size_t mTemporaryFileLimitBytes = 0;
    bool mStreamingOptionsConfigured = false;
    mutable std::unordered_map<ChunkPos, std::vector<std::uint32_t>, ChunkPosHash>
        mCommandIndex;
    mutable std::vector<std::uint32_t> mBroadCommands;
    mutable std::atomic_bool mCommandIndexReady = false;
    mutable std::mutex mCommandIndexMutex;
};

} // namespace water_structure
