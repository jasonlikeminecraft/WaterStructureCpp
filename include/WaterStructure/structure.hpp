#pragma once

#include "result.hpp"
#include "types.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace water_structure {

enum class StructureId : std::uint8_t {
    Unknown = 0,
    Schematic,
    SchemV1,
    SchemV2,
    Litematic,
    MCStructure,
    MCWorld,
    BDX,
    Construction,
    AxiomBP,
    MCFunction,
    KBDX,
    IBImport,
    MianYangV1,
    MianYangV2,
    MianYangV3,
    MianYangV4,
    GangBanV1,
    GangBanV2,
    GangBanV3,
    GangBanV4,
    GangBanV5,
    GangBanV6,
    GangBanV7,
    RunAway,
    QingXuV1,
    TimeBuilderV1,
    FuHongV1,
    FuHongV2,
    FuHongV3,
    FuHongV4,
    FuHongV5,
    BDS,
    SIBI,
    BCF,
    TIBI,
    CovStructure,
    NexusNP
};

using ChunkMap = std::unordered_map<ChunkPos, ChunkData, ChunkPosHash>;
using NbtChunkMap = std::unordered_map<ChunkPos, std::vector<BlockEntity>, ChunkPosHash>;
using ChunkVisitor = std::function<Result<void>(ChunkPos, const ChunkData&)>;
using ChunkNbtVisitor = std::function<Result<void>(ChunkPos, std::span<const BlockEntity>)>;

// Palette-preserving view of one subchunk. `sub_y` is the structure-local
// subchunk Y (floor_div(y - 64, 16)); `indices` are in Bedrock native (x,y,z)
// order: index = x*256 + y*16 + z. Indices are a moveable vector so the data
// can flow decode -> adapter -> structure -> writer without array copies.
struct SubChunkPaletteData {
    std::int32_t sub_y = 0;
    std::vector<BlockState> palette;
    // Readers with one structure-wide palette can share it across all
    // subchunks instead of deep-copying BlockState strings thousands of times.
    // Consumers use shared_palette when non-null, otherwise palette.
    std::shared_ptr<const std::vector<BlockState>> shared_palette;
    std::vector<std::uint16_t> indices; // 4096 entries, native (x,y,z) order
};

using ChunkPaletteVisitor =
    std::function<Result<void>(ChunkPos, std::span<const SubChunkPaletteData>)>;

struct ConversionCallbacks {
    std::function<void(std::size_t)> start;
    std::function<void()> progress;
    // Optional end-of-conversion snapshot.  It is intentionally one callback
    // rather than per-block telemetry so enabling statistics does not add
    // synchronization to the hot path.
    std::function<void(const struct ConversionStats&)> statistics;
    // World conversion knobs are carried alongside callbacks so existing
    // format-specific write_to_world override signatures remain source
    // compatible when applications and the library are rebuilt together.
    std::size_t worker_count = 0;
    std::size_t max_in_flight_chunks = 0;
    std::size_t soft_memory_budget_bytes = 450u * 1024u * 1024u;
    bool allow_temporary_spool = true;
    bool collect_statistics = false;
    std::filesystem::path temporary_directory{};
    std::size_t temporary_file_limit_bytes = 0;
    bool profiling = false;
};

struct ConversionStats {
    StructureId source_format = StructureId::Unknown;
    StructureId target_format = StructureId::Unknown;
    std::uint64_t detect_ms = 0;
    std::uint64_t parse_decompress_ms = 0;
    std::uint64_t palette_runtime_mapping_ms = 0;
    std::uint64_t chunk_materialization_ms = 0;
    std::uint64_t nbt_entity_decode_ms = 0;
    std::uint64_t encode_compress_ms = 0;
    std::uint64_t leveldb_write_ms = 0;
    std::uint64_t leveldb_close_ms = 0;
    std::uint64_t mcworld_unpack_ms = 0;
    std::uint64_t mcworld_pack_ms = 0;
    std::uint64_t elapsed_ms = 0;
    std::uint64_t compressed_input_bytes = 0;
    std::uint64_t compressed_output_bytes = 0;
    std::uint64_t leveldb_batches = 0;
    std::uint64_t peak_memory_bytes = 0;
    std::uint64_t decoded_blocks = 0;
    std::uint64_t encoded_blocks = 0;
    std::uint64_t command_count = 0;
    std::uint64_t chunk_window_peak = 0;
    std::uint64_t temporary_spool_bytes = 0;
    std::size_t source_chunks = 0;
    std::size_t completed_chunks = 0;
    std::size_t non_air_blocks = 0;
    std::size_t error_offset = 0;
    ChunkPos error_chunk{};
    bool has_error_chunk = false;
    bool success = false;
    std::string error_stage;
    std::string error_location;
};

class WorldSource;
class WorldTarget;

class IStructure {
public:
    virtual ~IStructure() = default;

    virtual StructureId id() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual Size size() const noexcept = 0;
    virtual BlockPos offset() const noexcept = 0;
    virtual void set_offset(BlockPos offset) noexcept = 0;

    virtual Result<void> read(const std::filesystem::path& path) = 0;
    virtual Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const = 0;
    // Schem-like writers only serialize the primary block layer and should call
    // get_chunks_layer0(). General consumers, including world conversion, use
    // get_chunks()/visit_chunks() and receive both layers.
    virtual Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const {
        return get_chunks(positions);
    }
    // Streaming compatibility extension. The default deliberately asks for one
    // chunk at a time, so readers without a specialized override still keep the
    // materialized map bounded to a single chunk.
    virtual Result<void> visit_chunks(
        std::span<const ChunkPos> positions,
        const ChunkVisitor& visitor) const {
        if (!visitor) return Result<void>::failure("chunk visitor is empty");
        for (const auto position : positions) {
            const std::array<ChunkPos, 1> request{ position };
            auto chunks = get_chunks(request);
            if (!chunks) return Result<void>::failure(chunks.error());
            const auto found = chunks.value().find(position);
            if (found == chunks.value().end()) continue;
            auto visited = visitor(position, found->second);
            if (!visited) return visited;
        }
        return Result<void>::success();
    }
    virtual Result<void> visit_chunk_nbt(
        std::span<const ChunkPos> positions,
        const ChunkNbtVisitor& visitor) const {
        if (!visitor) return Result<void>::failure("chunk NBT visitor is empty");
        for (const auto position : positions) {
            const std::array<ChunkPos, 1> request{ position };
            auto entities = get_chunk_nbt(request);
            if (!entities) return Result<void>::failure(entities.error());
            const auto found = entities.value().find(position);
            if (found == entities.value().end()) continue;
            auto visited = visitor(position, found->second);
            if (!visited) return visited;
        }
        return Result<void>::success();
    }
    // Allows streaming consumers to release source-side chunk caches between batches.
    virtual void release_cached_chunks() const noexcept {}
    // Palette streaming extension: readers that can decode subchunk palettes
    // without materializing per-block runtime IDs or BlockState objects (e.g.
    // MCWorld) override this to feed writers directly. States are returned as
    // decoded; consumers apply the block-upgrade schemas once per distinct
    // state. The default reports "unsupported"; writers fall back to
    // get_chunks()/get_chunks_layer0().
    virtual Result<void> visit_chunk_palettes(
        std::span<const ChunkPos> positions,
        const ChunkPaletteVisitor& visitor) const {
        if (!visitor) return Result<void>::failure("chunk palette visitor is empty");
        return Result<void>::failure("此格式不支持 palette 流式读取");
    }
    // Zero lets the writer choose its default. Row-oriented readers may
    // request a wider batch so each encoded row is decoded only once.
    virtual std::size_t preferred_palette_batch_size() const noexcept { return 0; }
    virtual Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const = 0;
    virtual Result<std::size_t> count_non_air_blocks() const = 0;
    virtual Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const = 0;
    virtual Result<void> read_from_world(WorldSource& world, BlockBox box, ConversionCallbacks callbacks) = 0;
};

} // namespace water_structure
