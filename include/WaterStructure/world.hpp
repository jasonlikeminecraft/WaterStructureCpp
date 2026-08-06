#pragma once

#include "result.hpp"
#include "structure.hpp"

#include <BedrockWorldOperator/BedrockWorldOperator.hpp>

#include <filesystem>
#include <optional>

namespace water_structure {

class WorldSource {
public:
    virtual ~WorldSource() = default;
    virtual Result<ChunkData> load_chunk(ChunkPos pos) const = 0;
    virtual Result<std::vector<BlockEntity>> load_chunk_nbt(ChunkPos pos) const = 0;
};

class WorldTarget {
public:
    virtual ~WorldTarget() = default;
    virtual Result<void> save_chunk(ChunkPos pos, const ChunkData& chunk) = 0;
    virtual Result<void> save_chunk_nbt(ChunkPos pos, std::span<const BlockEntity> entities) = 0;
};

class BedrockWorldAdapter final : public WorldSource, public WorldTarget {
public:
    static Result<BedrockWorldAdapter> open(const std::filesystem::path& directory, bool write_back_archive = true);

    BedrockWorldAdapter() = default;
    BedrockWorldAdapter(BedrockWorldAdapter&&) noexcept = default;
    BedrockWorldAdapter& operator=(BedrockWorldAdapter&&) noexcept = default;
    BedrockWorldAdapter(const BedrockWorldAdapter&) = delete;
    BedrockWorldAdapter& operator=(const BedrockWorldAdapter&) = delete;
    ~BedrockWorldAdapter() override;

    Result<void> close();
    bool valid() const noexcept;
    const std::filesystem::path& directory() const noexcept { return mDirectory; }

    Result<ChunkData> load_chunk(ChunkPos pos) const override;
    Result<ChunkData> load_chunk_range(
        ChunkPos pos,
        std::int32_t min_sub_y,
        std::int32_t max_sub_y,
        bool include_layer1 = true) const;
    Result<std::vector<BlockEntity>> load_chunk_nbt(ChunkPos pos) const override;
    Result<void> save_chunk(ChunkPos pos, const ChunkData& chunk) override;
    Result<void> save_chunk_nbt(ChunkPos pos, std::span<const BlockEntity> entities) override;

private:
    std::filesystem::path mDirectory;
    std::filesystem::path mArchivePath;
    std::filesystem::path mTemporaryDirectory;
    bool mWriteBackArchive = true;
    std::optional<BedrockWorldOperator::World> mWorld;
};

Result<void> convert_to_world(const IStructure& structure, WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks = {});

} // namespace water_structure
