#include "ibimport_writer.hpp"
#include "../core/bounded_thread_pool.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/format_registry.hpp>

#include <io/stream_reader.h>
#include <nlohmann/json.hpp>
#include <tag_compound.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <deque>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace water_structure {
namespace {

constexpr std::uint8_t kXorKey = 193;

std::array<std::uint8_t, 5> fixed_varint(std::uint64_t value)
{
    if (value > ((std::uint64_t{ 1 } << 35) - 1)) {
        throw std::runtime_error("IBImport 段长度超过 35-bit varint");
    }
    std::array<std::uint8_t, 5> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
        if (index + 1 != result.size()) result[index] |= 0x80u;
    }
    return result;
}

class SegmentWriter {
public:
    explicit SegmentWriter(std::ofstream& output)
        : mOutput(output), mBuffer(256 * 1024)
    {
        mLengthPosition = mOutput.tellp();
        const auto placeholder = fixed_varint(0);
        mOutput.write(reinterpret_cast<const char*>(placeholder.data()), placeholder.size());
        mOutput.put(static_cast<char>(kXorKey));
        if (!mOutput) throw std::runtime_error("写入 IBImport 段头失败");
    }

    void write(std::string_view bytes)
    {
        while (!bytes.empty()) {
            if (mPosition == mBuffer.size()) flush();
            const auto count = std::min(mBuffer.size() - mPosition, bytes.size());
            for (std::size_t index = 0; index < count; ++index) {
                mBuffer[mPosition + index] = static_cast<char>(
                    static_cast<std::uint8_t>(bytes[index]) ^ kXorKey);
            }
            mPosition += count;
            mLength += count;
            bytes.remove_prefix(count);
        }
    }

    void close()
    {
        if (mClosed) return;
        flush();
        const auto end = mOutput.tellp();
        const auto length = fixed_varint(mLength);
        mOutput.seekp(mLengthPosition);
        mOutput.write(reinterpret_cast<const char*>(length.data()), length.size());
        mOutput.seekp(end);
        if (!mOutput) throw std::runtime_error("回填 IBImport 段长度失败");
        mClosed = true;
    }

    ~SegmentWriter()
    {
        if (!mClosed) {
            try { close(); } catch (...) {}
        }
    }

private:
    void flush()
    {
        if (mPosition == 0) return;
        mOutput.write(mBuffer.data(), static_cast<std::streamsize>(mPosition));
        if (!mOutput) throw std::runtime_error("写入 IBImport 段数据失败");
        mPosition = 0;
    }

    std::ofstream& mOutput;
    std::streampos mLengthPosition{};
    std::vector<char> mBuffer;
    std::size_t mPosition = 0;
    std::uint64_t mLength = 0;
    bool mClosed = false;
};

std::uint32_t block_at(
    const ChunkData& chunk,
    RuntimeRegistry& registry,
    int x,
    int y,
    int z)
{
    const auto sub_y = floor_div(y - 64, 16);
    const auto sub = chunk.sub_chunks.find(sub_y);
    if (sub == chunk.sub_chunks.end()) return registry.air_runtime_id();
    const auto local_y = y - (sub_y * 16 + 64);
    const auto index = static_cast<std::size_t>(
        (local_y * 16 + floor_mod(z, 16)) * 16 + floor_mod(x, 16));
    return sub->second.layer0[index];
}

std::string palette_state_key(const BlockState& state)
{
    auto properties = state.states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        if (left.name != right.name) return left.name < right.name;
        if (left.type != right.type) return left.type < right.type;
        return left.value < right.value;
    });
    std::string key = state.name + '|' + std::to_string(state.version);
    for (const auto& property : properties) {
        key += '|'; key += property.name; key += ':';
        key += std::to_string(static_cast<int>(property.type));
        key += '='; key += property.value;
    }
    return key;
}

std::string block_name(BlockState state)
{
    if (state.name.empty()) return "unknown []";
    auto name = std::move(state.name);
    if (name.starts_with("minecraft:")) name.erase(0, 10);
    if (name.empty()) name = "unknown";
    auto properties = std::move(state.states);
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::string encoded = name + " [";
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) encoded += ',';
        encoded += nlohmann::json(properties[index].name).dump();
        encoded += '=';
        switch (properties[index].type) {
        case BlockStateValueType::Byte:
            encoded += properties[index].value == "0" ? "false" : "true";
            break;
        case BlockStateValueType::Short:
        case BlockStateValueType::Int:
        case BlockStateValueType::Long:
            encoded += properties[index].value;
            break;
        case BlockStateValueType::String:
            encoded += nlohmann::json(properties[index].value).dump();
            break;
        }
    }
    encoded += ']';
    return encoded;
}

std::string block_name(RuntimeRegistry& registry, std::uint32_t runtime_id)
{
    auto state = registry.state(runtime_id);
    return state ? block_name(std::move(*state)) : "unknown []";
}

class PaletteStateCache final {
public:
    explicit PaletteStateCache(RuntimeRegistry& registry) : mRegistry(registry)
    {
        mStates.emplace_back(); // handle 0 is air and is never emitted
        mByFormatted.emplace("", 0);
    }

    std::uint32_t handle(const BlockState& decoded)
    {
        const auto key = palette_state_key(decoded);
        if (const auto found = mByDecoded.find(key); found != mByDecoded.end())
            return found->second;
        auto upgraded = mRegistry.upgrade_state(decoded);
        auto name = upgraded.name;
        if (name.starts_with("minecraft:")) name.erase(0, 10);
        if (name == "air") {
            mByDecoded.emplace(key, 0);
            return 0;
        }
        auto formatted = block_name(std::move(upgraded));
        if (const auto found = mByFormatted.find(formatted); found != mByFormatted.end()) {
            mByDecoded.emplace(key, found->second);
            return found->second;
        }
        const auto result = static_cast<std::uint32_t>(mStates.size());
        mStates.push_back(std::move(formatted));
        mByFormatted.emplace(mStates.back(), result);
        mByDecoded.emplace(key, result);
        return result;
    }

    std::string_view state(std::uint32_t handle) const
    {
        return handle < mStates.size() ? std::string_view(mStates[handle]) : std::string_view{};
    }

private:
    RuntimeRegistry& mRegistry;
    std::unordered_map<std::string, std::uint32_t> mByDecoded;
    std::unordered_map<std::string, std::uint32_t> mByFormatted;
    std::vector<std::string> mStates;
};

class StringWriter final {
public:
    void write(std::string_view bytes) { mBytes.append(bytes); }
    std::string take() { return std::move(mBytes); }
private:
    std::string mBytes;
};

template<class Writer>
void write_command(
    Writer& script,
    std::string_view command,
    int x1,
    int y1,
    int z1,
    int x2,
    int y2,
    int z2,
    std::string_view state)
{
    std::string line;
    line.reserve(96 + state.size());
    line += command;
    line += " ~" + std::to_string(x1) + " ~" + std::to_string(y1) + " ~" + std::to_string(z1);
    if (command == "fill") {
        line += " ~" + std::to_string(x2) + " ~" + std::to_string(y2) + " ~" + std::to_string(z2);
    }
    line += ' ';
    line += state;
    line += "\r\n";
    script.write(line);
}

template<class Writer>
void write_dense_commands(
    Writer& script,
    std::span<const std::uint32_t> blocks,
    std::uint32_t air,
    int x_begin,
    int z_begin,
    int width,
    int depth,
    int height,
    const std::function<std::string_view(std::uint32_t)>& state_for)
{
    const auto index = [width, depth](int x, int y, int z) {
        return static_cast<std::size_t>((y * depth + z) * width + x);
    };
    std::vector<std::uint8_t> used(blocks.size(), 0);
    const auto same_free = [&](int x, int y, int z, std::uint32_t runtime) {
        return !used[index(x, y, z)] && blocks[index(x, y, z)] == runtime;
    };
    for (int y = 0; y < height; ++y) {
        for (int z = 0; z < depth; ++z) {
            for (int x = 0; x < width; ++x) {
                const auto runtime = blocks[index(x, y, z)];
                if (runtime == air || used[index(x, y, z)]) continue;
                int x2 = x;
                while (x2 + 1 < width && same_free(x2 + 1, y, z, runtime)) ++x2;
                int z2 = z;
                while (z2 + 1 < depth) {
                    bool full = true;
                    for (int xx = x; xx <= x2; ++xx) {
                        if (!same_free(xx, y, z2 + 1, runtime)) { full = false; break; }
                    }
                    if (!full) break;
                    ++z2;
                }
                int y2 = y;
                while (y2 + 1 < height) {
                    const auto next_volume = static_cast<std::int64_t>(x2 - x + 1) *
                        (z2 - z + 1) * (y2 - y + 2);
                    if (next_volume > 32768) break;
                    bool full = true;
                    for (int zz = z; zz <= z2 && full; ++zz) {
                        for (int xx = x; xx <= x2; ++xx) {
                            if (!same_free(xx, y2 + 1, zz, runtime)) { full = false; break; }
                        }
                    }
                    if (!full) break;
                    ++y2;
                }
                for (int yy = y; yy <= y2; ++yy) {
                    for (int zz = z; zz <= z2; ++zz) {
                        for (int xx = x; xx <= x2; ++xx) used[index(xx, yy, zz)] = 1;
                    }
                }
                const auto state = state_for(runtime);
                const auto volume = static_cast<std::int64_t>(x2 - x + 1) *
                    (y2 - y + 1) * (z2 - z + 1);
                const auto command = volume == 1 ? "setblock" : "fill";
                write_command(script, command,
                    x_begin + x, y, z_begin + z,
                    x_begin + x2, y2, z_begin + z2, state);
            }
        }
    }
}

void write_chunk_commands(
    SegmentWriter& script,
    const ChunkData& chunk,
    RuntimeRegistry& registry,
    int x_begin,
    int x_end,
    int z_begin,
    int z_end,
    int height,
    std::unordered_map<std::uint32_t, std::string>& state_cache)
{
    const auto width = x_end - x_begin;
    const auto depth = z_end - z_begin;
    const auto air = registry.air_runtime_id();
    const auto index = [width, depth](int x, int y, int z) {
        return static_cast<std::size_t>((y * depth + z) * width + x);
    };
    std::vector<std::uint32_t> blocks(
        static_cast<std::size_t>(width) * height * depth, air);
    for (int y = 0; y < height; ++y) {
        for (int z = z_begin; z < z_end; ++z) {
            for (int x = x_begin; x < x_end; ++x) {
                blocks[index(x - x_begin, y, z - z_begin)] =
                    block_at(chunk, registry, x, y, z);
            }
        }
    }
    const auto state_for = [&](std::uint32_t runtime) -> std::string_view {
        const auto found = state_cache.find(runtime);
        if (found != state_cache.end()) return found->second;
        auto [inserted, _] = state_cache.emplace(runtime, block_name(registry, runtime));
        return inserted->second;
    };
    write_dense_commands(
        script, blocks, air, x_begin, z_begin, width, depth, height, state_for);
}

struct PreparedPaletteChunk {
    std::vector<std::uint32_t> blocks;
    std::vector<std::string> states;
    int x_begin = 0;
    int z_begin = 0;
    int width = 0;
    int depth = 0;
    int height = 0;
};

Result<PreparedPaletteChunk> prepare_palette_chunk(
    std::span<const SubChunkPaletteData> subchunks,
    PaletteStateCache& states,
    int x_begin,
    int x_end,
    int z_begin,
    int z_end,
    int height)
{
    const auto width = x_end - x_begin;
    const auto depth = z_end - z_begin;
    const auto index = [width, depth](int x, int y, int z) {
        return static_cast<std::size_t>((y * depth + z) * width + x);
    };
    PreparedPaletteChunk result;
    result.blocks.assign(static_cast<std::size_t>(width) * height * depth, 0);
    result.states.emplace_back();
    result.x_begin = x_begin;
    result.z_begin = z_begin;
    result.width = width;
    result.depth = depth;
    result.height = height;
    std::unordered_map<std::uint32_t, std::uint32_t> local_handles;
    local_handles.emplace(0, 0);
    for (const auto& subchunk : subchunks) {
        if (subchunk.indices.size() != 4096) {
            return Result<PreparedPaletteChunk>::failure(
                "IBImport palette subchunk indices 不是 4096 项");
        }
        std::vector<std::uint32_t> handles;
        handles.reserve(subchunk.palette.size());
        for (const auto& state : subchunk.palette) {
            const auto global = states.handle(state);
            const auto found = local_handles.find(global);
            if (found != local_handles.end()) {
                handles.push_back(found->second);
                continue;
            }
            const auto local = static_cast<std::uint32_t>(result.states.size());
            result.states.emplace_back(states.state(global));
            local_handles.emplace(global, local);
            handles.push_back(local);
        }
        if (!subchunk.indices.empty() &&
            *std::max_element(subchunk.indices.begin(), subchunk.indices.end()) >=
                handles.size()) {
            return Result<PreparedPaletteChunk>::failure(
                "IBImport palette index 越界");
        }
        for (int local_y = 0; local_y < 16; ++local_y) {
            const auto y = subchunk.sub_y * 16 + 64 + local_y;
            if (y < 0 || y >= height) continue;
            for (int z = 0; z < depth; ++z) {
                for (int x = 0; x < width; ++x) {
                    const auto native = static_cast<std::size_t>(
                        x * 256 + local_y * 16 + z);
                    result.blocks[index(x, y, z)] =
                        handles[subchunk.indices[native]];
                }
            }
        }
    }
    return Result<PreparedPaletteChunk>::success(std::move(result));
}

std::string encode_palette_chunk(PreparedPaletteChunk input)
{
    StringWriter output;
    const auto state_for = [&](std::uint32_t handle) -> std::string_view {
        return handle < input.states.size() ? input.states[handle] : std::string_view{};
    };
    write_dense_commands(
        output, input.blocks, 0, input.x_begin, input.z_begin,
        input.width, input.depth, input.height, state_for);
    return output.take();
}

std::string base64(std::string_view input)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto count = std::min<std::size_t>(3, input.size() - offset);
        std::uint32_t value = static_cast<std::uint8_t>(input[offset]) << 16;
        if (count > 1) value |= static_cast<std::uint8_t>(input[offset + 1]) << 8;
        if (count > 2) value |= static_cast<std::uint8_t>(input[offset + 2]);
        output.push_back(alphabet[(value >> 18) & 63]);
        output.push_back(alphabet[(value >> 12) & 63]);
        output.push_back(count > 1 ? alphabet[(value >> 6) & 63] : '=');
        output.push_back(count > 2 ? alphabet[value & 63] : '=');
    }
    return output;
}

const nbt::value* find_value(const nbt::tag_compound& compound, const char* key)
{
    return compound.has_key(key) ? &compound.at(key) : nullptr;
}

std::string string_value(const nbt::value* value)
{
    return value && value->get_type() == nbt::tag_type::String
        ? value->as<nbt::tag_string>().get()
        : std::string{};
}

std::int32_t int_value(const nbt::value* value)
{
    if (!value) return 0;
    switch (value->get_type()) {
    case nbt::tag_type::Byte: return value->as<nbt::tag_byte>().get();
    case nbt::tag_type::Short: return value->as<nbt::tag_short>().get();
    case nbt::tag_type::Int: return value->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return static_cast<std::int32_t>(value->as<nbt::tag_long>().get());
    default: return 0;
    }
}

bool command_block(const nbt::tag_compound& compound)
{
    return string_value(find_value(compound, "id")) == "CommandBlock";
}

} // namespace

Result<void> write_ibimport(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path,
    const ConversionOptions& options)
{
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0 || size.volume() <= 0) {
        return Result<void>::failure("IBImport 输出尺寸无效");
    }
    auto output_buffer = std::make_unique<char[]>(1024 * 1024);
    std::ofstream output;
    output.rdbuf()->pubsetbuf(output_buffer.get(), 1024 * 1024);
    output.open(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 IBImport: " + output_path.string());
    try {
        using Clock = std::chrono::steady_clock;
        const auto elapsed_ms = [](Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr ||
            std::getenv("WATER_STRUCTURE_IBIMPORT_PROFILE") != nullptr;
        const auto total_start = Clock::now();
        double palette_total_ms = 0.0;
        double palette_prepare_ms = 0.0;
        double palette_encode_ms = 0.0;
        double generic_load_ms = 0.0;
        double generic_encode_ms = 0.0;
        double nbt_ms = 0.0;
        output.write("IBImport ", 9);
        SegmentWriter script(output);
        std::unordered_map<std::uint32_t, std::string> state_cache;
        state_cache.reserve(256);
        PaletteStateCache palette_states(registry);
        constexpr int kChunkBatchSize = 32;
        std::vector<ChunkPos> positions;
        positions.reserve(kChunkBatchSize);
        const auto worker_count = std::max<std::size_t>(
            1, options.thread_count == 0 ? 3 : options.thread_count);
        const auto max_in_flight = std::max<std::size_t>(
            1, options.max_in_flight_tasks == 0
                ? worker_count * 2
                : options.max_in_flight_tasks);
        detail::BoundedThreadPool encode_pool(worker_count, max_in_flight);
        struct EncodedChunk {
            std::string commands;
            double encode_ms = 0.0;
        };
        std::deque<std::future<EncodedChunk>> pending;
        const auto drain_one = [&]() {
            auto encoded = pending.front().get();
            pending.pop_front();
            palette_encode_ms += encoded.encode_ms;
            script.write(encoded.commands);
        };
        bool palette_enabled = true;
        bool palette_used = false;
        for (int chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
            for (int batch_x = 0; batch_x < size.chunk_x_count();
                 batch_x += kChunkBatchSize) {
                const auto batch_end = std::min(
                    size.chunk_x_count(), batch_x + kChunkBatchSize);
                positions.clear();
                for (auto chunk_x = batch_x; chunk_x < batch_end; ++chunk_x)
                    positions.push_back({ chunk_x, chunk_z });

                if (palette_enabled) {
                    bool callback_called = false;
                    const auto palette_start = Clock::now();
                    auto palette = structure.visit_chunk_palettes(
                        positions,
                        [&](ChunkPos position,
                            std::span<const SubChunkPaletteData> subchunks) -> Result<void> {
                            callback_called = true;
                            const auto prepare_start = Clock::now();
                            auto prepared = prepare_palette_chunk(
                                subchunks, palette_states,
                                position.x * 16,
                                std::min(size.width, (position.x + 1) * 16),
                                position.z * 16,
                                std::min(size.length, (position.z + 1) * 16),
                                size.height);
                            palette_prepare_ms += elapsed_ms(prepare_start);
                            if (!prepared) return Result<void>::failure(prepared.error());
                            pending.push_back(encode_pool.submit(
                                [input = std::move(prepared).value()]() mutable {
                                    const auto start = Clock::now();
                                    auto commands = encode_palette_chunk(std::move(input));
                                    const auto duration = std::chrono::duration<double, std::milli>(
                                        Clock::now() - start).count();
                                    return EncodedChunk{ std::move(commands), duration };
                                }));
                            if (pending.size() >= max_in_flight) drain_one();
                            return Result<void>::success();
                        });
                    palette_total_ms += elapsed_ms(palette_start);
                    if (palette) {
                        palette_used = true;
                        continue;
                    }
                    if (callback_called || palette_used) {
                        throw std::runtime_error(
                            "IBImport palette 流水线中途失败: " + palette.error());
                    }
                    palette_enabled = false;
                }

                const auto load_start = Clock::now();
                auto chunks = structure.get_chunks_layer0(positions);
                generic_load_ms += elapsed_ms(load_start);
                if (!chunks) throw std::runtime_error("生成 IBImport chunk 失败: " + chunks.error());
                const auto encode_start = Clock::now();
                for (const auto position : positions) {
                    const auto found = chunks.value().find(position);
                    const ChunkData empty;
                    const auto& chunk = found == chunks.value().end() ? empty : found->second;
                    write_chunk_commands(script, chunk, registry,
                        position.x * 16,
                        std::min(size.width, (position.x + 1) * 16),
                        position.z * 16,
                        std::min(size.length, (position.z + 1) * 16),
                        size.height, state_cache);
                }
                generic_encode_ms += elapsed_ms(encode_start);
                structure.release_cached_chunks();
            }
        }
        while (!pending.empty()) drain_one();
        script.close();

        const auto nbt_start = Clock::now();
        SegmentWriter json_segment(output);
        json_segment.write("[\n");
        bool wrote_any = false;
        for (int chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
            for (int chunk_x = 0; chunk_x < size.chunk_x_count(); ++chunk_x) {
                const ChunkPos chunk_position{ chunk_x, chunk_z };
                const std::array<ChunkPos, 1> requested{ chunk_position };
                auto entities = structure.get_chunk_nbt(requested);
                if (!entities) {
                    throw std::runtime_error("生成 IBImport NBT 失败: " + entities.error());
                }
                const auto found_entities = entities.value().find(chunk_position);
                if (found_entities == entities.value().end() || found_entities->second.empty()) {
                    structure.release_cached_chunks();
                    continue;
                }

                std::optional<ChunkMap> block_chunk;
                for (const auto& entity : found_entities->second) {
                    if (entity.payload.empty()) continue;
                    const std::string bytes(entity.payload.begin(), entity.payload.end());
                    std::istringstream input(bytes, std::ios::binary);
                    auto [_, compound] = nbt::io::read_compound(input, endian::little);
                    if (!command_block(*compound)) continue;
                    const auto x = chunk_position.x * 16 + entity.pos.x;
                    const auto y = entity.pos.y - kOverworldMinY;
                    const auto z = chunk_position.z * 16 + entity.pos.z;
                    if (x < 0 || x >= size.width || y < 0 || y >= size.height ||
                        z < 0 || z >= size.length) continue;
                    if (!block_chunk) {
                        auto loaded = structure.get_chunks_layer0(requested);
                        if (!loaded) throw std::runtime_error(loaded.error());
                        block_chunk.emplace(std::move(loaded).value());
                    }
                    const auto chunk_it = block_chunk->find(chunk_position);
                    const auto runtime_id = chunk_it == block_chunk->end()
                        ? registry.air_runtime_id()
                        : block_at(chunk_it->second, registry, x, y, z);
                    auto state = registry.state(runtime_id);
                    auto mode = 0;
                    if (state) {
                        auto name = state->name;
                        if (name.starts_with("minecraft:")) name.erase(0, 10);
                        if (name == "chain_command_block") mode = 1;
                        else if (name == "repeating_command_block") mode = 2;
                    }
                    nlohmann::json item{
                        { "posX", "~" + std::to_string(x) },
                        { "posY", "~" + std::to_string(y) },
                        { "posZ", "~" + std::to_string(z) },
                        { "CommandMessage", base64(string_value(find_value(*compound, "Command"))) },
                        { "Commandtitle", string_value(find_value(*compound, "CustomName")) },
                        { "mode", mode },
                        { "isTime", int_value(find_value(*compound, "TickDelay")) },
                        { "Conditional", int_value(find_value(*compound, "conditionalMode")) != 0 ? 1 : 0 }
                    };
                    if (int_value(find_value(*compound, "auto")) == 0) item["isRedstone"] = true;
                    if (wrote_any) json_segment.write(",\n");
                    json_segment.write("    " + item.dump());
                    wrote_any = true;
                }
                block_chunk.reset();
                structure.release_cached_chunks();
            }
        }
        if (wrote_any) json_segment.write("\n");
        json_segment.write("]\n");
        json_segment.close();
        nbt_ms = elapsed_ms(nbt_start);
        output.close();
        if (!output) return Result<void>::failure("写入 IBImport 失败");
        if (profile) {
            std::cerr << "ibimport_writer_profile palette_used=" << (palette_used ? 1 : 0)
                      << " palette_total_ms=" << palette_total_ms
                      << " palette_prepare_ms=" << palette_prepare_ms
                      << " palette_encode_ms=" << palette_encode_ms
                      << " generic_load_ms=" << generic_load_ms
                      << " generic_encode_ms=" << generic_encode_ms
                      << " nbt_ms=" << nbt_ms
                      << " total_ms=" << elapsed_ms(total_start) << '\n';
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("序列化 IBImport 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
