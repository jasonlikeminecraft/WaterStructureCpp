#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
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

    std::int32_t chunk_x_count() const noexcept { return (width + 15) / 16; }
    std::int32_t chunk_z_count() const noexcept { return (length + 15) / 16; }
    std::int64_t volume() const noexcept {
        return static_cast<std::int64_t>(width) * height * length;
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

struct SubChunkData {
    BlockLayer layer0{};
    BlockLayer layer1{};
};

struct ChunkData {
    std::unordered_map<std::int32_t, SubChunkData> sub_chunks;
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
