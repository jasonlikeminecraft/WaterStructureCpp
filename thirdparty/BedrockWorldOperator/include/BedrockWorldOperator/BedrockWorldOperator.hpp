#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace BedrockWorldOperator {

using Bytes = std::vector<std::uint8_t>;
using BlockRuntimeList = std::vector<std::uint32_t>;

template <class T>
struct Result {
    bool ok = false;
    T value{};
    std::string error;

    explicit operator bool() const noexcept { return ok; }

    static Result success(T nextValue)
    {
        Result result;
        result.ok = true;
        result.value = std::move(nextValue);
        return result;
    }

    static Result failure(std::string message)
    {
        Result result;
        result.error = std::move(message);
        return result;
    }
};

template <>
struct Result<void> {
    bool ok = false;
    std::string error;

    explicit operator bool() const noexcept { return ok; }

    static Result success()
    {
        Result result;
        result.ok = true;
        return result;
    }

    static Result failure(std::string message)
    {
        Result result;
        result.error = std::move(message);
        return result;
    }
};

enum class Encoding {
    Network,
    Disk
};

enum class Dimension {
    Overworld = 0,
    Nether = 1,
    TheEnd = 2
};

struct ChunkPos {
    int x = 0;
    int z = 0;
};

struct SubChunkPos {
    int x = 0;
    int y = 0;
    int z = 0;
};

enum class BlockStateValueType {
    Byte,
    Short,
    Int,
    Long,
    String
};

struct BlockStateProperty {
    std::string name;
    BlockStateValueType type = BlockStateValueType::Int;
    std::string stringValue;
    std::int64_t intValue = 0;
};

struct BlockState {
    std::string name;
    std::vector<BlockStateProperty> states;
    std::int32_t version = 0;
    std::uint16_t paletteIndex = 0; // index into this subchunk's palette (unique per block type within a subchunk)
};

using BlockStateList = std::vector<BlockState>;

struct BlockRuntimeResolver {
    std::function<std::optional<std::string>(std::uint32_t runtimeId)> runtimeIdToName;
    std::function<std::optional<BlockState>(std::uint32_t runtimeId)> runtimeIdToState;
    std::function<std::optional<std::uint32_t>(std::string_view name)> nameToRuntimeId;
    std::function<std::uint32_t()> airRuntimeId;
    std::function<std::optional<std::uint32_t>(const BlockState& state)> stateToRuntimeId;
};

void setBlockRuntimeResolver(BlockRuntimeResolver resolver);
std::uint32_t airRuntimeId();
std::optional<std::string> runtimeIdToName(std::uint32_t runtimeId);
std::optional<std::uint32_t> nameToRuntimeId(std::string_view name);

class SubChunk {
public:
    SubChunk();
    SubChunk(SubChunk&&) noexcept = default;
    SubChunk& operator=(SubChunk&&) noexcept = default;
    SubChunk(const SubChunk&) = delete;
    SubChunk& operator=(const SubChunk&) = delete;
    ~SubChunk();

    static SubChunk createAirFilled();

    bool valid() const noexcept;
    BlockRuntimeList blocks(int layer = 0) const;
    std::span<const std::uint32_t> blocksView(int layer = 0) const noexcept;
    Result<void> setBlocks(std::span<const std::uint32_t> blocks, int layer = 0);

private:
    struct Impl;
    explicit SubChunk(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> mImpl;

    friend class World;
    friend class Chunk;
    friend struct DecodedSubChunk;
    friend Result<struct DecodedSubChunk> decodeSubChunkPayload(std::span<const std::uint8_t> payload, Encoding encoding, int rangeStart, int rangeEnd);
    friend Result<Bytes> encodeSubChunkPayload(const SubChunk& subChunk, Encoding encoding, int rangeStart, int rangeEnd, int index);
};

struct PositionedSubChunkWrite {
    SubChunkPos position;
    std::optional<SubChunk> subChunk;
};

struct PositionedSubChunkPayload {
    SubChunkPos position;
    std::optional<Bytes> payload;
};

class Chunk {
public:
    Chunk();
    Chunk(Chunk&&) noexcept = default;
    Chunk& operator=(Chunk&&) noexcept = default;
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    ~Chunk();

private:
    struct Impl;
    explicit Chunk(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> mImpl;
};

struct DecodedSubChunk;

struct DecodedSubChunk {
    SubChunk subChunk;
    int index = 0;
};

struct SubChunkDecodeProfile {
    std::uint64_t calls = 0;
    std::uint64_t sampledCalls = 0;
    std::uint64_t sampledLayers = 0;
    std::uint64_t sampledPaletteEntries = 0;
    std::uint64_t payloadCopyNs = 0;
    std::uint64_t nativeInitNs = 0;
    std::uint64_t packedReadNs = 0;
    std::uint64_t paletteResolveNs = 0;
    std::uint64_t blockExpandNs = 0;
    std::uint64_t setBlocksNs = 0;
    std::uint64_t wrapperNs = 0;
};

struct SubChunkEncodeProfile {
    std::uint64_t calls = 0;
    std::uint64_t sampledCalls = 0;
    std::uint64_t sampledLayers = 0;
    std::uint64_t sampledPaletteEntries = 0;
    std::uint64_t paletteBuildNs = 0;
    std::uint64_t indexPackNs = 0;
    std::uint64_t packedWriteNs = 0;
    std::uint64_t paletteWriteNs = 0;
};

class World {
public:
    World();
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World();

    static Result<World> open(const std::filesystem::path& dir);

    bool valid() const noexcept;
    Result<void> close();
    Result<Bytes> levelDat() const;
    Result<std::optional<Bytes>> loadSubChunkPayload(Dimension dim, SubChunkPos pos) const;
    Result<SubChunk> loadSubChunk(Dimension dim, SubChunkPos pos) const;
    Result<std::vector<DecodedSubChunk>> loadSubChunks(Dimension dim, ChunkPos chunk, int minSubY, int maxSubY) const;
    Result<BlockStateList> loadSubChunkBlockStates(Dimension dim, SubChunkPos pos) const;
    Result<std::vector<SubChunkPos>> listSubChunks(Dimension dim) const;
    Result<void> saveSubChunk(Dimension dim, SubChunkPos pos, const SubChunk& subChunk);
    Result<void> saveSubChunksBatch(Dimension dim, std::span<const PositionedSubChunkWrite> writes);
    Result<void> saveSubChunkPayloadsBatch(Dimension dim, std::span<const PositionedSubChunkPayload> writes);
    Result<Bytes> loadNbt(Dimension dim, ChunkPos pos) const;
    Result<std::vector<ChunkPos>> listNbtChunks(Dimension dim) const;
    Result<void> saveNbt(Dimension dim, ChunkPos pos, std::span<const std::uint8_t> payload);
    Result<Bytes> loadBiomes(Dimension dim, ChunkPos pos) const;
    Result<void> saveBiomes(Dimension dim, ChunkPos pos, std::span<const std::uint8_t> payload);

private:
    struct Impl;
    explicit World(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> mImpl;
};

Result<DecodedSubChunk> decodeSubChunkPayload(std::span<const std::uint8_t> payload, Encoding encoding, int rangeStart, int rangeEnd);
Result<Bytes> encodeSubChunkPayload(const SubChunk& subChunk, Encoding encoding, int rangeStart, int rangeEnd, int index);
void resetSubChunkDecodeProfile() noexcept;
SubChunkDecodeProfile subChunkDecodeProfile() noexcept;
void resetSubChunkEncodeProfile() noexcept;
SubChunkEncodeProfile subChunkEncodeProfile() noexcept;

} // namespace BedrockWorldOperator
