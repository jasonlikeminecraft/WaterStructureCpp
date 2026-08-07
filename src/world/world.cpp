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
#include <sstream>

namespace water_structure {

namespace {
constexpr std::int32_t kMinSubChunkY = kOverworldMinY / 16;
constexpr std::int32_t kMaxSubChunkY = 19;

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
        const auto blocks0 = loaded_subchunk.blocks(0);
        if (blocks0.size() != 4096) {
            return Result<ChunkData>::failure(error_for("读取 subchunk 失败", "方块数量不是 4096"));
        }
        const auto blocks1 = include_layer1 ? loaded_subchunk.blocks(1) : BedrockWorldOperator::BlockRuntimeList{};
        if (include_layer1 && blocks1.size() != 4096) {
            return Result<ChunkData>::failure(error_for("读取 subchunk 失败", "方块数量不是 4096"));
        }
        SubChunkData data;
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    const auto native_index = static_cast<std::size_t>(x * 256 + y * 16 + z);
                    const auto internal_index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
                    data.layer0[internal_index] = blocks0[native_index];
                    if (include_layer1) data.layer1[internal_index] = blocks1[native_index];
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
    if (!valid()) {
        return Result<void>::failure("世界未打开");
    }

    for (const auto& [sub_y, data] : chunk.sub_chunks) {
        if (sub_y < kMinSubChunkY || sub_y > kMaxSubChunkY) {
            return Result<void>::failure(
                "subchunk Y 超出 Overworld 范围: " + std::to_string(sub_y)
            );
        }
        auto sub_chunk = BedrockWorldOperator::SubChunk::createAirFilled();
        BlockLayer native_layer0{};
        BlockLayer native_layer1{};
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 16; ++z) {
                    const auto native_index = static_cast<std::size_t>(x * 256 + y * 16 + z);
                    const auto internal_index = static_cast<std::size_t>((y * 16 + z) * 16 + x);
                    native_layer0[native_index] = data.layer0[internal_index];
                    native_layer1[native_index] = data.layer1[internal_index];
                }
            }
        }
        if (!sub_chunk.setBlocks(std::span<const std::uint32_t>(native_layer0.data(), native_layer0.size()), 0)) {
            return Result<void>::failure("创建 subchunk layer 0 失败");
        }
        if (!sub_chunk.setBlocks(std::span<const std::uint32_t>(native_layer1.data(), native_layer1.size()), 1)) {
            return Result<void>::failure("创建 subchunk layer 1 失败");
        }
        auto saved = mWorld->saveSubChunk(
            BedrockWorldOperator::Dimension::Overworld,
            { pos.x, sub_y, pos.z },
            sub_chunk
        );
        if (!saved) {
            return Result<void>::failure(saved.error);
        }
    }
    return Result<void>::success();
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
    const auto structure_size = structure.size();
    const auto total_chunks = static_cast<std::size_t>(structure_size.chunk_x_count()) * structure_size.chunk_z_count();
    if (callbacks.start) {
        callbacks.start(total_chunks);
    }

    std::vector<ChunkPos> positions;
    positions.reserve(total_chunks);
    for (std::int32_t x = 0; x < structure_size.chunk_x_count(); ++x) {
        for (std::int32_t z = 0; z < structure_size.chunk_z_count(); ++z) {
            positions.push_back({ x, z });
        }
    }

    constexpr std::size_t batch_size = 64;
    for (std::size_t begin = 0; begin < positions.size(); begin += batch_size) {
        const auto end = std::min(begin + batch_size, positions.size());
        const auto batch = std::span<const ChunkPos>(positions).subspan(begin, end - begin);

        auto chunks = structure.get_chunks(batch);
        if (!chunks) {
            return Result<void>::failure(chunks.error());
        }
        for (auto& [local_pos, chunk] : chunks.value()) {
            const ChunkPos target_pos{ local_pos.x + start.x, local_pos.z + start.z };
            ChunkData shifted;
            const auto sub_y_offset = start.y - kMinSubChunkY;
            for (auto& [local_sub_y, sub_chunk] : chunk.sub_chunks) {
                const auto target_sub_y = local_sub_y + sub_y_offset;
                if (target_sub_y < kMinSubChunkY || target_sub_y > kMaxSubChunkY) {
                    return Result<void>::failure(
                        "目标 subchunk 超出 Overworld 高度范围: chunk=(" + std::to_string(target_pos.x) + ", " +
                        std::to_string(target_pos.z) + "), subY=" + std::to_string(target_sub_y)
                    );
                }
                shifted.sub_chunks.emplace(target_sub_y, std::move(sub_chunk));
            }
            auto saved = world.save_chunk(target_pos, shifted);
            if (!saved) {
                return Result<void>::failure(
                    "保存区块 (" + std::to_string(target_pos.x) + ", " + std::to_string(target_pos.z) + ") 失败: " +
                    saved.error()
                );
            }
            if (callbacks.progress) {
                callbacks.progress();
            }
        }

        auto entities = structure.get_chunk_nbt(batch);
        if (!entities) {
            return Result<void>::failure(entities.error());
        }
        for (auto& [local_pos, values] : entities.value()) {
            if (values.empty()) {
                continue;
            }
            const ChunkPos target_pos{ local_pos.x + start.x, local_pos.z + start.z };
            for (auto& entity : values) {
                entity.pos.x += target_pos.x * 16;
                entity.pos.y += start.y * 16 - kOverworldMinY;
                entity.pos.z += target_pos.z * 16;
            }
            auto saved = world.save_chunk_nbt(
                target_pos,
                values
            );
            if (!saved) {
                return Result<void>::failure(
                    "保存区块 NBT (" + std::to_string(target_pos.x) + ", " + std::to_string(target_pos.z) + ") 失败: " +
                    saved.error()
                );
            }
        }
    }
    return Result<void>::success();
}

} // namespace water_structure
