#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace water_structure {

struct BlockPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    auto operator<=>(const BlockPos&) const = default;
};

struct ChunkPos {
    std::int32_t x = 0;
    std::int32_t z = 0;
    auto operator<=>(const ChunkPos&) const = default;
};

struct SubChunkPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    auto operator<=>(const SubChunkPos&) const = default;
};

struct Size {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t length = 0;

    // Avoid `width + 15`: dimensions originate in untrusted files and a
    // near-INT32_MAX value would wrap before the division.  Non-positive
    // dimensions represent an empty/invalid structure and therefore expose
    // no chunk columns.
    std::int32_t chunk_x_count() const noexcept {
        if (width <= 0) return 0;
        return width / 16 + (width % 16 != 0 ? 1 : 0);
    }
    std::int32_t chunk_z_count() const noexcept {
        if (length <= 0) return 0;
        return length / 16 + (length % 16 != 0 ? 1 : 0);
    }
    std::int64_t volume() const noexcept {
        if (width <= 0 || height <= 0 || length <= 0) return 0;
        constexpr auto max_value = std::numeric_limits<std::int64_t>::max();
        const auto w = static_cast<std::int64_t>(width);
        const auto h = static_cast<std::int64_t>(height);
        const auto l = static_cast<std::int64_t>(length);
        if (w > max_value / h) return max_value;
        const auto wh = w * h;
        if (wh > max_value / l) return max_value;
        return wh * l;
    }
};

enum class BlockStateValueType : std::uint8_t {
    Byte,
    Short,
    Int,
    Long,
    String
};

struct BlockStateProperty {
    std::string name;
    BlockStateValueType type = BlockStateValueType::String;
    std::string value;
};

struct BlockState {
    std::string name;
    std::vector<BlockStateProperty> states;
    std::int32_t version = 0;
};

using NbtPayload = std::vector<std::uint8_t>;
using BlockLayer = std::array<std::uint32_t, 4096>;

// Internal structure readers use (y,z,x) indexing.  The Bedrock writer can
// opt into the native (x,y,z) layout when a producer already emits blocks in
// that order, avoiding a full 4096-entry transpose at save time.
enum class BlockLayerLayout : std::uint8_t {
    Internal,
    Native
};

struct SubChunkData {
    BlockLayer layer0{};
    BlockLayer layer1{};
};

struct ChunkData {
    std::unordered_map<std::int32_t, SubChunkData> sub_chunks;
    BlockLayerLayout layout = BlockLayerLayout::Internal;
};

struct BlockEntity {
    BlockPos pos;
    NbtPayload payload;
};

struct BlockBox {
    BlockPos min;
    BlockPos max;
};

struct BlockPosHash {
    std::size_t operator()(const BlockPos& pos) const noexcept {
        std::size_t h = static_cast<std::size_t>(pos.x);
        h = h * 0x9e3779b9u + static_cast<std::size_t>(pos.y);
        h = h * 0x9e3779b9u + static_cast<std::size_t>(pos.z);
        return h;
    }
};

struct ChunkPosHash {
    std::size_t operator()(const ChunkPos& pos) const noexcept {
        return static_cast<std::size_t>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.x)) << 32u) ^
            static_cast<std::uint32_t>(pos.z);
    }
};

struct SubChunkPosHash {
    std::size_t operator()(const SubChunkPos& pos) const noexcept {
        std::size_t h = static_cast<std::size_t>(pos.x);
        h = h * 0x9e3779b9u + static_cast<std::size_t>(pos.y);
        h = h * 0x9e3779b9u + static_cast<std::size_t>(pos.z);
        return h;
    }
};

} // namespace water_structure
