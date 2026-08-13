#pragma once

#include <WaterStructure/chunk_index.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/structure.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace water_structure {

class BdxStructure final : public IStructure {
public:
    explicit BdxStructure(RuntimeRegistry& registry) : mRegistry(registry) {}
    StructureId id() const noexcept override { return StructureId::BDX; }
    std::string_view name() const noexcept override { return "BDX"; }
    Size size() const noexcept override { return mSize; }
    BlockPos offset() const noexcept override { return mOffset; }
    void set_offset(BlockPos offset) noexcept override;
    Result<void> read(const std::filesystem::path& path) override;
    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override;
    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override;
    Result<std::size_t> count_non_air_blocks() const override { return Result<std::size_t>::success(mNonAirBlocks); }
    Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const override;
    Result<void> read_from_world(WorldSource&, BlockBox, ConversionCallbacks) override;
    const std::string& author() const noexcept { return mAuthor; }
    void set_streaming_world_import(bool enabled) noexcept { mBoundsOnly = enabled; }

private:
    struct ZRunConsumerRef {
        using Callback = void (*)(
            void*,
            BlockPos,
            std::span<const std::uint32_t>,
            bool);

        void* context = nullptr;
        Callback callback = nullptr;

        explicit operator bool() const noexcept { return callback != nullptr; }

        void operator()(
            BlockPos position,
            std::span<const std::uint32_t> runtime_ids,
            bool contains_air) const
        {
            callback(context, position, runtime_ids, contains_air);
        }
    };

    Result<void> ensure_blocks_loaded() const;

    struct Block { std::int32_t x = 0, y = 0, z = 0; std::uint32_t runtime_id = 0; };
    RuntimeRegistry& mRegistry;
    Size mSize{}, mOriginalSize{};
    BlockPos mOffset{};
    BlockPos mMin{};
    std::filesystem::path mSourcePath;
    mutable std::vector<Block> mBlocks;
    mutable ChunkBlockIndex mChunkIndex;
    std::unordered_map<BlockPos, NbtPayload, BlockPosHash> mBlockEntities;
    std::size_t mNonAirBlocks = 0;
    std::string mAuthor;
    std::uint8_t mPoolId = 117;
    mutable bool mBlocksLoaded = false;
    bool mMaterializeBlocks = false;
    bool mCaptureEntities = true;
    bool mBoundsOnly = false;
    std::function<void(BlockPos, std::uint32_t)> mBlockConsumer;
    ZRunConsumerRef mZRunConsumer;
};

} // namespace water_structure
