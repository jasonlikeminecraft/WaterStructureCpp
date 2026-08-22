#include <WaterStructure/world.hpp>
#include <WaterStructure/chunk_stream.hpp>
#include <WaterStructure/coordinates.hpp>

#include "archive.hpp"
#include "../core/bounded_thread_pool.hpp"

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_primitive.h>

#if defined(_WIN32)
#  include <Windows.h>
#else
#  include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <thread>

namespace water_structure::detail {

void BoundedThreadPoolDeleter::operator()(BoundedThreadPool* pool) const noexcept
{
    delete pool;
}

} // namespace water_structure::detail

namespace water_structure {

namespace {
constexpr std::int32_t kMinSubChunkY = kOverworldMinY / 16;
constexpr std::int32_t kMaxSubChunkY = 19;
using Clock = std::chrono::steady_clock;

std::size_t world_encode_worker_count(std::size_t chunk_count, std::size_t requested)
{
    if (chunk_count < 2) return 1;
    // Encoding is CPU-heavy, while the following LevelDB commit is a single
    // writer. Two workers are the conservative default: it overlaps BWO
    // encoding without competing excessively for memory bandwidth. The
    // setting is intentionally process-local so callers that already own a
    // larger scheduler can tune it without changing the public ABI.
    std::size_t configured = requested == 0 ? 2 : requested;
    const auto* text = requested == 0
        ? std::getenv("WATER_STRUCTURE_WORLD_ENCODE_THREADS") : nullptr;
    if (text && *text) {
        std::size_t parsed = 0;
        const auto* end = text + std::char_traits<char>::length(text);
        const auto [next, error] = std::from_chars(text, end, parsed);
        if (error == std::errc{} && next == end && parsed != 0) configured = parsed;
        else if (error == std::errc{} && next == end && parsed == 0) configured = 1;
    }
    // Avoid accidental thread explosions from an environment value while
    // still allowing explicit tuning for high-core machines.
    configured = std::min<std::size_t>(configured, 16);
    return std::min(configured, chunk_count);
}

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

std::filesystem::path make_temporary_world_directory(
    const std::filesystem::path& configured_root = {})
{
#if defined(_WIN32)
    const auto process_id = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process_id = static_cast<std::uint64_t>(getpid());
#endif
    static std::atomic<std::uint64_t> sequence{0};
    const auto unique = std::to_string(process_id) + "-" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    const auto root = configured_root.empty()
        ? std::filesystem::temp_directory_path()
        : configured_root;
    return root / "WaterStructureCpp" / unique;
}

std::filesystem::path make_temporary_archive_path(const std::filesystem::path& target)
{
#if defined(_WIN32)
    const auto process_id = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process_id = static_cast<std::uint64_t>(getpid());
#endif
    static std::atomic<std::uint64_t> sequence{0};
    auto path = target;
    path += ".water_structure_tmp-" + std::to_string(process_id) + "-" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return path;
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
    return open(directory, WorldOpenOptions{
        .write_back_archive = write_back_archive
    });
}

Result<BedrockWorldAdapter> BedrockWorldAdapter::open(
    const std::filesystem::path& directory,
    const WorldOpenOptions& options)
{
    const auto unpack_start = Clock::now();
    std::uint64_t unpack_ms = 0;
    std::filesystem::path world_directory = directory;
    std::filesystem::path temporary_directory;
    std::filesystem::path archive_path;
    auto extension = directory.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const bool archive_extension = extension == ".mcworld" || extension == ".zip";
    if (std::filesystem::is_regular_file(directory)) {
        if (!archive_extension) {
            return Result<BedrockWorldAdapter>::failure("世界文件扩展名不是 .mcworld 或 .zip");
        }
        if (!options.allow_temporary_spool) {
            return Result<BedrockWorldAdapter>::failure(
                "capability error: .mcworld 输入需要受控临时目录，但 allow_temporary_spool=false");
        }
        temporary_directory = make_temporary_world_directory(options.temporary_directory);
        const auto extracted = archive::extract_zip(
            directory, temporary_directory, options.temporary_file_limit_bytes);
        if (!extracted) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(temporary_directory, cleanup_error);
            return Result<BedrockWorldAdapter>::failure(extracted.error());
        }
        unpack_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - unpack_start).count());
        world_directory = locate_world_root(temporary_directory);
        if (world_directory.empty()) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(temporary_directory, cleanup_error);
            return Result<BedrockWorldAdapter>::failure(".mcworld 中未找到包含 db 的世界目录");
        }
        archive_path = std::filesystem::absolute(directory);
    } else if (!std::filesystem::exists(directory) && archive_extension) {
        // A missing .mcworld/.zip is an output archive request. Build the
        // world in a private directory first, then close() packs it and
        // atomically moves the archive into place. This keeps LevelDB away
        // from the final archive path and preserves streaming writes.
        if (!options.allow_temporary_spool) {
            return Result<BedrockWorldAdapter>::failure(
                "capability error: .mcworld 输出需要受控临时目录，但 allow_temporary_spool=false");
        }
        temporary_directory = make_temporary_world_directory(options.temporary_directory);
        world_directory = temporary_directory;
        archive_path = std::filesystem::absolute(directory);
        std::error_code parent_error;
        std::filesystem::create_directories(archive_path.parent_path(), parent_error);
        if (parent_error) {
            return Result<BedrockWorldAdapter>::failure(
                "创建 .mcworld 输出目录失败: " + parent_error.message());
        }
    } else if (std::filesystem::is_directory(directory) && !options.write_back_archive) {
        if (!options.allow_temporary_spool) {
            return Result<BedrockWorldAdapter>::failure(
                "capability error: 只读目录世界需要受控副本，但 allow_temporary_spool=false");
        }
        temporary_directory = make_temporary_world_directory(options.temporary_directory);
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
    result.mWriteBackArchive = options.write_back_archive;
    result.mTemporaryFileLimitBytes = options.temporary_file_limit_bytes;
    result.mIoStats.mcworld_unpack_ms = unpack_ms;
    result.mWorld.emplace(std::move(opened.value));
    return Result<BedrockWorldAdapter>::success(std::move(result));
}

BedrockWorldAdapter::~BedrockWorldAdapter()
{
    // Never call deferred user code from a destructor. A callback may capture
    // locals declared after the adapter and those references are no longer
    // valid by the time automatic destruction reaches us. Explicit close()
    // remains the commit/statistics boundary for callers that need final
    // LevelDB-close and archive-pack timings.
    mDeferredStats.reset();
    mDeferredStatisticsCallback = {};
    try {
        if (mDiscardOnClose) (void)discard();
        else (void)close();
    } catch (...) {
        // Destruction is a last-resort resource boundary and must never
        // terminate the process. Explicit close()/discard() report errors.
    }
}

Result<void> BedrockWorldAdapter::cleanup_temporary_artifacts()
{
    std::string error;
    if (!mPendingArchivePath.empty()) {
        std::error_code remove_error;
        std::filesystem::remove(mPendingArchivePath, remove_error);
        if (remove_error) {
            error = "清理临时 .mcworld 失败: " + remove_error.message();
        } else {
            mPendingArchivePath.clear();
        }
    }
    if (!mTemporaryDirectory.empty()) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(mTemporaryDirectory, cleanup_error);
        if (cleanup_error) {
            if (!error.empty()) error += "; ";
            error += "清理世界临时目录失败: " + cleanup_error.message();
        } else {
            mTemporaryDirectory.clear();
        }
    }
    return error.empty()
        ? Result<void>::success()
        : Result<void>::failure(std::move(error));
}

Result<void> BedrockWorldAdapter::discard()
{
    mDiscardOnClose = false;
    mEncodePool.reset();
    mEncodePoolWorkers = 0;
    std::string error;
    if (mWorld) {
        const auto close_start = Clock::now();
        auto closed = mWorld->close();
        mIoStats.leveldb_close_ms += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - close_start).count());
        mWorld.reset();
        if (!closed) error = "关闭丢弃世界失败: " + closed.error;
    }
    // Clearing the commit destination before cleanup makes repeated close()
    // calls cleanup-only even if a locked temporary file cannot be removed on
    // the first attempt. The original/final archive is never touched here.
    mArchivePath.clear();
    mWriteBackArchive = false;
    auto cleaned = cleanup_temporary_artifacts();
    if (!cleaned) {
        if (!error.empty()) error += "; ";
        error += cleaned.error();
    }
    emit_deferred_statistics(false, error.empty() ? "conversion aborted" : "discard cleanup");
    return error.empty()
        ? Result<void>::success()
        : Result<void>::failure(std::move(error));
}

Result<void> BedrockWorldAdapter::close()
{
    if (mDiscardOnClose) return discard();
    // All save_chunks() calls join their encoding futures before returning,
    // so releasing the reusable pool here is safe and avoids keeping worker
    // threads alive during archive packing.
    mEncodePool.reset();
    mEncodePoolWorkers = 0;
    if (!mWorld) {
        auto cleaned = cleanup_temporary_artifacts();
        if (!cleaned) {
            emit_deferred_statistics(false, "mcworld cleanup");
            return cleaned;
        }
        emit_deferred_statistics(true);
        return Result<void>::success();
    }
    const auto close_start = Clock::now();
    auto result = mWorld->close();
    mIoStats.leveldb_close_ms += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - close_start).count());
    mWorld.reset();
    if (!result) {
        mArchivePath.clear();
        mWriteBackArchive = false;
        const auto cleaned = cleanup_temporary_artifacts();
        emit_deferred_statistics(false, "leveldb close");
        auto error = result.error;
        if (!cleaned) error += "; " + cleaned.error();
        return Result<void>::failure(std::move(error));
    }
    if (!mArchivePath.empty() && mWriteBackArchive) {
        const auto pack_start = Clock::now();
        mPendingArchivePath = make_temporary_archive_path(mArchivePath);
        const auto packed = archive::create_zip(
            mDirectory, mPendingArchivePath, mTemporaryFileLimitBytes);
        mIoStats.mcworld_pack_ms += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - pack_start).count());
        if (!packed) {
            mArchivePath.clear();
            mWriteBackArchive = false;
            const auto cleaned = cleanup_temporary_artifacts();
            emit_deferred_statistics(false, "mcworld pack");
            auto error = packed.error();
            if (!cleaned) error += "; " + cleaned.error();
            return Result<void>::failure(std::move(error));
        }
#if defined(_WIN32)
        if (!MoveFileExW(mPendingArchivePath.c_str(), mArchivePath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            mArchivePath.clear();
            mWriteBackArchive = false;
            const auto cleaned = cleanup_temporary_artifacts();
            emit_deferred_statistics(false, "mcworld replace");
            auto message = "替换 .mcworld 失败，Win32 error=" + std::to_string(error);
            if (!cleaned) message += "; " + cleaned.error();
            return Result<void>::failure(std::move(message));
        }
#else
        std::error_code replace_error;
        std::filesystem::rename(mPendingArchivePath, mArchivePath, replace_error);
        if (replace_error) {
            mArchivePath.clear();
            mWriteBackArchive = false;
            const auto cleaned = cleanup_temporary_artifacts();
            emit_deferred_statistics(false, "mcworld replace");
            auto message = "替换 .mcworld 失败: " + replace_error.message();
            if (!cleaned) message += "; " + cleaned.error();
            return Result<void>::failure(std::move(message));
        }
#endif
        mPendingArchivePath.clear();
        mArchivePath.clear();
        mWriteBackArchive = false;
    }
    mArchivePath.clear();
    mWriteBackArchive = false;
    const auto cleaned = cleanup_temporary_artifacts();
    if (!cleaned) {
        emit_deferred_statistics(false, "mcworld cleanup");
        return cleaned;
    }
    emit_deferred_statistics(true);
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
    const auto air_runtime_id = BedrockWorldOperator::airRuntimeId();
    for (const auto& write : chunks) {
        if (!write.chunk) {
            return Result<std::vector<EncodedSubChunkData>>::failure("chunk write is empty");
        }
    }
    if (chunks.empty()) {
        return Result<std::vector<EncodedSubChunkData>>::success({});
    }

    // BWO's database writer is deliberately kept out of this worker pool.
    // Each task only creates independent subchunk payloads; the caller later
    // submits the merged vector through one WriteBatch. This avoids competing
    // DB::Write calls while overlapping the expensive palette encoding stage.
    const auto encode_range = [chunks, air_runtime_id](std::size_t begin,
                                                        std::size_t end)
        -> Result<std::vector<EncodedSubChunkData>> {
        std::size_t sub_chunk_count = 0;
        for (std::size_t index = begin; index < end; ++index) {
            sub_chunk_count += chunks[index].chunk->sub_chunks.size();
        }
        std::vector<EncodedSubChunkData> result;
        result.reserve(sub_chunk_count);
        try {
            for (std::size_t index = begin; index < end; ++index) {
                const auto& chunk_write = chunks[index];
                const auto pos = chunk_write.pos;
                for (const auto& [sub_y, data] : chunk_write.chunk->sub_chunks) {
                    if (sub_y < kMinSubChunkY || sub_y > kMaxSubChunkY) {
                        return Result<std::vector<EncodedSubChunkData>>::failure(
                            "subchunk Y 超出 Overworld 范围: " + std::to_string(sub_y));
                    }
                    auto sub_chunk = BedrockWorldOperator::SubChunk::createAirFilled();
                    BlockLayer native_layer0{};
                    const bool has_layer1 = std::any_of(
                        data.layer1.begin(), data.layer1.end(),
                        [air_runtime_id](const auto runtime_id) {
                            return runtime_id != air_runtime_id;
                        });
                    BlockLayer native_layer1{};
                    const auto* layer0 = &native_layer0;
                    const auto* layer1 = &native_layer1;
                    if (chunk_write.chunk->layout == BlockLayerLayout::Native) {
                        // Schem/BDX producers already emit Bedrock's native
                        // x/y/z order. Pass their arrays directly to BWO;
                        // setBlocks() owns its storage, so an intermediate
                        // 4096-entry copy is wasted.
                        layer0 = &data.layer0;
                        if (has_layer1) layer1 = &data.layer1;
                    } else {
                        for (int x = 0; x < 16; ++x) {
                            for (int y = 0; y < 16; ++y) {
                                for (int z = 0; z < 16; ++z) {
                                    const auto native_index = static_cast<std::size_t>(
                                        x * 256 + y * 16 + z);
                                    const auto internal_index = static_cast<std::size_t>(
                                        (y * 16 + z) * 16 + x);
                                    native_layer0[native_index] = data.layer0[internal_index];
                                    if (has_layer1) {
                                        native_layer1[native_index] = data.layer1[internal_index];
                                    }
                                }
                            }
                        }
                    }
                    if (!sub_chunk.setBlocks(
                        std::span<const std::uint32_t>(layer0->data(), layer0->size()), 0)) {
                        return Result<std::vector<EncodedSubChunkData>>::failure(
                            "创建 subchunk layer 0 失败");
                    }
                    if (has_layer1 && !sub_chunk.setBlocks(
                        std::span<const std::uint32_t>(layer1->data(), layer1->size()), 1)) {
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
        } catch (const std::exception& error) {
            return Result<std::vector<EncodedSubChunkData>>::failure(
                std::string("并行编码 subchunk 失败: ") + error.what());
        }
        return Result<std::vector<EncodedSubChunkData>>::success(std::move(result));
    };

    const auto worker_count = world_encode_worker_count(chunks.size(), mConfiguredWorkerCount);
    if (worker_count == 1) return encode_range(0, chunks.size());

    // Keep the pool on the world adapter. Schem and other row-oriented
    // writers call save_chunks() many times; constructing/joining threads for
    // every stripe would erase the benefit of parallel encoding.
    if (!mEncodePool || mEncodePoolWorkers != worker_count) {
        mEncodePool.reset(new detail::BoundedThreadPool(worker_count, worker_count));
        mEncodePoolWorkers = worker_count;
    }
    std::vector<std::future<Result<std::vector<EncodedSubChunkData>>>> pending;
    pending.reserve(worker_count);
    const auto base = chunks.size() / worker_count;
    const auto remainder = chunks.size() % worker_count;
    std::size_t begin = 0;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        const auto count = base + (worker < remainder ? 1 : 0);
        const auto end = begin + count;
        pending.push_back(mEncodePool->submit([&encode_range, begin, end] {
            return encode_range(begin, end);
        }));
        begin = end;
    }

    std::vector<EncodedSubChunkData> result;
    for (auto& task : pending) {
        auto encoded = task.get();
        if (!encoded) return Result<std::vector<EncodedSubChunkData>>::failure(encoded.error());
        auto values = std::move(encoded).value();
        result.insert(
            result.end(),
            std::make_move_iterator(values.begin()),
            std::make_move_iterator(values.end()));
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

namespace {

BlockState internal_state(const BedrockWorldOperator::BlockState& decoded)
{
    BlockState result;
    result.name = decoded.name;
    result.version = decoded.version;
    result.states.reserve(decoded.states.size());
    for (const auto& property : decoded.states) {
        BlockStateProperty item;
        item.name = property.name;
        switch (property.type) {
        case BedrockWorldOperator::BlockStateValueType::Byte:
            item.type = BlockStateValueType::Byte;
            item.value = std::to_string(static_cast<std::int8_t>(property.intValue));
            break;
        case BedrockWorldOperator::BlockStateValueType::Short:
            item.type = BlockStateValueType::Short;
            item.value = std::to_string(static_cast<std::int16_t>(property.intValue));
            break;
        case BedrockWorldOperator::BlockStateValueType::Long:
            item.type = BlockStateValueType::Long;
            item.value = std::to_string(property.intValue);
            break;
        case BedrockWorldOperator::BlockStateValueType::String:
            item.type = BlockStateValueType::String;
            item.value = property.stringValue;
            break;
        default:
            item.type = BlockStateValueType::Int;
            item.value = std::to_string(static_cast<std::int32_t>(property.intValue));
            break;
        }
        result.states.push_back(std::move(item));
    }
    return result;
}

} // namespace

Result<std::optional<DecodedStateSubChunk>> BedrockWorldAdapter::load_subchunk_palette(
    SubChunkPos pos) const
{
    if (!valid()) {
        return Result<std::optional<DecodedStateSubChunk>>::failure("世界未打开");
    }
    auto loaded = mWorld->loadSubChunkPalette(
        BedrockWorldOperator::Dimension::Overworld,
        { pos.x, pos.y, pos.z });
    if (!loaded) {
        return Result<std::optional<DecodedStateSubChunk>>::failure(loaded.error);
    }
    if (!loaded.value) {
        return Result<std::optional<DecodedStateSubChunk>>::success(std::nullopt);
    }
    DecodedStateSubChunk result;
    result.palette.reserve(loaded.value->palette.size());
    for (const auto& state : loaded.value->palette) {
        result.palette.push_back(internal_state(state));
    }
    result.indices.assign(loaded.value->indices.begin(), loaded.value->indices.end());
    return Result<std::optional<DecodedStateSubChunk>>::success(std::move(result));
}

Result<std::vector<std::optional<DecodedStateSubChunk>>> BedrockWorldAdapter::load_chunk_palettes(
    ChunkPos pos,
    std::int32_t min_sub_y,
    std::int32_t max_sub_y) const
{
    if (!valid()) {
        return Result<std::vector<std::optional<DecodedStateSubChunk>>>::failure("世界未打开");
    }
    if (min_sub_y > max_sub_y) {
        return Result<std::vector<std::optional<DecodedStateSubChunk>>>::success({});
    }
    auto loaded = mWorld->loadSubChunkPalettes(
        BedrockWorldOperator::Dimension::Overworld,
        { pos.x, pos.z },
        min_sub_y,
        max_sub_y);
    if (!loaded) {
        return Result<std::vector<std::optional<DecodedStateSubChunk>>>::failure(loaded.error);
    }
    std::vector<std::optional<DecodedStateSubChunk>> result(
        static_cast<std::size_t>(max_sub_y - min_sub_y + 1));
    for (auto& [sub_y, decoded] : loaded.value) {
        auto& slot = result[static_cast<std::size_t>(sub_y - min_sub_y)];
        DecodedStateSubChunk data;
        data.palette.reserve(decoded.palette.size());
        for (const auto& state : decoded.palette) {
            data.palette.push_back(internal_state(state));
        }
        data.indices.assign(decoded.indices.begin(), decoded.indices.end());
        slot = std::move(data);
    }
    return Result<std::vector<std::optional<DecodedStateSubChunk>>>::success(std::move(result));
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
    const bool detail_profile =
        std::getenv("WATER_STRUCTURE_PROFILE_DETAIL") != nullptr;
    const auto encode_start = Clock::now();
    auto encoded = encode_chunks(chunks);
    if (!encoded) return Result<void>::failure(encoded.error());
    auto payloads = std::move(encoded).value();
    const auto encode_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - encode_start).count();
    const auto payload_count = payloads.size();
    std::uint64_t payload_bytes = 0;
    for (const auto& payload : payloads) payload_bytes += payload.payload.size();
    const auto save_start = Clock::now();
    auto saved = save_subchunk_payloads(std::move(payloads));
    const auto leveldb_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - save_start).count();
    mIoStats.encode_compress_ms += static_cast<std::uint64_t>(std::max(0.0, encode_ms));
    mIoStats.leveldb_write_ms += static_cast<std::uint64_t>(std::max(0.0, leveldb_ms));
    mIoStats.compressed_output_bytes += payload_bytes;
    ++mIoStats.leveldb_batches;
    if (detail_profile) {
        std::cerr << "world_save_chunks_profile chunks=" << chunks.size()
                  << " subchunks=" << payload_count
                  << " encode_ms=" << encode_ms
                  << " leveldb_ms=" << leveldb_ms << '\n';
    }
    return saved;
}

WorldIoStats BedrockWorldAdapter::take_io_stats() noexcept
{
    auto result = mIoStats;
    mIoStats = {};
    return result;
}

void BedrockWorldAdapter::defer_statistics(
    ConversionStats stats,
    std::function<void(const ConversionStats&)> callback) noexcept
{
    // A target is normally closed after one conversion. If a caller performs
    // consecutive writes, complete the earlier pipeline snapshot before
    // replacing it; the final write still receives close/archive timings.
    try {
        if (mDeferredStats) emit_deferred_statistics(true);
        mDeferredStats = std::move(stats);
        mDeferredStatisticsCallback = std::move(callback);
    } catch (...) {
        mDeferredStats.reset();
        mDeferredStatisticsCallback = {};
    }
}

void BedrockWorldAdapter::emit_deferred_statistics(
    bool success,
    std::string_view error_stage) noexcept
{
    try {
        if (!mDeferredStats) return;
        const auto io = take_io_stats();
        auto stats = std::move(*mDeferredStats);
        mDeferredStats.reset();
        stats.encode_compress_ms += io.encode_compress_ms;
        stats.leveldb_write_ms += io.leveldb_write_ms;
        stats.leveldb_close_ms += io.leveldb_close_ms;
        stats.mcworld_unpack_ms += io.mcworld_unpack_ms;
        stats.mcworld_pack_ms += io.mcworld_pack_ms;
        stats.compressed_output_bytes += io.compressed_output_bytes;
        stats.leveldb_batches += io.leveldb_batches;
        stats.temporary_spool_bytes += io.temporary_spool_bytes;
        stats.elapsed_ms += io.leveldb_close_ms + io.mcworld_pack_ms;
        stats.success = success;
        if (!success && stats.error_stage.empty()) stats.error_stage = std::string(error_stage);
        auto callback = std::move(mDeferredStatisticsCallback);
        mDeferredStatisticsCallback = {};
        if (!callback) return;
        callback(stats);
    } catch (...) {
        mDeferredStats.reset();
        mDeferredStatisticsCallback = {};
        // Telemetry must not change conversion or close semantics.
    }
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

namespace {

class WorldChunkSink final : public ChunkSink {
public:
    WorldChunkSink(
        WorldTarget& world,
        SubChunkPos start,
        std::size_t batch_capacity,
        ConversionCallbacks callbacks,
        ConversionStats& stats)
        : mWorld(world),
          mStart(start),
          mBatchCapacity(std::max<std::size_t>(batch_capacity, 1)),
          mCallbacks(std::move(callbacks)),
          mStats(stats)
    {
        mPositions.reserve(mBatchCapacity);
        mChunks.reserve(mBatchCapacity);
        mEntities.reserve(mBatchCapacity);
    }

    Result<void> push(StreamChunk&& source) override
    {
        const ChunkPos target_position{
            source.position.x + mStart.x,
            source.position.z + mStart.z
        };
        ChunkData shifted;
        shifted.layout = source.blocks.layout;
        const auto sub_y_offset = mStart.y - kMinSubChunkY;
        for (auto& [local_sub_y, sub_chunk] : source.blocks.sub_chunks) {
            const auto target_sub_y = local_sub_y + sub_y_offset;
            if (target_sub_y < kMinSubChunkY || target_sub_y > kMaxSubChunkY) {
                mStats.error_location = "chunk=(" + std::to_string(target_position.x) + "," +
                    std::to_string(target_position.z) + "),subY=" + std::to_string(target_sub_y);
                mStats.error_chunk = target_position;
                mStats.has_error_chunk = true;
                return Result<void>::failure(
                    "目标 subchunk 超出 Overworld 高度范围: " + mStats.error_location);
            }
            shifted.sub_chunks.emplace(target_sub_y, std::move(sub_chunk));
        }

        for (auto& entity : source.entities) {
            entity.pos.x += target_position.x * 16;
            entity.pos.y += mStart.y * 16 - kOverworldMinY;
            entity.pos.z += target_position.z * 16;
        }
        mPositions.push_back(target_position);
        mChunks.push_back(std::move(shifted));
        mEntities.push_back(std::move(source.entities));
        ++mReceived;
        if (mChunks.size() >= mBatchCapacity) return flush();
        return Result<void>::success();
    }

    Result<void> finish() override { return flush(); }

    std::size_t received() const noexcept { return mReceived; }
    double save_chunks_ms() const noexcept { return mSaveChunksMs; }
    double save_entities_ms() const noexcept { return mSaveEntitiesMs; }

private:
    Result<void> flush()
    {
        if (mChunks.empty()) return Result<void>::success();
        std::vector<ChunkWrite> writes;
        writes.reserve(mChunks.size());
        for (std::size_t index = 0; index < mChunks.size(); ++index) {
            writes.push_back({ mPositions[index], &mChunks[index] });
        }

        const auto save_start = Clock::now();
        auto saved = mWorld.save_chunks(writes);
        const auto saved_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - save_start).count();
        mSaveChunksMs += saved_ms;
        if (dynamic_cast<BedrockWorldAdapter*>(&mWorld) == nullptr) {
            mStats.leveldb_write_ms += static_cast<std::uint64_t>(std::max(0.0, saved_ms));
            ++mStats.leveldb_batches;
        }
        if (!saved) {
            mStats.error_chunk = mPositions.front();
            mStats.has_error_chunk = true;
            mStats.error_location = "chunk=(" + std::to_string(mPositions.front().x) + "," +
                std::to_string(mPositions.front().z) + ")";
            return Result<void>::failure("批量保存区块 " + mStats.error_location + " 失败: " + saved.error());
        }

        for (std::size_t index = 0; index < mEntities.size(); ++index) {
            if (mEntities[index].empty()) continue;
            const auto entity_start = Clock::now();
            auto entity_saved = mWorld.save_chunk_nbt(mPositions[index], mEntities[index]);
            mSaveEntitiesMs += std::chrono::duration<double, std::milli>(
                Clock::now() - entity_start).count();
            if (!entity_saved) {
                mStats.error_chunk = mPositions[index];
                mStats.has_error_chunk = true;
                mStats.error_location = "chunk=(" + std::to_string(mPositions[index].x) + "," +
                    std::to_string(mPositions[index].z) + ")";
                return Result<void>::failure(
                    "保存区块 NBT " + mStats.error_location + " 失败: " + entity_saved.error());
            }
        }

        mStats.completed_chunks += mChunks.size();
        if (mCallbacks.progress) {
            for (std::size_t index = 0; index < mChunks.size(); ++index) mCallbacks.progress();
        }
        mPositions.clear();
        mChunks.clear();
        mEntities.clear();
        return Result<void>::success();
    }

    WorldTarget& mWorld;
    SubChunkPos mStart{};
    std::size_t mBatchCapacity = 1;
    ConversionCallbacks mCallbacks;
    ConversionStats& mStats;
    std::vector<ChunkPos> mPositions;
    std::vector<ChunkData> mChunks;
    std::vector<std::vector<BlockEntity>> mEntities;
    std::size_t mReceived = 0;
    double mSaveChunksMs = 0.0;
    double mSaveEntitiesMs = 0.0;
};

} // namespace

Result<void> convert_to_world(const IStructure& structure, WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks)
{
    WorldConversionOptions options;
    options.worker_count = callbacks.worker_count;
    options.max_in_flight_chunks = callbacks.max_in_flight_chunks;
    options.soft_memory_budget_bytes = callbacks.soft_memory_budget_bytes;
    options.allow_temporary_spool = callbacks.allow_temporary_spool;
    options.collect_statistics = callbacks.collect_statistics;
    options.temporary_directory = callbacks.temporary_directory;
    options.temporary_file_limit_bytes = callbacks.temporary_file_limit_bytes;
    options.profiling = callbacks.profiling;
    options.callbacks = std::move(callbacks);
    return convert_to_world(structure, world, start, options);
}

void BedrockWorldAdapter::configure_conversion(const WorldConversionOptions& options) noexcept
{
    mConfiguredWorkerCount = options.worker_count;
    mConfiguredMaxInFlightChunks = options.max_in_flight_chunks;
    mConfiguredSoftMemoryBudget = options.soft_memory_budget_bytes;
    if (options.temporary_file_limit_bytes != 0) {
        mTemporaryFileLimitBytes = options.temporary_file_limit_bytes;
    }
}

Result<void> convert_to_world(
    const IStructure& structure,
    WorldTarget& world,
    SubChunkPos start,
    const WorldConversionOptions& options)
{
    const auto total_start = Clock::now();
    const auto elapsed_ms = [](const auto begin) {
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    };
    ConversionStats stats;
    stats.source_format = structure.id();
    stats.target_format = StructureId::MCWorld;
    const auto finish = [&](Result<void> result, std::string_view stage = {}) -> Result<void> {
        auto* adapter = dynamic_cast<BedrockWorldAdapter*>(&world);
        if (adapter && !result) adapter->mDiscardOnClose = true;
        if (adapter && options.callbacks.statistics) {
            const auto io = adapter->take_io_stats();
            stats.encode_compress_ms += io.encode_compress_ms;
            stats.leveldb_write_ms += io.leveldb_write_ms;
            stats.leveldb_close_ms += io.leveldb_close_ms;
            stats.mcworld_unpack_ms += io.mcworld_unpack_ms;
            stats.mcworld_pack_ms += io.mcworld_pack_ms;
            stats.compressed_output_bytes += io.compressed_output_bytes;
            stats.leveldb_batches += io.leveldb_batches;
            stats.temporary_spool_bytes += io.temporary_spool_bytes;
        }
        stats.success = result.ok();
        if (!result && stats.error_stage.empty()) stats.error_stage = std::string(stage);
        stats.elapsed_ms = static_cast<std::uint64_t>(elapsed_ms(total_start));
        if (options.callbacks.statistics) {
            // close()/archive packing occurs after this function returns. A
            // successful Bedrock conversion therefore publishes its final
            // snapshot from BedrockWorldAdapter::close().
            if (adapter && result.ok()) {
                adapter->defer_statistics(stats, options.callbacks.statistics);
            } else {
                try {
                    options.callbacks.statistics(stats);
                } catch (...) {
                    // Statistics are diagnostic only and must not replace the
                    // conversion error (or turn success into failure).
                }
            }
        }
        return result;
    };
    const auto& callbacks = options.callbacks;
    if (auto* adapter = dynamic_cast<BedrockWorldAdapter*>(&world)) {
        adapter->configure_conversion(options);
    }
    const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr;
    double visit_chunks_ms = 0.0;
    double save_chunks_ms = 0.0;
    double save_entities_ms = 0.0;
    const auto structure_size = structure.size();
    if (structure_size.width <= 0 || structure_size.height <= 0 ||
        structure_size.length <= 0) {
        return finish(Result<void>::failure(
            "结构尺寸必须为正数"), "size");
    }
    const auto chunk_x_count = structure_size.chunk_x_count();
    const auto chunk_z_count = structure_size.chunk_z_count();
    if (chunk_x_count <= 0 || chunk_z_count <= 0) {
        return finish(Result<void>::failure(
            "结构 chunk 尺寸无效"), "size");
    }
    const auto x_chunks = static_cast<std::size_t>(chunk_x_count);
    const auto z_chunks = static_cast<std::size_t>(chunk_z_count);
    if (z_chunks > std::numeric_limits<std::size_t>::max() / x_chunks) {
        return finish(Result<void>::failure(
            "结构 chunk 数量溢出"), "size");
    }
    const auto total_chunks = x_chunks * z_chunks;
    stats.source_chunks = total_chunks;
    if (callbacks.start) {
        callbacks.start(total_chunks);
    }

    const std::size_t format_default_batch_size =
        (structure.id() == StructureId::SchemV1 || structure.id() == StructureId::SchemV2)
        ? static_cast<std::size_t>(structure_size.chunk_x_count())
        : structure.id() == StructureId::MCFunction
        ? 8
        : 64;
    // Source materialization, the bounded queue, the sink batch, parallel BWO
    // payloads and LevelDB's WriteBatch can overlap. Charge the complete
    // pipeline rather than one ChunkData instance; this is deliberately
    // conservative so the default 450 MiB soft budget leaves headroom below
    // the external 500 MiB hard test limit for registry/NBT/allocator state.
    constexpr std::size_t kConservativeBytesPerChunk = 6u * 1024u * 1024u;
    const auto budget_chunks = std::max<std::size_t>(1,
        options.soft_memory_budget_bytes / kConservativeBytesPerChunk);
    const auto explicit_chunks = options.max_in_flight_chunks == 0
        ? format_default_batch_size : options.max_in_flight_chunks;
    const auto batch_size = std::max<std::size_t>(1,
        std::min({ format_default_batch_size, explicit_chunks, budget_chunks }));
    stats.chunk_window_peak = batch_size;
    WorldChunkSink sink(world, start, batch_size, callbacks, stats);
    auto stream = ChunkStream::from_structure_grid(
        structure, structure_size, batch_size);
    const auto pipeline_start = Clock::now();
    auto pumped = stream.pump(sink, {
        .worker_count = options.worker_count == 0 ? 1 : options.worker_count,
        .max_in_flight_chunks = batch_size,
        .soft_memory_budget_bytes = options.soft_memory_budget_bytes
    });
    const auto pipeline_ms = elapsed_ms(pipeline_start);
    save_chunks_ms = sink.save_chunks_ms();
    save_entities_ms = sink.save_entities_ms();
    visit_chunks_ms = std::max(0.0, pipeline_ms - save_chunks_ms - save_entities_ms);
    stats.chunk_materialization_ms += static_cast<std::uint64_t>(visit_chunks_ms);
    if (!pumped) {
        auto stage = std::string_view{"chunk stream"};
        if (stats.has_error_chunk) stage = "world sink";
        return finish(std::move(pumped), stage);
    }
    const auto missing = total_chunks > sink.received() ? total_chunks - sink.received() : 0;
    if (callbacks.progress) {
        for (std::size_t index = 0; index < missing; ++index) callbacks.progress();
    }
    stats.completed_chunks = total_chunks;
    if (profile) {
        std::cerr << "world_conversion_profile visit_chunks_ms=" << visit_chunks_ms
                  << " save_chunks_ms=" << save_chunks_ms
                  << " pipeline_wall_ms=" << pipeline_ms
                  << " save_entities_ms=" << save_entities_ms
                  << '\n';
    }
    return finish(Result<void>::success());
}

} // namespace water_structure
