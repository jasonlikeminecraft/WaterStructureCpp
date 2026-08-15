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

struct ChunkWrite {
    ChunkPos pos{};
    const ChunkData* chunk = nullptr;
};

struct EncodedSubChunkData {
    SubChunkPos pos{};
    std::vector<std::uint8_t> payload;
};

// Palette-preserving view of one subchunk. `indices` are in Bedrock native
// (x,y,z) order: index = x*256 + y*16 + z. Consumers format each palette
// entry once and scan the indices instead of expanding 4096 BlockStates.
struct DecodedStateSubChunk {
    std::vector<BlockState> palette;
    std::vector<std::uint16_t> indices; // 4096 entries, native (x,y,z) order
};

class WorldTarget {
public:
    virtual ~WorldTarget() = default;
    virtual Result<void> save_chunk(ChunkPos pos, const ChunkData& chunk) = 0;
    virtual Result<void> save_chunks(std::span<const ChunkWrite> chunks) {
        for (const auto& write : chunks) {
            if (!write.chunk) return Result<void>::failure("chunk write is empty");
            auto saved = save_chunk(write.pos, *write.chunk);
            if (!saved) return saved;
        }
        return Result<void>::success();
    }
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
    Result<std::optional<BlockBox>> stored_block_bounds() const;
    Result<std::vector<BlockEntity>> load_chunk_nbt(ChunkPos pos) const override;
    Result<std::vector<EncodedSubChunkData>> encode_chunks(
        std::span<const ChunkWrite> chunks) const;
    Result<SubChunkData> decode_subchunk_payload(
        std::span<const std::uint8_t> payload) const;
    Result<std::optional<std::vector<std::uint8_t>>> load_subchunk_payload(
        SubChunkPos pos) const;
    // Returns the palette plus packed indices for one subchunk, or nullopt
    // when the subchunk is absent from the world. No per-block BlockState or
    // runtime-ID expansion is performed.
    Result<std::optional<DecodedStateSubChunk>> load_subchunk_palette(
        SubChunkPos pos) const;
    // Batch variant: loads all stored subchunk palettes of one chunk in a
    // single call, indexed by (sub_y - min_sub_y); nullopt slots are absent.
    Result<std::vector<std::optional<DecodedStateSubChunk>>> load_chunk_palettes(
        ChunkPos pos,
        std::int32_t min_sub_y,
        std::int32_t max_sub_y) const;
    Result<void> save_subchunk_payloads(
        std::vector<EncodedSubChunkData> subchunks);
    Result<void> save_chunk(ChunkPos pos, const ChunkData& chunk) override;
    Result<void> save_chunks(std::span<const ChunkWrite> chunks) override;
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
