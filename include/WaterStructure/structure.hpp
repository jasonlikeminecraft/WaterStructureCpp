#pragma once

#include "result.hpp"
#include "types.hpp"

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

struct ConversionCallbacks {
    std::function<void(std::size_t)> start;
    std::function<void()> progress;
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
    // Schem-like writers only serialize the primary block layer. Other formats
    // keep the full get_chunks behavior through this default implementation;
    // for an optimized override, layer1 is unspecified and must not be consumed.
    virtual Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const {
        return get_chunks(positions);
    }
    // Allows streaming consumers to release source-side chunk caches between batches.
    virtual void release_cached_chunks() const noexcept {}
    virtual Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const = 0;
    virtual Result<std::size_t> count_non_air_blocks() const = 0;
    virtual Result<void> write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const = 0;
    virtual Result<void> read_from_world(WorldSource& world, BlockBox box, ConversionCallbacks callbacks) = 0;
};

} // namespace water_structure
