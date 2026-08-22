#include "bdx_writer.hpp"

#include <WaterStructure/coordinates.hpp>

#include <brotli/encode.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kChunkBatchSize = 32;
constexpr std::size_t kCommandBufferSize = 256 * 1024;
constexpr std::size_t kFileBufferSize = 1024 * 1024;
constexpr std::uint32_t kBrotliQuality = 6;

class BrotliCommandWriter final {
public:
    explicit BrotliCommandWriter(
        const std::filesystem::path& path,
        std::size_t command_buffer_size = kCommandBufferSize,
        std::size_t file_buffer_size = kFileBufferSize)
        : mCommandBufferLimit(std::max<std::size_t>(command_buffer_size, 4096)),
          mFileBuffer(std::max<std::size_t>(file_buffer_size, 4096)),
          mOutputBuffer(mCommandBufferLimit),
          mEncoder(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr),
              &BrotliEncoderDestroyInstance)
    {
        if (!mEncoder) throw std::runtime_error("无法创建 BDX Brotli encoder");
        if (BrotliEncoderSetParameter(
                mEncoder.get(), BROTLI_PARAM_QUALITY, kBrotliQuality) != BROTLI_TRUE) {
            throw std::runtime_error("无法设置 BDX Brotli quality");
        }
        mStaging.reserve(mCommandBufferLimit);
        mOutput.rdbuf()->pubsetbuf(
            mFileBuffer.data(), static_cast<std::streamsize>(mFileBuffer.size()));
        mOutput.open(path, std::ios::binary | std::ios::trunc);
        if (!mOutput) throw std::runtime_error("无法创建 BDX: " + path.string());
        mOutput.write("BD@", 3);
        if (!mOutput) throw std::runtime_error("写入 BDX 文件头失败: " + path.string());
    }

    void append_byte(std::uint8_t value)
    {
        if (mStaging.size() == mCommandBufferLimit) flush_staging();
        mStaging.push_back(value);
    }

    void append(std::span<const std::uint8_t> bytes)
    {
        while (!bytes.empty()) {
            if (mStaging.size() == mCommandBufferLimit) flush_staging();
            const auto count = std::min(bytes.size(), mCommandBufferLimit - mStaging.size());
            mStaging.insert(mStaging.end(), bytes.begin(), bytes.begin() + count);
            bytes = bytes.subspan(count);
        }
    }

    void append_text(std::string_view text)
    {
        append(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    void finish()
    {
        if (mFinished) return;
        flush_staging();
        while (BrotliEncoderIsFinished(mEncoder.get()) != BROTLI_TRUE) {
            pump(BROTLI_OPERATION_FINISH, {});
        }
        mOutput.flush();
        if (!mOutput) throw std::runtime_error("写入 BDX 压缩流失败");
        mFinished = true;
    }

private:
    using Encoder = std::unique_ptr<BrotliEncoderState, decltype(&BrotliEncoderDestroyInstance)>;

    void flush_staging()
    {
        if (mStaging.empty()) return;
        pump(BROTLI_OPERATION_PROCESS, mStaging);
        mStaging.clear();
    }

    void pump(BrotliEncoderOperation operation, std::span<const std::uint8_t> input)
    {
        auto available_input = input.size();
        auto* next_input = input.data();
        do {
            auto available_output = mOutputBuffer.size();
            auto* next_output = mOutputBuffer.data();
            if (BrotliEncoderCompressStream(
                    mEncoder.get(),
                    operation,
                    &available_input,
                    &next_input,
                    &available_output,
                    &next_output,
                    nullptr) != BROTLI_TRUE) {
                throw std::runtime_error("BDX writer Brotli 流式压缩失败");
            }
            const auto produced = mOutputBuffer.size() - available_output;
            if (produced != 0) {
                mOutput.write(
                    reinterpret_cast<const char*>(mOutputBuffer.data()),
                    static_cast<std::streamsize>(produced));
                if (!mOutput) throw std::runtime_error("写入 BDX 压缩数据失败");
            }
        } while (available_input != 0 ||
                 BrotliEncoderHasMoreOutput(mEncoder.get()) == BROTLI_TRUE);
    }

    std::size_t mCommandBufferLimit = 0;
    std::vector<char> mFileBuffer;
    std::vector<std::uint8_t> mStaging;
    std::vector<std::uint8_t> mOutputBuffer;
    Encoder mEncoder;
    std::ofstream mOutput;
    bool mFinished = false;
};

void append_u16(BrotliCommandWriter& output, std::uint16_t value)
{
    output.append_byte(static_cast<std::uint8_t>(value >> 8));
    output.append_byte(static_cast<std::uint8_t>(value));
}

void append_u32(BrotliCommandWriter& output, std::uint32_t value)
{
    output.append_byte(static_cast<std::uint8_t>(value >> 24));
    output.append_byte(static_cast<std::uint8_t>(value >> 16));
    output.append_byte(static_cast<std::uint8_t>(value >> 8));
    output.append_byte(static_cast<std::uint8_t>(value));
}

void append_cstring(BrotliCommandWriter& output, std::string_view value)
{
    output.append_text(value);
    output.append_byte(0);
}

void append_move_axis(
    BrotliCommandWriter& output,
    std::int64_t difference,
    std::uint8_t increment,
    std::uint8_t decrement,
    std::uint8_t int8_command,
    std::uint8_t int16_command,
    std::uint8_t int32_command)
{
    if (difference == 0) return;
    // A pair of int32 coordinates can be farther apart than one signed int32
    // delta. Emit bounded moves until the remainder is representable instead
    // of overflowing `target - cursor` before command selection.
    while (difference > std::numeric_limits<std::int32_t>::max()) {
        output.append_byte(int32_command);
        append_u32(output, static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max()));
        difference -= std::numeric_limits<std::int32_t>::max();
    }
    while (difference < std::numeric_limits<std::int32_t>::min()) {
        output.append_byte(int32_command);
        append_u32(output, static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::min()));
        difference -= std::numeric_limits<std::int32_t>::min();
    }
    if (difference == 1) {
        output.append_byte(increment);
    } else if (difference == -1) {
        output.append_byte(decrement);
    } else if (difference >= std::numeric_limits<std::int8_t>::min() &&
        difference <= std::numeric_limits<std::int8_t>::max()) {
        output.append_byte(int8_command);
        output.append_byte(static_cast<std::uint8_t>(static_cast<std::int8_t>(difference)));
    } else if (difference >= std::numeric_limits<std::int16_t>::min() &&
        difference <= std::numeric_limits<std::int16_t>::max()) {
        output.append_byte(int16_command);
        append_u16(output, static_cast<std::uint16_t>(static_cast<std::int16_t>(difference)));
    } else {
        output.append_byte(int32_command);
        append_u32(output, static_cast<std::uint32_t>(difference));
    }
}

void append_move(BrotliCommandWriter& output, BlockPos& cursor, BlockPos target)
{
    append_move_axis(output,
        static_cast<std::int64_t>(target.x) - cursor.x, 14, 15, 28, 20, 21);
    append_move_axis(output,
        static_cast<std::int64_t>(target.y) - cursor.y, 16, 17, 29, 22, 23);
    append_move_axis(output,
        static_cast<std::int64_t>(target.z) - cursor.z, 18, 19, 30, 24, 25);
    cursor = target;
}

std::string escaped(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

std::string state_string(const BlockState& state)
{
    if (state.states.empty()) return "[]";
    auto properties = state.states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::string result = "[";
    for (std::size_t i = 0; i < properties.size(); ++i) {
        if (i != 0) result.push_back(',');
        const auto& property = properties[i];
        result += "\"" + escaped(property.name) + "\"=";
        if (property.type == BlockStateValueType::String) {
            result += "\"" + escaped(property.value) + "\"";
        } else if (property.type == BlockStateValueType::Byte) {
            result += property.value == "0" ? "false" : "true";
        } else {
            result += property.value;
        }
    }
    result.push_back(']');
    return result;
}

const BlockLayer* layer_at(const ChunkMap& chunks, ChunkPos chunk, std::int32_t sub_y)
{
    const auto chunk_it = chunks.find(chunk);
    if (chunk_it == chunks.end()) return nullptr;
    const auto sub_it = chunk_it->second.sub_chunks.find(sub_y);
    return sub_it == chunk_it->second.sub_chunks.end() ? nullptr : &sub_it->second.layer0;
}

} // namespace

Result<void> write_bdx(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path,
    const ConversionOptions& options)
{
    try {
        const auto size = structure.size();
        if (size.width <= 0 || size.height <= 0 || size.length <= 0) {
            return Result<void>::failure("BDX writer: 结构尺寸无效");
        }
        const auto budget = options.soft_memory_budget_bytes;
        if (budget < 64u * 1024u) {
            return Result<void>::failure("BDX writer soft_memory_budget 至少需要 64 KiB");
        }
        const auto requested_batch = options.max_in_flight_chunks == 0
            ? kChunkBatchSize
            : std::max<std::size_t>(1, options.max_in_flight_chunks);
        const auto subchunk_count = static_cast<std::uint64_t>(
            (static_cast<std::int64_t>(size.height) + 15) / 16);
        constexpr std::uint64_t kChunkOverheadBytes = 64u * 1024u;
        constexpr std::uint64_t kSubchunkBytes =
            2ull * 4096ull * sizeof(std::uint32_t);
        if (subchunk_count >
            (std::numeric_limits<std::uint64_t>::max() - kChunkOverheadBytes) /
                kSubchunkBytes) {
            return Result<void>::failure("BDX writer chunk memory estimate overflow");
        }
        const auto estimated_chunk_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
            kChunkOverheadBytes + subchunk_count * kSubchunkBytes,
            std::numeric_limits<std::size_t>::max()));
        const auto budget_batch = (budget / 2) / std::max<std::size_t>(1, estimated_chunk_bytes);
        if (budget_batch == 0) {
            return Result<void>::failure(
                "BDX writer 单 chunk 超过 soft_memory_budget");
        }
        const auto batch_size = std::min({
            kChunkBatchSize, requested_batch, budget_batch });
        const auto buffer_budget = std::max<std::size_t>(4096, budget / 8);
        BrotliCommandWriter output(output_path,
            std::min<std::size_t>(kCommandBufferSize, buffer_budget),
            std::min<std::size_t>(kFileBufferSize, buffer_budget));
        output.append_text("BDX");
        output.append_byte(0);
        output.append_byte(0);

        std::vector<ChunkPos> positions;
        positions.reserve(batch_size);
        std::unordered_map<std::uint32_t, std::pair<std::uint16_t, std::uint16_t>> palette;
        BlockPos cursor{};
        auto place = [&](std::uint32_t runtime_id, BlockPos position) -> Result<void> {
            append_move(output, cursor, position);
            auto found = palette.find(runtime_id);
            if (found == palette.end()) {
                if (palette.size() >= (std::numeric_limits<std::uint16_t>::max() + 1ull) / 2ull) {
                    return Result<void>::failure("BDX writer palette 超过 constant string uint16 范围");
                }
                auto state = registry.state(runtime_id).value_or(BlockState{ "minecraft:unknown", {}, 0 });
                const auto name_id = static_cast<std::uint16_t>(palette.size() * 2);
                const auto states_id = static_cast<std::uint16_t>(name_id + 1);
                output.append_byte(1);
                append_cstring(output, state.name);
                output.append_byte(1);
                append_cstring(output, state_string(state));
                found = palette.emplace(runtime_id, std::pair{ name_id, states_id }).first;
            }
            output.append_byte(5);
            append_u16(output, found->second.first);
            append_u16(output, found->second.second);
            return Result<void>::success();
        };

        const auto minimum_sub_y = floor_div(-64, 16);
        const auto maximum_sub_y = floor_div(size.height - 1 - 64, 16);
        for (std::int32_t chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
            for (std::int32_t batch_x = 0;
                 batch_x < size.chunk_x_count();
                 batch_x += static_cast<std::int32_t>(batch_size)) {
                const auto batch_end = std::min<std::int32_t>(
                    size.chunk_x_count(),
                    batch_x + static_cast<std::int32_t>(batch_size));
                positions.clear();
                for (auto chunk_x = batch_x; chunk_x < batch_end; ++chunk_x) {
                    positions.push_back({ chunk_x, chunk_z });
                }
                auto chunks = structure.get_chunks_layer0(positions);
                structure.release_cached_chunks();
                if (!chunks) {
                    return Result<void>::failure(
                        "BDX writer 获取 chunks 失败: " + chunks.error());
                }

                for (auto chunk_x = batch_x; chunk_x < batch_end; ++chunk_x) {
                    for (std::int32_t sub_y = minimum_sub_y;
                         sub_y <= maximum_sub_y;
                         ++sub_y) {
                        const auto* layer = layer_at(
                            chunks.value(), { chunk_x, chunk_z }, sub_y);
                        if (!layer) continue;
                        for (std::int32_t local_x = 0; local_x < 16; ++local_x) {
                            const auto x = chunk_x * 16 + local_x;
                            if (x >= size.width) break;
                            for (std::int32_t local_y = 0; local_y < 16; ++local_y) {
                                const auto y = sub_y * 16 + 64 + local_y;
                                if (y < 0 || y >= size.height) continue;
                                for (std::int32_t local_z = 0; local_z < 16; ++local_z) {
                                    const auto z = chunk_z * 16 + local_z;
                                    if (z >= size.length) break;
                                    const auto index = static_cast<std::size_t>(
                                        (local_y * 16 + local_z) * 16 + local_x);
                                    const auto runtime_id = (*layer)[index];
                                    if (runtime_id == registry.air_runtime_id()) continue;
                                    const auto placed = place(runtime_id, { x, y, z });
                                    if (!placed) return placed;
                                }
                            }
                        }
                    }
                }
            }
        }
        output.append_byte(88);
        output.finish();
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("BDX writer 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
