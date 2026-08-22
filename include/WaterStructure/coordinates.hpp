#pragma once

#include "types.hpp"

#include <cstdint>

namespace water_structure {

constexpr std::int32_t kOverworldMinY = -64;

constexpr std::int32_t floor_div(std::int32_t value, std::int32_t divisor) noexcept
{
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return quotient - (remainder < 0 ? 1 : 0);
}

constexpr std::int32_t floor_mod(std::int32_t value, std::int32_t divisor) noexcept
{
    return value - floor_div(value, divisor) * divisor;
}

constexpr std::int64_t floor_div64(std::int64_t value, std::int64_t divisor) noexcept
{
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return quotient - (remainder != 0 && ((remainder < 0) != (divisor < 0)) ? 1 : 0);
}

constexpr std::int64_t floor_mod64(std::int64_t value, std::int64_t divisor) noexcept
{
    return value - floor_div64(value, divisor) * divisor;
}

constexpr ChunkPos block_to_chunk(BlockPos pos) noexcept
{
    return { floor_div(pos.x, 16), floor_div(pos.z, 16) };
}

constexpr std::int32_t world_y_to_subchunk(std::int32_t y) noexcept
{
    return floor_div(y, 16);
}

constexpr std::int32_t structure_y_to_chunk_local(std::int32_t y) noexcept
{
    return y + kOverworldMinY;
}

} // namespace water_structure
