#pragma once

#include "result.hpp"
#include "structure.hpp"

#include <BedrockWorldOperator/BedrockWorldOperator.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace water_structure {

struct WorldConversionOptions;
class BdxStructure;

namespace detail {
class BoundedThreadPool;
struct BoundedThreadPoolDeleter {
    void operator()(BoundedThreadPool*) const noexcept;
};
}

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

// Adapter-side stage counters. Encoding is CPU work (possibly parallel),
// whereas LevelDB and archive commits are serialized; keeping them separate
// prevents save time from being reported as one opaque I/O bucket.
struct WorldIoStats {
    std::uint64_t encode_compress_ms = 0;
    std::uint64_t leveldb_write_ms = 0;
    std::uint64_t leveldb_close_ms = 0;
    std::uint64_t mcworld_unpack_ms = 0;
    std::uint64_t mcworld_pack_ms = 0;
    std::uint64_t compressed_output_bytes = 0;
    std::uint64_t leveldb_batches = 0;
    std::uint64_t temporary_spool_bytes = 0;
};

struct WorldOpenOptions {
    bool write_back_archive = true;
    bool allow_temporary_spool = true;
    std::filesystem::path temporary_directory{};
    // Zero is unlimited. For archive inputs this limits the declared total
    // uncompressed size; for outputs it limits both the temporary world data
    // and the generated archive before the atomic replace.
    std::size_t temporary_file_limit_bytes = 0;
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
    static Result<BedrockWorldAdapter> open(
        const std::filesystem::path& directory,
        const WorldOpenOptions& options);

    BedrockWorldAdapter() = default;
    BedrockWorldAdapter(BedrockWorldAdapter&&) noexcept = default;
    BedrockWorldAdapter& operator=(BedrockWorldAdapter&&) noexcept = default;
    BedrockWorldAdapter(const BedrockWorldAdapter&) = delete;
    BedrockWorldAdapter& operator=(const BedrockWorldAdapter&) = delete;
    ~BedrockWorldAdapter() override;

    // Call close() explicitly when a statistics callback was supplied. The
    // destructor still closes and packs resources, but deliberately does not
    // invoke user code because captures referenced by the callback may already
    // have been destroyed during stack unwinding.
    Result<void> close();
    // Close the LevelDB handle and remove only adapter-owned temporary files
    // without creating/replacing the destination archive. Use this after a
    // failed conversion so a partial .mcworld cannot overwrite a valid one.
    Result<void> discard();
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
    void configure_conversion(const WorldConversionOptions& options) noexcept;
    // Transfer and reset counters without adding a virtual to WorldTarget;
    // third-party WorldTarget implementations therefore keep their ABI.
    WorldIoStats take_io_stats() noexcept;

private:
    friend class BdxStructure;
    friend Result<void> convert_to_world(
        const IStructure& structure,
        WorldTarget& world,
        SubChunkPos start,
        const WorldConversionOptions& options);

    void defer_statistics(
        ConversionStats stats,
        std::function<void(const ConversionStats&)> callback) noexcept;
    void emit_deferred_statistics(bool success, std::string_view error_stage = {}) noexcept;
    Result<void> cleanup_temporary_artifacts();

    std::filesystem::path mDirectory;
    std::filesystem::path mArchivePath;
    std::filesystem::path mTemporaryDirectory;
    std::filesystem::path mPendingArchivePath;
    bool mWriteBackArchive = true;
    bool mDiscardOnClose = false;
    std::size_t mTemporaryFileLimitBytes = 0;
    std::optional<BedrockWorldOperator::World> mWorld;
    // Lazily created so repeated save_chunks() calls (for example one Schem
    // Z stripe at a time) reuse the workers instead of creating threads for
    // every batch. The pool only performs CPU-side payload encoding; LevelDB
    // commits remain on the caller thread.
    mutable std::unique_ptr<detail::BoundedThreadPool, detail::BoundedThreadPoolDeleter> mEncodePool;
    mutable std::size_t mEncodePoolWorkers = 0;
    mutable std::size_t mConfiguredWorkerCount = 0;
    mutable std::size_t mConfiguredMaxInFlightChunks = 0;
    mutable std::size_t mConfiguredSoftMemoryBudget = 450u * 1024u * 1024u;
    mutable WorldIoStats mIoStats{};
    std::optional<ConversionStats> mDeferredStats;
    std::function<void(const ConversionStats&)> mDeferredStatisticsCallback;
};

struct WorldConversionOptions {
    std::size_t worker_count = 0;
    std::size_t max_in_flight_chunks = 0;
    std::size_t soft_memory_budget_bytes = 450u * 1024u * 1024u;
    bool allow_temporary_spool = true;
    bool collect_statistics = false;
    std::filesystem::path temporary_directory{};
    std::size_t temporary_file_limit_bytes = 0;
    bool profiling = false;
    ConversionCallbacks callbacks{};
};

Result<void> convert_to_world(const IStructure& structure, WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks = {});
Result<void> convert_to_world(
    const IStructure& structure,
    WorldTarget& world,
    SubChunkPos start,
    const WorldConversionOptions& options);

} // namespace water_structure
