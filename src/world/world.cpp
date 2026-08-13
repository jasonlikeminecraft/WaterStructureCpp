#include <WaterStructure/world.hpp>
#include <WaterStructure/coordinates.hpp>

#include "archive.hpp"

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_primitive.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>

namespace water_structure {

namespace {
constexpr std::int32_t kMinSubChunkY = kOverworldMinY / 16;
constexpr std::int32_t kMaxSubChunkY = 19;
using Clock = std::chrono::steady_clock;

std::string error_for(const char* operation, const std::string& detail)
{
    return std::string(operation) + ": " + detail;
}

std::uint32_t& block_at(BlockLayer& layer, int x, int y, int z)
{
    return layer[static_cast<std::size_t>((y * 16 + z) * 16 + x)];
}

std::optional<std::int32_t> compound_int(const nbt::tag_compound& compound, const char* key)
{
    if (!compound.has_key(key)) {
        return std::nullopt;
    }
    const auto& value = compound.at(key);
    switch (value.get_type()) {
    case nbt::tag_type::Byte: return static_cast<std::int32_t>(value.as<nbt::tag_byte>().get());
    case nbt::tag_type::Short: return static_cast<std::int32_t>(value.as<nbt::tag_short>().get());
    case nbt::tag_type::Int: return value.as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return static_cast<std::int32_t>(value.as<nbt::tag_long>().get());
    default: return std::nullopt;
    }
}

NbtPayload serialize_compound(const nbt::tag_compound& compound)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", compound, output, endian::little);
    const auto bytes = output.str();
    return NbtPayload(bytes.begin(), bytes.end());
}

std::filesystem::path locate_world_root(const std::filesystem::path& extracted)
{
    if (std::filesystem::is_directory(extracted / "db")) return extracted;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(extracted)) {
        if (entry.is_directory() && entry.path().filename() == "db") {
            const auto candidate = entry.path().parent_path();
            if (std::filesystem::is_regular_file(candidate / "level.dat")) return candidate;
        }
    }
    return {};
}

std::filesystem::path make_temporary_world_directory()
{
    const auto unique = std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / "WaterStructureCpp" / unique;
}

Result<void> copy_world_directory(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    try {
        std::filesystem::create_directories(destination);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
            if (entry.is_symlink()) {
                return Result<void>::failure("只读世界目录不允许符号链接: " + entry.path().string());
            }
            const auto relative = std::filesystem::relative(entry.path(), source);
            const auto target = destination / relative;
            if (entry.is_directory()) {
                std::filesystem::create_directories(target);
            } else if (entry.is_regular_file()) {
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy_file(entry.path(), target);
            }
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("复制只读世界目录失败: " + std::string(error.what()));
    }
}

} // namespace

Result<BedrockWorldAdapter> BedrockWorldAdapter::open(
    const std::filesystem::path& directory,
    bool write_back_archive)
{
    std::filesystem::path world_directory = directory;
    std::filesystem::path temporary_directory;
    std::filesystem::path archive_path;
    if (std::filesystem::is_regular_file(directory)) {
        const auto extension = directory.extension().string();
        if (extension != ".mcworld" && extension != ".zip") {
            return Result<BedrockWorldAdapter>::failure("世界文件扩展名不是 .mcworld 或 .zip");
        }
        temporary_directory = make_temporary_world_directory();
        const auto extracted = archive::extract_zip(directory, temporary_directory);
        if (!extracted) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(temporary_directory, cleanup_error);
            return Result<BedrockWorldAdapter>::failure(extracted.error());
        }
        world_directory = locate_world_root(temporary_directory);
        if (world_directory.empty()) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(temporary_directory, cleanup_error);
            return Result<BedrockWorldAdapter>::failure(".mcworld 中未找到包含 db 的世界目录");
        }
        archive_path = std::filesystem::absolute(directory);
    } else if (std::filesystem::is_directory(directory) && !write_back_archive) {
        temporary_directory = make_temporary_world_directory();
        const auto copied = copy_world_directory(directory, temporary_directory);
        if (!copied) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(temporary_directory, cleanup_error);
            return Result<BedrockWorldAdapter>::failure(copied.error());
        }
        world_directory = temporary_directory;
    }

    auto opened = BedrockWorldOperator::World::open(world_directory);
    if (!opened) {
        if (!temporary_directory.empty()) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(temporary_directory, cleanup_error);
        }
        return Result<BedrockWorldAdapter>::failure(opened.error);
    }
    BedrockWorldAdapter result;
    result.mDirectory = world_directory;
    result.mArchivePath = std::move(archive_path);
    result.mTemporaryDirectory = std::move(temporary_directory);
    result.mWriteBackArchive = write_back_archive;
    result.mWorld.emplace(std::move(opened.value));
    return Result<BedrockWorldAdapter>::success(std::move(result));
}

BedrockWorldAdapter::~BedrockWorldAdapter()
{
    (void)close();
}

Result<void> BedrockWorldAdapter::close()
{
    if (!mWorld) {
        return Result<void>::success();
    }
    auto result = mWorld->close();
    mWorld.reset();
    if (!result) {
        return Result<void>::failure(result.error);
    }
    if (!mArchivePath.empty() && mWriteBackArchive) {
        auto temporary_archive = mArchivePath;
        temporary_archive += ".water_structure_tmp";
        const auto packed = archive::create_zip(mDirectory, temporary_archive);
        if (!packed) return packed;
        if (!MoveFileExW(
                temporary_archive.c_str(),
                mArchivePath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            return Result<void>::failure("替换 .mcworld 失败，Win32 error=" + std::to_string(error));
        }
        std::error_code cleanup_error;
        std::filesystem::remove_all(mTemporaryDirectory, cleanup_error);
        if (cleanup_error) {
            return Result<void>::failure("清理解压目录失败: " + cleanup_error.message());
        }
        mArchivePath.clear();
        mTemporaryDirectory.clear();
    }
    if (!mTemporaryDirectory.empty()) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(mTemporaryDirectory, cleanup_error);
        if (cleanup_error) return Result<void>::failure("清理解压目录失败: " + cleanup_error.message());
        mArchivePath.clear();
        mTemporaryDirectory.clear();
    }
    return Result<void>::success();
}

bool BedrockWorldAdapter::valid() const noexcept
{
    return mWorld.has_value() && mWorld->valid();
}

Result<std::optional<BlockBox>> BedrockWorldAdapter::stored_block_bounds() const
{
    if (!valid()) return Result<std::optional<BlockBox>>::failure("世界未打开");
    auto positions = mWorld->listSubChunks(BedrockWorldOperator::Dimension::Overworld);
    if (!positions) return Result<std::optional<BlockBox>>::failure(positions.error);
    if (positions.value.empty()) return Result<std::optional<BlockBox>>::success(std::nullopt);

    auto min_x = positions.value.front().x;
    auto min_y = positions.value.front().y;
    auto min_z = positions.value.front().z;
    auto max_x = min_x;
    auto max_y = min_y;
    auto max_z = min_z;
    for (const auto& position : positions.value) {
        min_x = std::min(min_x, position.x);
        min_y = std::min(min_y, position.y);
        min_z = std::min(min_z, position.z);
        max_x = std::max(max_x, position.x);
        max_y = std::max(max_y, position.y);
        max_z = std::max(max_z, position.z);
    }
    const auto block_coordinate = [](int value, int edge) -> std::optional<std::int32_t> {
        const auto result = static_cast<std::int64_t>(value) * 16 + edge;
        if (result < std::numeric_limits<std::int32_t>::min() ||
            result > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return static_cast<std::int32_t>(result);
    };
    const auto block_min_x = block_coordinate(min_x, 0);
    const auto block_min_y = block_coordinate(min_y, 0);
    const auto block_min_z = block_coordinate(min_z, 0);
    const auto block_max_x = block_coordinate(max_x, 15);
    const auto block_max_y = block_coordinate(max_y, 15);
    const auto block_max_z = block_coordinate(max_z, 15);
    if (!block_min_x || !block_min_y || !block_min_z || !block_max_x || !block_max_y || !block_max_z) {
        return Result<std::optional<BlockBox>>::failure("世界 subchunk 边界超出 int32 坐标范围");
    }
    return Result<std::optional<BlockBox>>::success(BlockBox{
        { *block_min_x, *block_min_y, *block_min_z },
        { *block_max_x, *block_max_y, *block_max_z }
    });
}

Result<ChunkData> BedrockWorldAdapter::load_chunk(ChunkPos pos) const
{
    return load_chunk_range(pos, kMinSubChunkY, kMaxSubChunkY, true);
}

Result<ChunkData> BedrockWorldAdapter::load_chunk_range(
    ChunkPos pos,
    std::int32_t min_sub_y,
    std::int32_t max_sub_y,
    bool include_layer1) const
{
    if (!valid()) {
        return Result<ChunkData>::failure("世界未打开");
    }
    min_sub_y = std::max(min_sub_y, kMinSubChunkY);
    max_sub_y = std::min(max_sub_y, kMaxSubChunkY);
    if (min_sub_y > max_sub_y) {
        return Result<ChunkData>::success({});
    }

    ChunkData result;
    auto loaded = mWorld->loadSubChunks(
        BedrockWorldOperator::Dimension::Overworld,
        { pos.x, pos.z },
        min_sub_y,
        max_sub_y
    );
    if (!loaded) {
        return Result<ChunkData>::failure(loaded.error);
    }

    for (auto& decoded : loaded.value) {
        const auto sub_y = decoded.index;
        auto& loaded_subchunk = decoded.subChunk;
        const auto blocks0 = loaded_subchunk.blocksView(0);
        if (blocks0.size() != 4096) {
            return Result<ChunkData>::failure(error_for("读取 subchunk 失败", "方块数量不是 4096"));
        }
        const auto blocks1 = include_layer1
            ? loaded_subchunk.blocksView(1)
            : std::span<const std::uint32_t>{};
        if (include_layer1 && !blocks1.empty() && blocks1.size() != 4096) {
            return Result<ChunkData>::failure(error_for("读取 subchunk 失败", "方块数量不是 4096"));
        }
        SubChunkData data;
        if (include_layer1 && blocks1.empty()) {
            data.layer1.fill(BedrockWorldOperator::airRuntimeId());
        }
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    const auto native_index = static_cast<std::size_t>(x * 256 + y * 16 + z);
                    const auto internal_index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
                    data.layer0[internal_index] = blocks0[native_index];
                    if (include_layer1 && !blocks1.empty()) {
                        data.layer1[internal_index] = blocks1[native_index];
                    }
                }
            }
        }
        result.sub_chunks.emplace(sub_y, std::move(data));
    }
    return Result<ChunkData>::success(std::move(result));
}

Result<std::vector<BlockEntity>> BedrockWorldAdapter::load_chunk_nbt(ChunkPos pos) const
{
    if (!valid()) {
        return Result<std::vector<BlockEntity>>::failure("世界未打开");
    }
    auto loaded = mWorld->loadNbt(BedrockWorldOperator::Dimension::Overworld, { pos.x, pos.z });
    if (!loaded || loaded.value.empty()) {
        return Result<std::vector<BlockEntity>>::success({});
    }
    try {
        const std::string bytes(loaded.value.begin(), loaded.value.end());
        std::istringstream input(bytes, std::ios::binary);
        std::vector<BlockEntity> entities;
        while (input.peek() != std::char_traits<char>::eof()) {
            const auto [name, compound] = nbt::io::read_compound(input, endian::little);
            if (!name.empty()) {
                return Result<std::vector<BlockEntity>>::failure("区块方块实体 NBT 根名称不为空");
            }
            BlockEntity entity;
            entity.pos = {
                compound_int(*compound, "x").value_or(pos.x * 16),
                compound_int(*compound, "y").value_or(0),
                compound_int(*compound, "z").value_or(pos.z * 16)
            };
            entity.payload = serialize_compound(*compound);
            entities.push_back(std::move(entity));
        }
        return Result<std::vector<BlockEntity>>::success(std::move(entities));
    } catch (const std::exception& error) {
        return Result<std::vector<BlockEntity>>::failure(
            "解析区块方块实体 NBT (" + std::to_string(pos.x) + ", " + std::to_string(pos.z) + ") 失败: " +
            error.what()
        );
    }
}

Result<void> BedrockWorldAdapter::save_chunk(ChunkPos pos, const ChunkData& chunk)
{
    const std::array writes{ ChunkWrite{ pos, &chunk } };
    return save_chunks(writes);
}

Result<std::vector<EncodedSubChunkData>> BedrockWorldAdapter::encode_chunks(
    std::span<const ChunkWrite> chunks) const
{
    if (!valid()) {
        return Result<std::vector<EncodedSubChunkData>>::failure("世界未打开");
    }

    std::vector<EncodedSubChunkData> result;
    std::size_t sub_chunk_count = 0;
    for (const auto& write : chunks) {
        if (!write.chunk) {
            return Result<std::vector<EncodedSubChunkData>>::failure("chunk write is empty");
        }
        sub_chunk_count += write.chunk->sub_chunks.size();
    }
    result.reserve(sub_chunk_count);
    const auto air_runtime_id = BedrockWorldOperator::airRuntimeId();
    for (const auto& chunk_write : chunks) {
        const auto pos = chunk_write.pos;
        for (const auto& [sub_y, data] : chunk_write.chunk->sub_chunks) {
            if (sub_y < kMinSubChunkY || sub_y > kMaxSubChunkY) {
                return Result<std::vector<EncodedSubChunkData>>::failure(
                    "subchunk Y 超出 Overworld 范围: " + std::to_string(sub_y)
                );
            }
            auto sub_chunk = BedrockWorldOperator::SubChunk::createAirFilled();
            BlockLayer native_layer0{};
            const bool has_layer1 = std::any_of(
                data.layer1.begin(), data.layer1.end(),
                [air_runtime_id](const auto runtime_id) { return runtime_id != air_runtime_id; });
            BlockLayer native_layer1{};
            if (chunk_write.chunk->layout == BlockLayerLayout::Native) {
                native_layer0 = data.layer0;
                if (has_layer1) native_layer1 = data.layer1;
            } else {
                for (int x = 0; x < 16; ++x) {
                    for (int y = 0; y < 16; ++y) {
                        for (int z = 0; z < 16; ++z) {
                            const auto native_index = static_cast<std::size_t>(x * 256 + y * 16 + z);
                            const auto internal_index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
                            native_layer0[native_index] = data.layer0[internal_index];
                            if (has_layer1) native_layer1[native_index] = data.layer1[internal_index];
                        }
                    }
                }
            }
            if (!sub_chunk.setBlocks(
                std::span<const std::uint32_t>(native_layer0.data(), native_layer0.size()), 0)) {
                return Result<std::vector<EncodedSubChunkData>>::failure(
                    "创建 subchunk layer 0 失败");
            }
            if (has_layer1 && !sub_chunk.setBlocks(
                std::span<const std::uint32_t>(native_layer1.data(), native_layer1.size()), 1)) {
                return Result<std::vector<EncodedSubChunkData>>::failure(
                    "创建 subchunk layer 1 失败");
            }
            auto encoded = BedrockWorldOperator::encodeSubChunkPayload(
                sub_chunk,
                BedrockWorldOperator::Encoding::Disk,
                kOverworldMinY,
                319,
                sub_y - kMinSubChunkY);
            if (!encoded) {
                return Result<std::vector<EncodedSubChunkData>>::failure(
                    "编码 subchunk 失败: " + encoded.error);
            }
            result.push_back({
                { pos.x, sub_y, pos.z },
                std::move(encoded.value)
            });
        }
    }
    return Result<std::vector<EncodedSubChunkData>>::success(std::move(result));
}

Result<SubChunkData> BedrockWorldAdapter::decode_subchunk_payload(
    std::span<const std::uint8_t> payload) const
{
    if (!valid()) return Result<SubChunkData>::failure("世界未打开");
    auto decoded = BedrockWorldOperator::decodeSubChunkPayload(
        payload, BedrockWorldOperator::Encoding::Disk, kOverworldMinY, 319);
    if (!decoded) {
        return Result<SubChunkData>::failure("解码 subchunk 失败: " + decoded.error);
    }
    const auto blocks0 = decoded.value.subChunk.blocksView(0);
    if (blocks0.size() != 4096) {
        return Result<SubChunkData>::failure("解码 subchunk 失败: layer 0 方块数量不是 4096");
    }
    const auto blocks1 = decoded.value.subChunk.blocksView(1);
    if (!blocks1.empty() && blocks1.size() != 4096) {
        return Result<SubChunkData>::failure("解码 subchunk 失败: layer 1 方块数量不是 4096");
    }
    SubChunkData data;
    data.layer1.fill(BedrockWorldOperator::airRuntimeId());
    std::copy(blocks0.begin(), blocks0.end(), data.layer0.begin());
    if (!blocks1.empty()) std::copy(blocks1.begin(), blocks1.end(), data.layer1.begin());
    return Result<SubChunkData>::success(std::move(data));
}

Result<std::optional<std::vector<std::uint8_t>>> BedrockWorldAdapter::load_subchunk_payload(
    SubChunkPos pos) const
{
    if (!valid()) {
        return Result<std::optional<std::vector<std::uint8_t>>>::failure("世界未打开");
    }
    auto loaded = mWorld->loadSubChunkPayload(
        BedrockWorldOperator::Dimension::Overworld,
        { pos.x, pos.y, pos.z });
    if (!loaded) {
        return Result<std::optional<std::vector<std::uint8_t>>>::failure(loaded.error);
    }
    return Result<std::optional<std::vector<std::uint8_t>>>::success(
        std::move(loaded.value));
}

Result<void> BedrockWorldAdapter::save_subchunk_payloads(
    std::vector<EncodedSubChunkData> subchunks)
{
    if (!valid()) return Result<void>::failure("世界未打开");
    std::vector<BedrockWorldOperator::PositionedSubChunkPayload> writes;
    writes.reserve(subchunks.size());
    for (auto& subchunk : subchunks) {
        writes.push_back({
            { subchunk.pos.x, subchunk.pos.y, subchunk.pos.z },
            std::move(subchunk.payload)
        });
    }
    auto saved = mWorld->saveSubChunkPayloadsBatch(
        BedrockWorldOperator::Dimension::Overworld, writes);
    if (!saved) return Result<void>::failure(saved.error);
    return Result<void>::success();
}

Result<void> BedrockWorldAdapter::save_chunks(std::span<const ChunkWrite> chunks)
{
    auto encoded = encode_chunks(chunks);
    if (!encoded) return Result<void>::failure(encoded.error());
    return save_subchunk_payloads(std::move(encoded).value());
}

Result<void> BedrockWorldAdapter::save_chunk_nbt(ChunkPos pos, std::span<const BlockEntity> entities)
{
    if (!valid()) {
        return Result<void>::failure("世界未打开");
    }

    try {
        std::ostringstream output(std::ios::binary);
        for (const auto& entity : entities) {
            if (entity.payload.empty()) {
                continue;
            }
            const std::string bytes(entity.payload.begin(), entity.payload.end());
            std::istringstream input(bytes, std::ios::binary);
            auto [name, compound] = nbt::io::read_compound(input, endian::little);
            compound->operator[]("x") = entity.pos.x;
            compound->operator[]("y") = entity.pos.y;
            compound->operator[]("z") = entity.pos.z;
            nbt::io::write_tag("", *compound, output, endian::little);
        }
        const auto bytes = output.str();
        if (bytes.empty()) {
            return Result<void>::success();
        }
        const auto payload = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        auto saved = mWorld->saveNbt(BedrockWorldOperator::Dimension::Overworld, { pos.x, pos.z }, payload);
        if (!saved) {
            return Result<void>::failure(saved.error);
        }
    } catch (const std::exception& error) {
        return Result<void>::failure(
            "序列化区块方块实体 NBT (" + std::to_string(pos.x) + ", " + std::to_string(pos.z) + ") 失败: " +
            error.what()
        );
    }
    return Result<void>::success();
}

Result<void> convert_to_world(const IStructure& structure, WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks)
{
    const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr;
    double visit_chunks_ms = 0.0;
    double save_chunks_ms = 0.0;
    double visit_entities_ms = 0.0;
    double save_entities_ms = 0.0;
    const auto elapsed_ms = [](const auto begin) {
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    };
    const auto structure_size = structure.size();
    const auto total_chunks = static_cast<std::size_t>(structure_size.chunk_x_count()) * structure_size.chunk_z_count();
    if (callbacks.start) {
        callbacks.start(total_chunks);
    }

    std::vector<ChunkPos> positions;
    positions.reserve(total_chunks);
    for (std::int32_t z = 0; z < structure_size.chunk_z_count(); ++z) {
        for (std::int32_t x = 0; x < structure_size.chunk_x_count(); ++x) {
            positions.push_back({ x, z });
        }
    }

    const std::size_t batch_size =
        (structure.id() == StructureId::SchemV1 || structure.id() == StructureId::SchemV2)
        ? static_cast<std::size_t>(structure_size.chunk_x_count())
        : 64;
    for (std::size_t begin = 0; begin < positions.size(); begin += batch_size) {
        const auto end = std::min(begin + batch_size, positions.size());
        const auto batch = std::span<const ChunkPos>(positions).subspan(begin, end - begin);
        const auto visit_chunks_start = Clock::now();
        auto visited_chunks = structure.visit_chunks(batch,
            [&](ChunkPos local_pos, const ChunkData& chunk) -> Result<void> {
            const ChunkPos target_pos{ local_pos.x + start.x, local_pos.z + start.z };
            ChunkData shifted;
            const auto sub_y_offset = start.y - kMinSubChunkY;
            for (const auto& [local_sub_y, sub_chunk] : chunk.sub_chunks) {
                const auto target_sub_y = local_sub_y + sub_y_offset;
                if (target_sub_y < kMinSubChunkY || target_sub_y > kMaxSubChunkY) {
                    return Result<void>::failure(
                        "目标 subchunk 超出 Overworld 高度范围: chunk=(" + std::to_string(target_pos.x) + ", " +
                        std::to_string(target_pos.z) + "), subY=" + std::to_string(target_sub_y)
                    );
                }
                shifted.sub_chunks.emplace(target_sub_y, sub_chunk);
            }
            const auto save_start = Clock::now();
            auto saved = world.save_chunk(target_pos, shifted);
            save_chunks_ms += elapsed_ms(save_start);
            if (!saved) {
                return Result<void>::failure(
                    "保存区块 (" + std::to_string(target_pos.x) + ", " + std::to_string(target_pos.z) + ") 失败: " +
                    saved.error()
                );
            }
            if (callbacks.progress) {
                callbacks.progress();
            }
            return Result<void>::success();
        });
        visit_chunks_ms += elapsed_ms(visit_chunks_start);
        if (!visited_chunks) return visited_chunks;

        const auto visit_entities_start = Clock::now();
        auto visited_entities = structure.visit_chunk_nbt(batch,
            [&](ChunkPos local_pos, std::span<const BlockEntity> values) -> Result<void> {
            if (values.empty()) return Result<void>::success();
            const ChunkPos target_pos{ local_pos.x + start.x, local_pos.z + start.z };
            std::vector<BlockEntity> shifted_values(values.begin(), values.end());
            for (auto& entity : shifted_values) {
                entity.pos.x += target_pos.x * 16;
                entity.pos.y += start.y * 16 - kOverworldMinY;
                entity.pos.z += target_pos.z * 16;
            }
            const auto save_start = Clock::now();
            auto saved = world.save_chunk_nbt(
                target_pos,
                shifted_values
            );
            save_entities_ms += elapsed_ms(save_start);
            if (!saved) {
                return Result<void>::failure(
                    "保存区块 NBT (" + std::to_string(target_pos.x) + ", " + std::to_string(target_pos.z) + ") 失败: " +
                    saved.error()
                );
            }
            return Result<void>::success();
        });
        visit_entities_ms += elapsed_ms(visit_entities_start);
        if (!visited_entities) return visited_entities;
        structure.release_cached_chunks();
    }
    if (profile) {
        std::cerr << "world_conversion_profile visit_chunks_ms=" << visit_chunks_ms
                  << " save_chunks_ms=" << save_chunks_ms
                  << " visit_without_save_ms=" << (visit_chunks_ms - save_chunks_ms)
                  << " visit_entities_ms=" << visit_entities_ms
                  << " save_entities_ms=" << save_entities_ms
                  << '\n';
    }
    return Result<void>::success();
}

} // namespace water_structure
