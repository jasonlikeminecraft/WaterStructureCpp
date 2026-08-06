#include "mcworld.hpp"

#include <WaterStructure/coordinates.hpp>

#include <io/stream_reader.h>
#include <tag_compound.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <regex>

namespace water_structure {

namespace {

std::optional<std::pair<BlockPos, BlockPos>> selection_from_text(const std::string& text)
{
    static const std::regex pattern(
        R"(@\[(-?\d+),(-?\d+),(-?\d+)\]~\[(-?\d+),(-?\d+),(-?\d+)\])");
    std::smatch match;
    if (!std::regex_search(text, match, pattern) || match.size() != 7) return std::nullopt;
    try {
        return std::pair{
            BlockPos{ std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str()) },
            BlockPos{ std::stoi(match[4].str()), std::stoi(match[5].str()), std::stoi(match[6].str()) }
        };
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::pair<BlockPos, BlockPos>> selection_from_level_dat(const std::filesystem::path& path)
{
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        std::array<char, 8> header{};
        input.read(header.data(), header.size());
        if (!input) return std::nullopt;
        const auto [root_name, root] = nbt::io::read_compound(input, endian::little);
        if (!root->has_key("LevelName") || root->at("LevelName").get_type() != nbt::tag_type::String) {
            return std::nullopt;
        }
        return selection_from_text(static_cast<const std::string&>(root->at("LevelName")));
    } catch (...) {
        return std::nullopt;
    }
}

const BlockLayer* layer_for(const ChunkData& chunk, std::int32_t sub_y, int layer)
{
    const auto it = chunk.sub_chunks.find(sub_y);
    if (it == chunk.sub_chunks.end()) return nullptr;
    return layer == 0 ? &it->second.layer0 : &it->second.layer1;
}

} // namespace

void McWorldStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> McWorldStructure::read(const std::filesystem::path& path)
{
    auto opened = BedrockWorldAdapter::open(path, false);
    if (!opened) return Result<void>::failure(opened.error());
    auto selection = selection_from_text(path.filename().string());
    if (!selection) selection = selection_from_level_dat(opened.value().directory() / "level.dat");
    if (!selection) {
        return Result<void>::failure(
            "无法从 MCWorld 文件名或 LevelName 解析选区 @[x,y,z]~[x,y,z]");
    }
    mMin = {
        std::min(selection->first.x, selection->second.x),
        std::min(selection->first.y, selection->second.y),
        std::min(selection->first.z, selection->second.z)
    };
    mMax = {
        std::max(selection->first.x, selection->second.x),
        std::max(selection->first.y, selection->second.y),
        std::max(selection->first.z, selection->second.z)
    };
    mOriginalSize = {
        mMax.x - mMin.x + 1,
        mMax.y - mMin.y + 1,
        mMax.z - mMin.z + 1
    };
    mWorld.emplace(std::move(opened).value());
    mChunkCache.clear();
    set_offset({});
    return Result<void>::success();
}

Result<const ChunkData*> McWorldStructure::source_chunk(ChunkPos pos) const
{
    if (const auto it = mChunkCache.find(pos); it != mChunkCache.end()) {
        return Result<const ChunkData*>::success(&it->second);
    }
    if (!mWorld) return Result<const ChunkData*>::failure("MCWorld 尚未打开");
    auto loaded = mWorld->load_chunk(pos);
    if (!loaded) return Result<const ChunkData*>::failure(loaded.error());
    const auto [it, inserted] = mChunkCache.emplace(pos, std::move(loaded).value());
    return Result<const ChunkData*>::success(&it->second);
}

Result<ChunkMap> McWorldStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    for (const auto local_chunk_pos : positions) {
        auto& output = result.at(local_chunk_pos);
        const auto local_x_begin = std::max(0, local_chunk_pos.x * 16);
        const auto local_x_end = std::min(mSize.width - 1, local_chunk_pos.x * 16 + 15);
        const auto local_z_begin = std::max(0, local_chunk_pos.z * 16);
        const auto local_z_end = std::min(mSize.length - 1, local_chunk_pos.z * 16 + 15);
        if (local_x_begin > local_x_end || local_z_begin > local_z_end) continue;
        for (int local_x = local_x_begin; local_x <= local_x_end; ++local_x) {
            const auto original_x = local_x - mOffset.x;
            if (original_x < 0 || original_x >= mOriginalSize.width) continue;
            const auto source_x = mMin.x + original_x;
            for (int local_z = local_z_begin; local_z <= local_z_end; ++local_z) {
                const auto original_z = local_z - mOffset.z;
                if (original_z < 0 || original_z >= mOriginalSize.length) continue;
                const auto source_z = mMin.z + original_z;
                const ChunkPos source_chunk_pos{ floor_div(source_x, 16), floor_div(source_z, 16) };
                const auto source = source_chunk(source_chunk_pos);
                if (!source) return Result<ChunkMap>::failure(source.error());
                for (int local_y = 0; local_y < mSize.height; ++local_y) {
                    const auto original_y = local_y - mOffset.y;
                    if (original_y < 0 || original_y >= mOriginalSize.height) continue;
                    const auto source_y = mMin.y + original_y;
                    const auto source_sub_y = floor_div(source_y, 16);
                    const auto source_index = static_cast<std::size_t>(
                        (floor_mod(source_y, 16) * 16 + floor_mod(source_z, 16)) * 16 + floor_mod(source_x, 16));
                    const auto output_sub_y = floor_div(local_y - 64, 16);
                    const auto output_index = static_cast<std::size_t>(
                        ((local_y - (output_sub_y * 16 + 64)) * 16 + floor_mod(local_z, 16)) * 16 +
                        floor_mod(local_x, 16));
                    for (int layer = 0; layer < 2; ++layer) {
                        const auto* source_layer = layer_for(*source.value(), source_sub_y, layer);
                        if (!source_layer) continue;
                        const auto runtime_id = (*source_layer)[source_index];
                        if (runtime_id == mRegistry.air_runtime_id()) continue;
                        auto [sub_it, inserted] = output.sub_chunks.try_emplace(output_sub_y);
                        if (inserted) {
                            sub_it->second.layer0.fill(mRegistry.air_runtime_id());
                            sub_it->second.layer1.fill(mRegistry.air_runtime_id());
                        }
                        auto& target = layer == 0 ? sub_it->second.layer0 : sub_it->second.layer1;
                        target[output_index] = runtime_id;
                    }
                }
            }
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> McWorldStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    if (!mWorld) return Result<NbtChunkMap>::failure("MCWorld 尚未打开");
    const auto min_source_chunk_x = floor_div(mMin.x, 16);
    const auto max_source_chunk_x = floor_div(mMax.x, 16);
    const auto min_source_chunk_z = floor_div(mMin.z, 16);
    const auto max_source_chunk_z = floor_div(mMax.z, 16);
    for (int chunk_x = min_source_chunk_x; chunk_x <= max_source_chunk_x; ++chunk_x) {
        for (int chunk_z = min_source_chunk_z; chunk_z <= max_source_chunk_z; ++chunk_z) {
            const auto loaded = mWorld->load_chunk_nbt({ chunk_x, chunk_z });
            if (!loaded) return Result<NbtChunkMap>::failure(loaded.error());
            for (const auto& source : loaded.value()) {
                if (source.pos.x < mMin.x || source.pos.x > mMax.x || source.pos.y < mMin.y ||
                    source.pos.y > mMax.y || source.pos.z < mMin.z || source.pos.z > mMax.z) continue;
                const auto local_x = source.pos.x - mMin.x + mOffset.x;
                const auto local_y = source.pos.y - mMin.y + mOffset.y;
                const auto local_z = source.pos.z - mMin.z + mOffset.z;
                const ChunkPos local_chunk{ floor_div(local_x, 16), floor_div(local_z, 16) };
                const auto it = result.find(local_chunk);
                if (it == result.end()) continue;
                it->second.push_back({
                    {
                        local_x - local_chunk.x * 16,
                        structure_y_to_chunk_local(local_y),
                        local_z - local_chunk.z * 16
                    },
                    source.payload
                });
            }
        }
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<std::size_t> McWorldStructure::count_non_air_blocks() const
{
    return Result<std::size_t>::success(static_cast<std::size_t>(mOriginalSize.volume()));
}

Result<void> McWorldStructure::write_to_world(
    WorldTarget& world,
    SubChunkPos start,
    ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> McWorldStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("MCWorld 导出接口尚未迁移");
}

} // namespace water_structure
