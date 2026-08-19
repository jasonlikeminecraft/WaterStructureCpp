#include "schem.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include "../core/bounded_thread_pool.hpp"

#include <io/izlibstream.h>
#include <io/stream_reader.h>
#include <tag_compound.h>
#include <tag_primitive.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <system_error>
#include <vector>

namespace water_structure {

namespace {

using NbtReader = nbt::io::stream_reader;
constexpr std::size_t kIndexStride = 256;
constexpr std::uint16_t kMissingChunkOffset = std::numeric_limits<std::uint16_t>::max();

std::size_t schem_materialize_worker_count(std::size_t chunk_count)
{
    if (chunk_count < 2) return 1;
    std::size_t configured = 2;
    if (const auto* text = std::getenv("WATER_STRUCTURE_SCHEM_MATERIALIZE_THREADS");
        text && *text) {
        std::size_t parsed = 0;
        const auto* end = text + std::char_traits<char>::length(text);
        const auto [next, error] = std::from_chars(text, end, parsed);
        if (error == std::errc{} && next == end) configured = parsed;
    }
    if (configured == 0) return 1;
    return std::min<std::size_t>(std::min(configured, chunk_count), 8);
}

class EncodedReader {
public:
    explicit EncodedReader(const std::filesystem::path& path)
        : mInput(path, std::ios::binary), mBuffer(1u << 20) {}
    bool good() const noexcept { return static_cast<bool>(mInput); }
    bool seek(std::uint64_t offset)
    {
        mInput.clear();
        mInput.seekg(static_cast<std::streamoff>(offset));
        mPosition = mSize = 0;
        return static_cast<bool>(mInput);
    }
    bool get(std::uint8_t& value)
    {
        if (mPosition == mSize) {
            mInput.read(mBuffer.data(), static_cast<std::streamsize>(mBuffer.size()));
            mSize = static_cast<std::size_t>(mInput.gcount());
            mPosition = 0;
            if (mSize == 0) return false;
        }
        value = static_cast<std::uint8_t>(mBuffer[mPosition++]);
        return true;
    }
    bool read_varint_u32(std::uint32_t& result)
    {
        // The Schem palette is normally compact: the overwhelming majority of
        // indices fit in one byte.  Decode directly from the current buffer
        // for that case and for complete multi-byte varints.  Only a varint
        // crossing the buffer boundary takes the checked fallback below.
        if (mPosition < mSize) {
            const auto remaining = mSize - mPosition;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(mBuffer.data()) + mPosition;
            const auto first = bytes[0];
            if ((first & 0x80u) == 0) {
                result = first;
                ++mPosition;
                return true;
            }
            if (remaining >= 5) {
                std::uint64_t value = first & 0x7fu;
                int shift = 7;
                for (std::size_t index = 1; index < 5; ++index, shift += 7) {
                    const auto byte = bytes[index];
                    value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
                    if ((byte & 0x80u) == 0) {
                        if (value > std::numeric_limits<std::uint32_t>::max()) return false;
                        result = static_cast<std::uint32_t>(value);
                        mPosition += index + 1;
                        return true;
                    }
                }
                return false;
            }
        }
        std::uint64_t value = 0;
        int shift = 0;
        for (;;) {
            if (mPosition == mSize) {
                mInput.read(mBuffer.data(), static_cast<std::streamsize>(mBuffer.size()));
                mSize = static_cast<std::size_t>(mInput.gcount());
                mPosition = 0;
                if (mSize == 0) return false;
            }
            const auto byte = static_cast<std::uint8_t>(mBuffer[mPosition++]);
            value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) {
                if (value > std::numeric_limits<std::uint32_t>::max()) return false;
                result = static_cast<std::uint32_t>(value);
                return true;
            }
            shift += 7;
            if (shift >= 35) return false;
        }
    }
    bool read_varints_u16(
        std::span<std::uint16_t> output,
        std::uint16_t max_value)
    {
        constexpr std::uint64_t kContinuationBits = 0x8080808080808080ull;
        std::size_t output_position = 0;
        while (output_position < output.size()) {
            if (mPosition == mSize) {
                mInput.read(mBuffer.data(), static_cast<std::streamsize>(mBuffer.size()));
                mSize = static_cast<std::size_t>(mInput.gcount());
                mPosition = 0;
                if (mSize == 0) return false;
            }
            const auto available = std::min(
                mSize - mPosition,
                output.size() - output_position);
            std::size_t bulk_count = 0;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                mBuffer.data() + mPosition);
            while (bulk_count + sizeof(std::uint64_t) <= available) {
                std::uint64_t packed = 0;
                std::memcpy(&packed, bytes + bulk_count, sizeof(packed));
                if ((packed & kContinuationBits) != 0) break;
                for (std::size_t index = 0; index < sizeof(packed); ++index) {
                    const auto value = bytes[bulk_count + index];
                    if (value > max_value) return false;
                    output[output_position + bulk_count + index] = value;
                }
                bulk_count += sizeof(packed);
            }
            if (bulk_count != 0) {
                mPosition += bulk_count;
                output_position += bulk_count;
                continue;
            }
            std::uint32_t value = 0;
            if (!read_varint_u32(value) || value > max_value) return false;
            output[output_position++] = static_cast<std::uint16_t>(value);
        }
        return true;
    }
    bool read_exact(std::span<std::uint8_t> output)
    {
        mPosition = mSize = 0;
        mInput.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
        return static_cast<std::size_t>(mInput.gcount()) == output.size();
    }
private:
    std::ifstream mInput;
    std::vector<char> mBuffer;
    std::size_t mPosition = 0;
    std::size_t mSize = 0;
};

// Bounded reader over the BlockData payload inside the live gzip/NBT stream.
// It never reads beyond the byte-array length, so the NBT parser can resume
// immediately after the direct world conversion and validate trailing tags.
class LimitedEncodedReader {
public:
    LimitedEncodedReader(std::istream& input, std::uint64_t length)
        : mInput(input), mRemaining(length), mBuffer(1u << 20) {}

    bool get(std::uint8_t& value)
    {
        if (mPosition == mSize && !refill()) return false;
        value = static_cast<std::uint8_t>(mBuffer[mPosition++]);
        return true;
    }

    bool read_varint_u32(std::uint32_t& result)
    {
        if (mPosition < mSize) {
            const auto remaining = mSize - mPosition;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(mBuffer.data()) + mPosition;
            const auto first = bytes[0];
            if ((first & 0x80u) == 0) {
                result = first;
                ++mPosition;
                return true;
            }
            if (remaining >= 5) {
                std::uint64_t value = first & 0x7fu;
                int shift = 7;
                for (std::size_t index = 1; index < 5; ++index, shift += 7) {
                    const auto byte = bytes[index];
                    value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
                    if ((byte & 0x80u) == 0) {
                        if (value > std::numeric_limits<std::uint32_t>::max()) return false;
                        result = static_cast<std::uint32_t>(value);
                        mPosition += index + 1;
                        return true;
                    }
                }
                return false;
            }
        }
        std::uint64_t value = 0;
        int shift = 0;
        for (;;) {
            std::uint8_t byte = 0;
            if (!get(byte)) return false;
            value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) {
                if (value > std::numeric_limits<std::uint32_t>::max()) return false;
                result = static_cast<std::uint32_t>(value);
                return true;
            }
            shift += 7;
            if (shift >= 35) return false;
        }
    }

    bool read_varints_u16(
        std::span<std::uint16_t> output,
        std::uint16_t max_value)
    {
        // Schem palettes overwhelmingly use indices below 128. Decode eight
        // such one-byte varints per loop and only fall back to the fully
        // checked scalar decoder when a continuation bit is present. memcpy
        // keeps the 64-bit probe aligned and valid on every supported target.
        constexpr std::uint64_t kContinuationBits = 0x8080808080808080ull;
        std::size_t output_position = 0;
        while (output_position < output.size()) {
            if (mPosition == mSize && !refill()) return false;

            const auto available = std::min(
                mSize - mPosition,
                output.size() - output_position);
            std::size_t bulk_count = 0;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                mBuffer.data() + mPosition);
            while (bulk_count + sizeof(std::uint64_t) <= available) {
                std::uint64_t packed = 0;
                std::memcpy(&packed, bytes + bulk_count, sizeof(packed));
                if ((packed & kContinuationBits) != 0) break;

                for (std::size_t index = 0; index < sizeof(packed); ++index) {
                    const auto value = bytes[bulk_count + index];
                    if (value > max_value) return false;
                    output[output_position + bulk_count + index] = value;
                }
                bulk_count += sizeof(packed);
            }
            if (bulk_count != 0) {
                mPosition += bulk_count;
                output_position += bulk_count;
                continue;
            }

            std::uint32_t value = 0;
            if (!read_varint_u32(value) || value > max_value) return false;
            output[output_position++] = static_cast<std::uint16_t>(value);
        }
        return true;
    }

    std::uint64_t remaining() const noexcept
    {
        return mRemaining + (mSize - mPosition);
    }

private:
    bool refill()
    {
        mPosition = mSize = 0;
        if (mRemaining == 0) return false;
        const auto amount = static_cast<std::streamsize>(
            std::min<std::uint64_t>(mRemaining, mBuffer.size()));
        mInput.read(mBuffer.data(), amount);
        if (mInput.gcount() != amount) return false;
        mRemaining -= static_cast<std::uint64_t>(amount);
        mSize = static_cast<std::size_t>(amount);
        return true;
    }

    std::istream& mInput;
    std::uint64_t mRemaining = 0;
    std::vector<char> mBuffer;
    std::size_t mPosition = 0;
    std::size_t mSize = 0;
};

bool is_integer_type(nbt::tag_type type) noexcept
{
    return type == nbt::tag_type::Byte || type == nbt::tag_type::Short ||
        type == nbt::tag_type::Int || type == nbt::tag_type::Long;
}

std::optional<std::int32_t> read_integer(NbtReader& reader, nbt::tag_type type)
{
    auto payload = reader.read_payload(type);
    switch (type) {
    case nbt::tag_type::Byte: return payload->as<nbt::tag_byte>().get();
    case nbt::tag_type::Short: return payload->as<nbt::tag_short>().get();
    case nbt::tag_type::Int: return payload->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: {
        const auto value = payload->as<nbt::tag_long>().get();
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return static_cast<std::int32_t>(value);
    }
    default: return std::nullopt;
    }
}

void skip_bytes(std::istream& input, std::uint64_t count)
{
    std::vector<char> buffer(1u << 16);
    while (count != 0) {
        const auto amount = static_cast<std::streamsize>(std::min<std::uint64_t>(count, buffer.size()));
        input.read(buffer.data(), amount);
        if (input.gcount() != amount) throw nbt::io::input_error("NBT 数据意外结束");
        count -= static_cast<std::uint64_t>(amount);
    }
}

void skip_payload(NbtReader& reader, nbt::tag_type type)
{
    auto& input = reader.get_istr();
    switch (type) {
    case nbt::tag_type::Byte: skip_bytes(input, 1); break;
    case nbt::tag_type::Short: skip_bytes(input, 2); break;
    case nbt::tag_type::Int: skip_bytes(input, 4); break;
    case nbt::tag_type::Long: skip_bytes(input, 8); break;
    case nbt::tag_type::Float: skip_bytes(input, 4); break;
    case nbt::tag_type::Double: skip_bytes(input, 8); break;
    case nbt::tag_type::Byte_Array: {
        std::int32_t length = 0; reader.read_num(length);
        if (length < 0) throw nbt::io::input_error("NBT 数组长度无效");
        skip_bytes(input, static_cast<std::uint32_t>(length));
        break;
    }
    case nbt::tag_type::String: {
        std::uint16_t length = 0; reader.read_num(length); skip_bytes(input, length); break;
    }
    case nbt::tag_type::List: {
        const auto element_type = reader.read_type();
        std::int32_t length = 0; reader.read_num(length);
        if (length < 0) throw nbt::io::input_error("NBT 列表长度无效");
        for (std::int32_t i = 0; i < length; ++i) skip_payload(reader, element_type);
        break;
    }
    case nbt::tag_type::Compound:
        for (;;) {
            const auto child_type = reader.read_type(true);
            if (child_type == nbt::tag_type::End) break;
            reader.read_string();
            skip_payload(reader, child_type);
        }
        break;
    case nbt::tag_type::Int_Array:
    case nbt::tag_type::Long_Array: {
        std::int32_t length = 0; reader.read_num(length);
        if (length < 0) throw nbt::io::input_error("NBT 数组长度无效");
        const auto element_size = type == nbt::tag_type::Int_Array ? 4u : 8u;
        skip_bytes(input, static_cast<std::uint64_t>(length) * element_size);
        break;
    }
    default: throw nbt::io::input_error("未知 NBT payload 类型");
    }
}

bool read_encoded_index(EncodedReader& input, std::uint32_t& result, std::size_t* bytes_read = nullptr)
{
    std::uint64_t value = 0;
    int shift = 0;
    std::size_t count = 0;
    for (;;) {
        std::uint8_t byte = 0;
        if (!input.get(byte)) return false;
        ++count;
        value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            if (value > std::numeric_limits<std::uint32_t>::max()) return false;
            result = static_cast<std::uint32_t>(value);
            if (bytes_read) *bytes_read = count;
            return true;
        }
        shift += 7;
        if (shift >= 35) return false;
    }
}

// Fast path for the hot world-stream loop. EncodedReader keeps a contiguous
// heap buffer, so most palette indices are decoded without a function call or
// stream state check. The fallback handles a varint split at the buffer edge.
bool read_encoded_index_fast(
    EncodedReader& input, std::uint32_t& result)
{
    return input.read_varint_u32(result);
}

bool read_encoded_index_fast(
    EncodedReader& input,
    std::uint32_t& result,
    std::uint16_t max_value)
{
    if (!read_encoded_index_fast(input, result)) return false;
    return result <= max_value;
}

bool read_encoded_index_fast(
    LimitedEncodedReader& input,
    std::uint32_t& result,
    std::uint16_t max_value)
{
    if (!input.read_varint_u32(result)) return false;
    return result <= max_value;
}

template <typename Callback>
Result<void> consume_deferred_block_data(
    const std::filesystem::path& path,
    StructureId format,
    std::size_t expected_length,
    Callback&& callback)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法重新打开 Schem 文件: " + path.string());
    try {
        // BlockData is a multi-gigabyte logical stream. A 1 MiB bounded zlib
        // buffer avoids excessive 32 KiB underflow/refill calls without
        // materializing the compressed payload.
        zlib::izlibstream decompressed(input, 1u << 20);
        NbtReader reader(decompressed, endian::big);
        if (reader.read_type() != nbt::tag_type::Compound) {
            return Result<void>::failure("Schem 根标签不是 Compound");
        }
        const auto root_name = reader.read_string();
        bool found = false;
        std::function<Result<void>(bool)> parse_document = [&](bool blocks_payload) -> Result<void> {
            for (;;) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto key = reader.read_string();
                const bool target = type == nbt::tag_type::Byte_Array &&
                    ((format == StructureId::SchemV1 && !blocks_payload && key == "BlockData") ||
                     (format == StructureId::SchemV2 && blocks_payload && key == "Data"));
                if (target) {
                    if (found) return Result<void>::failure("Schem 包含重复 BlockData");
                    std::int32_t length = 0;
                    reader.read_num(length);
                    if (length < 0 || static_cast<std::size_t>(length) != expected_length) {
                        return Result<void>::failure("Schem BlockData 长度在流式写入前后不一致");
                    }
                    LimitedEncodedReader encoded(decompressed, static_cast<std::uint32_t>(length));
                    auto consumed = callback(encoded);
                    if (!consumed) return consumed;
                    if (encoded.remaining() != 0) {
                        return Result<void>::failure("Schem BlockData 未被完整消费");
                    }
                    found = true;
                } else if (key == "Blocks" && type == nbt::tag_type::Compound && !blocks_payload) {
                    auto nested = parse_document(true);
                    if (!nested) return nested;
                } else {
                    skip_payload(reader, type);
                }
            }
            return Result<void>::success();
        };

        Result<void> parsed = Result<void>::success();
        if (root_name == "Schematic") {
            parsed = parse_document(false);
        } else if (root_name.empty()) {
            for (;;) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto key = reader.read_string();
                if (key == "Schematic" && type == nbt::tag_type::Compound) {
                    parsed = parse_document(false);
                    if (!parsed) break;
                } else {
                    skip_payload(reader, type);
                }
            }
        } else {
            return Result<void>::failure("Schem 根标签缺少 Schematic 文档");
        }
        if (!parsed) return parsed;
        if (!found) return Result<void>::failure("Schem 流式写入时未找到 BlockData");
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("Schem 直接流式解压失败: " + std::string(error.what()));
    }
}

} // namespace

SchemStructure::~SchemStructure()
{
    release_block_data();
}

void SchemStructure::release_block_data() noexcept
{
    if (!mBlockDataPath.empty()) {
        std::error_code error;
        std::filesystem::remove(mBlockDataPath, error);
    }
    mBlockDataPath.clear();
    mSourcePath.clear();
    mRowOffsets.clear();
    mChunkOffsets.clear();
    mBlockCount = 0;
    mNonAirCount = 0;
    mBlockDataBytes = 0;
    mChunkOffsetCount = 0;
    mMaxPaletteIndex = 0;
    mNonAirCountValid = false;
    mSharedPaletteStates.reset();
    mSharedAirPaletteFlags.clear();
    mSharedAirPaletteIndex = 0;
    mBlockDataDeferred = false;
}

void SchemStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> SchemStructure::read(const std::filesystem::path& path)
{
    release_block_data();
    mPalette.clear();
    mDensePalette.clear();
    mOriginalSize = {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 Schem 文件: " + path.string());
    try {
        zlib::izlibstream decompressed(input, 1u << 20);
        NbtReader reader(decompressed, endian::big);
        if (reader.read_type() != nbt::tag_type::Compound) {
            return Result<void>::failure("Schem 根标签不是 Compound");
        }
        const auto root_name = reader.read_string();
        const auto unique = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        mBlockDataPath = std::filesystem::temp_directory_path() / ("water_structure_schem_" + unique + ".blockdata");
        std::ofstream raw;

        bool has_palette = false;
        bool has_data = false;
        bool has_root_block_data = false;
        bool has_nested_block_data = false;
        bool has_size = false;
        bool has_document = false;
        bool index_built_during_read = false;
        std::uint32_t indexed_max_palette_index = 0;
        auto parse_palette = [&](std::unique_ptr<nbt::tag> payload) -> Result<void> {
            const auto& palette = payload->as<nbt::tag_compound>();
            const auto known_unknown = mRegistry.find("minecraft:unknown");
            mUnknownRuntimeId = known_unknown ? *known_unknown :
                mRegistry.register_state(BlockState{ "minecraft:unknown", {}, 0 });
            std::uint32_t max_index = 0;
            for (const auto& [java_state, value] : palette) {
                std::int64_t index = 0;
                if (value.get_type() == nbt::tag_type::Byte) index = value.as<nbt::tag_byte>().get();
                else if (value.get_type() == nbt::tag_type::Short) index = value.as<nbt::tag_short>().get();
                else if (value.get_type() == nbt::tag_type::Int) index = value.as<nbt::tag_int>().get();
                else if (value.get_type() == nbt::tag_type::Long) index = value.as<nbt::tag_long>().get();
                else return Result<void>::failure("Schem palette 索引不是整数");
                if (index < 0 || index > std::numeric_limits<std::uint32_t>::max())
                    return Result<void>::failure("Schem palette 索引不是非负整数");
                const auto palette_index = static_cast<std::uint32_t>(index);
                mPalette[palette_index] = mRegistry.compatible_java_runtime_id(java_state).value_or(mUnknownRuntimeId);
                max_index = std::max(max_index, palette_index);
            }
            mMaxPaletteIndex = max_index;
            if (max_index <= (1u << 20) - 1) {
                mDensePalette.assign(static_cast<std::size_t>(max_index) + 1, mUnknownRuntimeId);
                for (const auto& [index, runtime_id] : mPalette) mDensePalette[index] = runtime_id;
            }
            has_palette = true;
            return Result<void>::success();
        };
        auto read_data = [&]() -> Result<void> {
            std::int32_t length = 0; reader.read_num(length);
            if (length < 0) return Result<void>::failure("Schem BlockData 长度无效");
            if (mDirectWorldStream && !has_data &&
                mMaxPaletteIndex <= std::numeric_limits<std::uint16_t>::max()) {
                mBlockDataBytes = static_cast<std::size_t>(length);
                if (mOriginalSize.width > 0 && mOriginalSize.height > 0 && mOriginalSize.length > 0) {
                    mBlockCount = static_cast<std::size_t>(mOriginalSize.volume());
                }
                mSourcePath = path;
                mBlockDataDeferred = true;
                has_data = true;
                // If Palette follows BlockData, consume the byte array once
                // without materializing it so the remaining NBT metadata can
                // still be parsed. write_to_world() then reopens the gzip
                // stream and performs the only varint/materialization pass.
                const bool metadata_ready = has_palette &&
                    mOriginalSize.width > 0 && mOriginalSize.height > 0 &&
                    mOriginalSize.length > 0;
                const bool detail_profile =
                    std::getenv("WATER_STRUCTURE_PROFILE_DETAIL") != nullptr;
                const auto skip_start = detail_profile
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                if (!metadata_ready) {
                    skip_bytes(decompressed, static_cast<std::uint32_t>(length));
                }
                if (detail_profile) {
                    const auto skip_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - skip_start).count();
                    std::cerr << "schem_read_blockdata_profile bytes=" << length
                              << " metadata_ready=" << (metadata_ready ? 1 : 0)
                              << " skip_ms=" << skip_ms << '\n';
                }
                return Result<void>::success();
            }
            const auto build_index = !has_data && !mStreamingWorldImport &&
                mOriginalSize.width > 0 && mOriginalSize.height > 0 && mOriginalSize.length > 0;
            if (!raw.is_open()) {
                raw.open(mBlockDataPath, std::ios::binary | std::ios::trunc);
                if (!raw) return Result<void>::failure("无法创建 Schem 临时 BlockData 文件");
            }
            const auto width = static_cast<std::size_t>(mOriginalSize.width);
            const auto rows = static_cast<std::size_t>(mOriginalSize.height) *
                static_cast<std::size_t>(mOriginalSize.length);
            const auto expected_count = static_cast<std::size_t>(mOriginalSize.volume());
            std::size_t position = 0;
            std::size_t row = 0;
            std::size_t x = 0;
            std::uint64_t encoded_offset = 0;
            std::uint64_t value = 0;
            int shift = 0;
            // When the palette precedes BlockData (the normal Schem layout),
            // count non-air values while the index pass is already decoding
            // them.  This makes inspect() reuse the same scan instead of
            // opening and decoding the temporary stream a second time.
            const auto* indexed_dense_palette =
                mDensePalette.empty() ? nullptr : mDensePalette.data();
            const auto air_runtime_id = mRegistry.air_runtime_id();
            if (build_index) {
                mChunkOffsetCount = (width + kIndexStride - 1) / kIndexStride;
                mRowOffsets.resize(rows);
                mChunkOffsets.assign(rows * mChunkOffsetCount, kMissingChunkOffset);
            }
            std::vector<char> buffer(1u << 20);
            std::int32_t remaining = length;
            while (remaining > 0) {
                const auto amount = std::min<std::int32_t>(remaining, static_cast<std::int32_t>(buffer.size()));
                decompressed.read(buffer.data(), amount);
                if (decompressed.gcount() != amount) return Result<void>::failure("Schem BlockData 提前结束");
                raw.write(buffer.data(), amount);
                if (!raw) return Result<void>::failure("写入 Schem 临时 BlockData 失败");
                if (build_index) {
                    for (std::int32_t index = 0; index < amount; ++index) {
                        const auto byte = static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]);
                        if (shift == 0) {
                            if (position >= expected_count || row >= rows)
                                return Result<void>::failure("Schem BlockData 方块数超过 size");
                            if (x == 0) mRowOffsets[row] = encoded_offset;
                            if (x % kIndexStride == 0) {
                                const auto relative_offset = encoded_offset - mRowOffsets[row];
                                if (relative_offset < kMissingChunkOffset) {
                                    mChunkOffsets[row * mChunkOffsetCount + x / kIndexStride] =
                                        static_cast<std::uint16_t>(relative_offset);
                                }
                            }
                        }
                        value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
                        ++encoded_offset;
                        if ((byte & 0x80u) == 0) {
                            if (value > std::numeric_limits<std::uint32_t>::max())
                                return Result<void>::failure("Schem varint 超出 uint32 范围");
                            indexed_max_palette_index = std::max(
                                indexed_max_palette_index, static_cast<std::uint32_t>(value));
                            if (indexed_dense_palette && value < mDensePalette.size() &&
                                indexed_dense_palette[value] != air_runtime_id) {
                                ++mNonAirCount;
                            }
                            value = 0;
                            shift = 0;
                            ++position;
                            ++x;
                            if (x == width) {
                                ++row;
                                x = 0;
                            }
                        } else {
                            shift += 7;
                            if (shift >= 35) return Result<void>::failure("Schem varint 过长");
                        }
                    }
                }
                remaining -= amount;
                mBlockDataBytes += static_cast<std::size_t>(amount);
            }
            if (build_index) {
                if (shift != 0) return Result<void>::failure("Schem BlockData 以截断 varint 结束");
                if (position != expected_count || row != rows || x != 0)
                    return Result<void>::failure("Schem BlockData 方块数与 size 不一致");
                index_built_during_read = true;
                mNonAirCountValid = indexed_dense_palette != nullptr;
            }
            has_data = true;
            return Result<void>::success();
        };
        std::function<Result<void>(bool)> parse_document = [&](bool blocks_payload) -> Result<void> {
            for (;;) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto key = reader.read_string();
                if (key == "Width" || key == "Height" || key == "Length") {
                    if (!is_integer_type(type)) { skip_payload(reader, type); continue; }
                    const auto value = read_integer(reader, type).value_or(0);
                    if (key == "Width") mOriginalSize.width = value;
                    else if (key == "Height") mOriginalSize.height = value;
                    else mOriginalSize.length = value;
                    has_size = true;
                } else if (key == "Palette" && type == nbt::tag_type::Compound) {
                    auto parsed = parse_palette(reader.read_payload(type)); if (!parsed) return parsed;
                    if (mBlockDataDeferred && mOriginalSize.width > 0 &&
                        mOriginalSize.height > 0 && mOriginalSize.length > 0) {
                        return Result<void>::success();
                    }
                } else if (key == "Blocks" && type == nbt::tag_type::Compound && !blocks_payload &&
                           mFormat == StructureId::SchemV1) {
                    // Reject the other Schem version before consuming its
                    // potentially enormous nested BlockData payload.  The
                    // registry probes V1 before V2, so this keeps a V2 file
                    // from being fully decompressed twice.
                    return Result<void>::failure("SchemV1 使用了 Blocks/Data 结构");
                } else if ((key == "BlockData" && type == nbt::tag_type::Byte_Array && !blocks_payload) ||
                           (key == "Data" && type == nbt::tag_type::Byte_Array && blocks_payload)) {
                    auto parsed = read_data(); if (!parsed) return parsed;
                    if (blocks_payload) has_nested_block_data = true;
                    else has_root_block_data = true;
                    if (mBlockDataDeferred && has_palette &&
                        mOriginalSize.width > 0 && mOriginalSize.height > 0 &&
                        mOriginalSize.length > 0) return Result<void>::success();
                } else if (key == "Blocks" && type == nbt::tag_type::Compound && !blocks_payload) {
                    auto parsed = parse_document(true); if (!parsed) return parsed;
                    if (mBlockDataDeferred && has_palette &&
                        mOriginalSize.width > 0 && mOriginalSize.height > 0 &&
                        mOriginalSize.length > 0) return Result<void>::success();
                } else {
                    skip_payload(reader, type);
                }
            }
            return Result<void>::success();
        };
        Result<void> parsed = Result<void>::success();
        if (root_name == "Schematic") {
            has_document = true;
            parsed = parse_document(false);
        } else if (root_name.empty()) {
            for (;;) {
                const auto type = reader.read_type(true);
                if (type == nbt::tag_type::End) break;
                const auto key = reader.read_string();
                if (key == "Schematic" && type == nbt::tag_type::Compound) {
                    has_document = true;
                    parsed = parse_document(false);
                }
                else skip_payload(reader, type);
                if (!parsed || mBlockDataDeferred) break;
            }
        }
        if (!parsed) return parsed;
        if (!has_document) return Result<void>::failure("Schem 根标签缺少 Schematic 文档");
        if (mOriginalSize.width <= 0 || mOriginalSize.height <= 0 || mOriginalSize.length <= 0 || !has_size)
            return Result<void>::failure("Schem 尺寸无效");
        if (!has_palette || !has_data) return Result<void>::failure("Schem 缺少 Palette/BlockData");
        if (index_built_during_read && indexed_max_palette_index > mMaxPaletteIndex)
            return Result<void>::failure("Schem BlockData 引用了 palette 范围外的索引");
        if (mFormat == StructureId::SchemV1 && !has_root_block_data)
            return Result<void>::failure("SchemV1 缺少根层 BlockData");
        if (mFormat == StructureId::SchemV2 && !has_nested_block_data)
            return Result<void>::failure("SchemV2 缺少 Blocks/Data");
        raw.close();
        mBlockCount = static_cast<std::size_t>(mOriginalSize.volume());
        if (mBlockDataDeferred) {
            std::error_code cleanup_error;
            std::filesystem::remove(mBlockDataPath, cleanup_error);
            mBlockDataPath.clear();
            set_offset({});
            return Result<void>::success();
        }
        if (!index_built_during_read &&
            (!mStreamingWorldImport || mMaxPaletteIndex > std::numeric_limits<std::uint16_t>::max())) {
            EncodedReader encoded(mBlockDataPath);
            if (!encoded.good()) return Result<void>::failure("无法打开 Schem 临时 BlockData 文件");
            const auto width = static_cast<std::size_t>(mOriginalSize.width);
            const auto rows = static_cast<std::size_t>(mOriginalSize.height) *
                static_cast<std::size_t>(mOriginalSize.length);
            mRowOffsets.resize(rows);
            mChunkOffsetCount = (width + kIndexStride - 1) / kIndexStride;
            mChunkOffsets.assign(rows * mChunkOffsetCount, kMissingChunkOffset);
            std::uint64_t encoded_offset = 0;
            for (std::size_t index = 0; index < mBlockCount; ++index) {
                const auto row = index / width;
                const auto x = index % width;
                if (x == 0) mRowOffsets[row] = encoded_offset;
                if (x % kIndexStride == 0) {
                    const auto relative_offset = encoded_offset - mRowOffsets[row];
                    if (relative_offset < kMissingChunkOffset) {
                        mChunkOffsets[row * mChunkOffsetCount + x / kIndexStride] =
                            static_cast<std::uint16_t>(relative_offset);
                    }
                }
                std::uint32_t palette_index = 0;
                std::size_t encoded_size = 0;
                if (!read_encoded_index(encoded, palette_index, &encoded_size) || palette_index > mMaxPaletteIndex) {
                    return Result<void>::failure("Schem BlockData varint 读取失败或 palette 索引越界");
                }
                encoded_offset += encoded_size;
            }
            std::uint8_t trailing_byte = 0;
            if (encoded.get(trailing_byte)) {
                return Result<void>::failure("Schem BlockData 方块数超过 size");
            }
        }
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 ") + std::string(name()) + " 失败: " + error.what());
    }
}

Result<ChunkMap> SchemStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    EncodedReader encoded(mBlockDataPath);
    if (!encoded.good()) return Result<ChunkMap>::failure("无法打开 Schem 临时 BlockData 文件");
    const auto width = mOriginalSize.width, height = mOriginalSize.height, length = mOriginalSize.length;
    const auto air_runtime_id = mRegistry.air_runtime_id();
    const auto* dense_palette = mDensePalette.empty() ? nullptr : mDensePalette.data();
    const auto runtime_id_for = [this, dense_palette](std::uint32_t index) noexcept {
        if (dense_palette && index < mDensePalette.size()) return dense_palette[index];
        const auto found = mPalette.find(index); return found == mPalette.end() ? mUnknownRuntimeId : found->second;
    };
    std::vector<std::uint8_t> encoded_row;
    const auto read_row_range = [this, &encoded, &encoded_row](
        std::size_t row_index,
        int begin_x,
        int end_x,
        std::span<std::uint32_t> output) -> bool {
        if (row_index >= mRowOffsets.size() || begin_x < 0 || end_x < begin_x ||
            output.size() != static_cast<std::size_t>(end_x - begin_x + 1)) return false;
        const auto checkpoint_index = static_cast<std::size_t>(begin_x) / kIndexStride;
        if (checkpoint_index >= mChunkOffsetCount) return false;
        const auto checkpoint = mChunkOffsets[row_index * mChunkOffsetCount + checkpoint_index];
        int source_x = static_cast<int>(checkpoint_index * kIndexStride);
        std::uint64_t source_offset = mRowOffsets[row_index];
        if (checkpoint == kMissingChunkOffset) {
            source_x = 0;
        } else {
            source_offset += checkpoint;
        }
        if (!encoded.seek(source_offset)) return false;
        const auto row_end = row_index + 1 < mRowOffsets.size()
            ? mRowOffsets[row_index + 1]
            : static_cast<std::uint64_t>(mBlockDataBytes);
        if (source_offset > row_end || row_end - source_offset > std::numeric_limits<std::size_t>::max())
            return false;
        encoded_row.resize(static_cast<std::size_t>(row_end - source_offset));
        if (!encoded.read_exact(encoded_row)) return false;
        std::size_t cursor = 0;
        for (; source_x <= end_x; ++source_x) {
            std::uint64_t value = 0;
            int shift = 0;
            for (;;) {
                if (cursor >= encoded_row.size()) return false;
                const auto byte = encoded_row[cursor++];
                value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
                if ((byte & 0x80u) == 0) break;
                shift += 7;
                if (shift >= 35) return false;
            }
            if (value > mMaxPaletteIndex) return false;
            if (source_x >= begin_x) {
                output[static_cast<std::size_t>(source_x - begin_x)] = static_cast<std::uint32_t>(value);
            }
        }
        return true;
    };
    // Conversion batches are ordered by Z, so a batch normally covers one Z
    // chunk and many X chunks. Decode each source row once and fan it out.
    if (!result.empty()) {
        auto first = result.begin();
        const auto first_chunk_min_z = static_cast<std::int64_t>(first->first.z) * 16;
        const auto first_min_z = std::max<std::int64_t>(0, first_chunk_min_z - mOffset.z);
        const auto first_max_z = std::min<std::int64_t>(length - 1, first_chunk_min_z + 15 - mOffset.z);
        bool same_z_range = first_min_z <= first_max_z;
        std::vector<std::pair<int, ChunkData*>> target_entries;
        int min_target_x = std::numeric_limits<int>::max();
        int max_target_x = std::numeric_limits<int>::min();
        int min_x = width, max_x = -1, min_z = static_cast<int>(first_min_z), max_z = static_cast<int>(first_max_z);
        for (auto& [pos, chunk] : result) {
            const auto chunk_min_z = static_cast<std::int64_t>(pos.z) * 16;
            const auto row_min_z = std::max<std::int64_t>(0, chunk_min_z - mOffset.z);
            const auto row_max_z = std::min<std::int64_t>(length - 1, chunk_min_z + 15 - mOffset.z);
            if (row_min_z != first_min_z || row_max_z != first_max_z) { same_z_range = false; break; }
            const auto chunk_min_x = static_cast<std::int64_t>(pos.x) * 16;
            const auto row_min_x = std::max<std::int64_t>(0, chunk_min_x - mOffset.x);
            const auto row_max_x = std::min<std::int64_t>(width - 1, chunk_min_x + 15 - mOffset.x);
            if (row_min_x <= row_max_x) {
                min_x = std::min(min_x, static_cast<int>(row_min_x));
                max_x = std::max(max_x, static_cast<int>(row_max_x));
                const auto target_x = floor_div(static_cast<std::int32_t>(chunk_min_x - mOffset.x), 16);
                target_entries.emplace_back(target_x, &chunk);
                min_target_x = std::min(min_target_x, target_x);
                max_target_x = std::max(max_target_x, target_x);
            }
        }
        if (same_z_range && min_x <= max_x) {
            std::vector<ChunkData*> targets(static_cast<std::size_t>(max_target_x - min_target_x + 1), nullptr);
            for (const auto& [target_x, chunk] : target_entries) targets[static_cast<std::size_t>(target_x - min_target_x)] = chunk;
            const int begin_x = (min_x / static_cast<int>(kIndexStride)) * static_cast<int>(kIndexStride);
            std::vector<std::uint32_t> row_data(static_cast<std::size_t>(max_x - begin_x + 1));
            for (int y = 0; y < height; ++y) {
                const int structure_y = y + mOffset.y, sub_y = floor_div(structure_y - 64, 16);
                const int local_y = structure_y - (sub_y * 16 + 64);
                std::vector<SubChunkData*> sub_chunks(targets.size(), nullptr);
                for (int z = min_z; z <= max_z; ++z) {
                    const auto row_index = static_cast<std::size_t>(y) * static_cast<std::size_t>(length) + z;
                    if (!read_row_range(row_index, begin_x, max_x, row_data))
                        return Result<ChunkMap>::failure("Schem BlockData 行索引读取失败");
                    for (int x = begin_x; x <= max_x; ++x) {
                        const auto palette_index = row_data[static_cast<std::size_t>(x - begin_x)];
                        if (x < min_x) continue;
                        const auto target_x = floor_div(static_cast<std::int32_t>(static_cast<std::int64_t>(x) + mOffset.x), 16);
                        const auto target_index = target_x - min_target_x;
                        if (target_index < 0 || static_cast<std::size_t>(target_index) >= targets.size()) continue;
                        auto* target = targets[static_cast<std::size_t>(target_index)];
                        if (!target) continue;
                        const auto runtime_id = runtime_id_for(palette_index);
                        if (runtime_id == air_runtime_id) continue;
                        auto& sub_chunk = sub_chunks[static_cast<std::size_t>(target_index)];
                        if (!sub_chunk) {
                            auto [sub_it, inserted] = target->sub_chunks.try_emplace(sub_y);
                            if (inserted) { sub_it->second.layer0.fill(air_runtime_id); sub_it->second.layer1.fill(air_runtime_id); }
                            sub_chunk = &sub_it->second;
                        }
                        const auto chunk_min_x = static_cast<std::int64_t>(target_x) * 16;
                        const auto local_x = static_cast<int>(static_cast<std::int64_t>(x) + mOffset.x - chunk_min_x);
                        const auto local_z = static_cast<int>(static_cast<std::int64_t>(z) + mOffset.z - first_chunk_min_z);
                        sub_chunk->layer0[static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x)] = runtime_id;
                    }
                }
            }
            return Result<ChunkMap>::success(std::move(result));
        }
    }
    for (auto& [chunk_pos, chunk] : result) {
        const auto chunk_min_x = static_cast<std::int64_t>(chunk_pos.x) * 16;
        const auto chunk_min_z = static_cast<std::int64_t>(chunk_pos.z) * 16;
        const auto source_min_x = std::max<std::int64_t>(0, chunk_min_x - mOffset.x);
        const auto source_max_x = std::min<std::int64_t>(width - 1, chunk_min_x + 15 - mOffset.x);
        const auto source_min_z = std::max<std::int64_t>(0, chunk_min_z - mOffset.z);
        const auto source_max_z = std::min<std::int64_t>(length - 1, chunk_min_z + 15 - mOffset.z);
        if (source_min_x > source_max_x || source_min_z > source_max_z) continue;
        const auto min_x = static_cast<int>(source_min_x), max_x = static_cast<int>(source_max_x);
        const int begin_x = (min_x / static_cast<int>(kIndexStride)) * static_cast<int>(kIndexStride);
        std::vector<std::uint32_t> row_data(static_cast<std::size_t>(max_x - begin_x + 1));
        for (int y = 0; y < height; ++y) {
            const int structure_y = y + mOffset.y, sub_y = floor_div(structure_y - 64, 16);
            const int local_y = structure_y - (sub_y * 16 + 64);
            SubChunkData* sub_chunk = nullptr;
            for (int z = static_cast<int>(source_min_z); z <= static_cast<int>(source_max_z); ++z) {
                const auto row_index = static_cast<std::size_t>(y) * static_cast<std::size_t>(length) + z;
                if (!read_row_range(row_index, begin_x, max_x, row_data))
                    return Result<ChunkMap>::failure("Schem BlockData 行索引读取失败");
                for (int x = begin_x; x <= max_x; ++x) {
                    const auto palette_index = row_data[static_cast<std::size_t>(x - begin_x)];
                    if (x < min_x) continue;
                    const auto runtime_id = runtime_id_for(palette_index);
                    if (runtime_id == air_runtime_id) continue;
                    if (!sub_chunk) {
                        auto [sub_it, inserted] = chunk.sub_chunks.try_emplace(sub_y);
                        if (inserted) { sub_it->second.layer0.fill(air_runtime_id); sub_it->second.layer1.fill(air_runtime_id); }
                        sub_chunk = &sub_it->second;
                    }
                    const auto local_z = static_cast<int>(static_cast<std::int64_t>(z) + mOffset.z - chunk_min_z);
                    const auto local_x = static_cast<int>(static_cast<std::int64_t>(x) + mOffset.x - chunk_min_x);
                    sub_chunk->layer0[static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x)] = runtime_id;
                }
            }
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

std::size_t SchemStructure::preferred_palette_batch_size() const noexcept
{
    constexpr std::size_t kBudget = 96ull * 1024ull * 1024ull;
    const auto subchunks = static_cast<std::size_t>(
        std::max(1, (mSize.height + 15) / 16));
    const auto bytes_per_chunk = subchunks * 4096ull * sizeof(std::uint16_t) +
        64ull * 1024ull;
    return std::max<std::size_t>(1, std::min<std::size_t>(
        static_cast<std::size_t>(std::max(1, mSize.chunk_x_count())),
        kBudget / std::max<std::size_t>(1, bytes_per_chunk)));
}

Result<void> SchemStructure::visit_chunk_palettes(
    std::span<const ChunkPos> positions,
    const ChunkPaletteVisitor& visitor) const
{
    if (!visitor) return Result<void>::failure("chunk palette visitor is empty");
    if (positions.empty()) return Result<void>::success();
    if (mOffset != BlockPos{}) {
        return Result<void>::failure("Schem palette 快速路径暂不支持 offset");
    }
    if (mMaxPaletteIndex > std::numeric_limits<std::uint16_t>::max()) {
        return Result<void>::failure("Schem palette 超过 uint16 容量");
    }
    const auto chunk_z = positions.front().z;
    int min_chunk_x = std::numeric_limits<int>::max();
    int max_chunk_x = std::numeric_limits<int>::min();
    std::unordered_map<int, std::size_t> target_by_x;
    target_by_x.reserve(positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        const auto position = positions[index];
        if (position.z != chunk_z) {
            return Result<void>::failure("Schem palette 批次必须位于同一 chunk Z");
        }
        if (position.x < 0 || position.x >= mSize.chunk_x_count() ||
            position.z < 0 || position.z >= mSize.chunk_z_count()) {
            return Result<void>::failure("Schem palette chunk 坐标越界");
        }
        target_by_x.emplace(position.x, index);
        min_chunk_x = std::min(min_chunk_x, position.x);
        max_chunk_x = std::max(max_chunk_x, position.x);
    }

    if (!mSharedPaletteStates) {
        auto states = std::make_shared<std::vector<BlockState>>(
            static_cast<std::size_t>(mMaxPaletteIndex) + 1,
            BlockState{ "minecraft:unknown", {}, 0 });
        mSharedAirPaletteFlags.assign(states->size(), 0);
        bool found_air = false;
        const auto air_runtime = mRegistry.air_runtime_id();
        for (std::uint32_t index = 0; index <= mMaxPaletteIndex; ++index) {
            const auto runtime = index < mDensePalette.size()
                ? mDensePalette[index]
                : (mPalette.contains(index) ? mPalette.at(index) : mUnknownRuntimeId);
            if (auto state = mRegistry.state(runtime)) (*states)[index] = std::move(*state);
            if (runtime == air_runtime) {
                mSharedAirPaletteFlags[index] = 1;
                if (!found_air) {
                    mSharedAirPaletteIndex = static_cast<std::uint16_t>(index);
                    found_air = true;
                }
            }
        }
        if (!found_air) {
            if (states->size() > std::numeric_limits<std::uint16_t>::max()) {
                return Result<void>::failure("Schem palette 没有 air 且无法追加 uint16 索引");
            }
            mSharedAirPaletteIndex = static_cast<std::uint16_t>(states->size());
            states->push_back(BlockState{ "minecraft:air", {}, 0 });
            mSharedAirPaletteFlags.push_back(1);
        }
        mSharedPaletteStates = std::move(states);
    }

    struct PaletteChunk {
        std::vector<std::unique_ptr<SubChunkPaletteData>> subchunks;
    };
    const auto min_sub_y = floor_div(-64, 16);
    const auto max_sub_y = floor_div(mSize.height - 1 - 64, 16);
    const auto subchunk_count = static_cast<std::size_t>(max_sub_y - min_sub_y + 1);
    std::vector<PaletteChunk> chunks(positions.size());
    for (auto& chunk : chunks) {
        chunk.subchunks.resize(subchunk_count);
    }

    EncodedReader encoded(mBlockDataPath);
    if (!encoded.good()) {
        return Result<void>::failure("无法打开 Schem 临时 BlockData 文件");
    }
    std::vector<std::uint8_t> encoded_row;
    const auto read_row_range = [this, &encoded, &encoded_row](
        std::size_t row_index,
        int begin_x,
        int end_x,
        std::span<std::uint32_t> output) -> bool {
        if (row_index >= mRowOffsets.size() || begin_x < 0 || end_x < begin_x ||
            output.size() != static_cast<std::size_t>(end_x - begin_x + 1)) return false;
        const auto checkpoint_index = static_cast<std::size_t>(begin_x) / kIndexStride;
        if (checkpoint_index >= mChunkOffsetCount) return false;
        const auto checkpoint = mChunkOffsets[row_index * mChunkOffsetCount + checkpoint_index];
        int source_x = static_cast<int>(checkpoint_index * kIndexStride);
        std::uint64_t source_offset = mRowOffsets[row_index];
        if (checkpoint == kMissingChunkOffset) source_x = 0;
        else source_offset += checkpoint;
        if (!encoded.seek(source_offset)) return false;
        const auto row_end = row_index + 1 < mRowOffsets.size()
            ? mRowOffsets[row_index + 1]
            : static_cast<std::uint64_t>(mBlockDataBytes);
        if (source_offset > row_end ||
            row_end - source_offset > std::numeric_limits<std::size_t>::max()) return false;
        encoded_row.resize(static_cast<std::size_t>(row_end - source_offset));
        if (!encoded.read_exact(encoded_row)) return false;
        std::size_t cursor = 0;
        for (; source_x <= end_x; ++source_x) {
            std::uint64_t value = 0;
            int shift = 0;
            for (;;) {
                if (cursor >= encoded_row.size()) return false;
                const auto byte = encoded_row[cursor++];
                value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
                if ((byte & 0x80u) == 0) break;
                shift += 7;
                if (shift >= 35) return false;
            }
            if (value > mMaxPaletteIndex) return false;
            if (source_x >= begin_x)
                output[static_cast<std::size_t>(source_x - begin_x)] =
                    static_cast<std::uint32_t>(value);
        }
        return true;
    };

    const auto min_x = min_chunk_x * 16;
    const auto max_x = std::min(mSize.width - 1, max_chunk_x * 16 + 15);
    const auto min_z = chunk_z * 16;
    const auto max_z = std::min(mSize.length - 1, min_z + 15);
    std::vector<std::uint32_t> row_data(
        static_cast<std::size_t>(max_x - min_x + 1));
    constexpr auto kMissingTarget = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> target_for_x(row_data.size(), kMissingTarget);
    std::vector<std::uint8_t> local_x_for_x(row_data.size(), 0);
    for (std::size_t offset = 0; offset < row_data.size(); ++offset) {
        const auto x = min_x + static_cast<int>(offset);
        const auto target = target_by_x.find(floor_div(x, 16));
        if (target != target_by_x.end()) target_for_x[offset] = target->second;
        local_x_for_x[offset] = static_cast<std::uint8_t>(floor_mod(x, 16));
    }
    for (int y = 0; y < mSize.height; ++y) {
        const auto sub_y = floor_div(y - 64, 16);
        const auto sub_index = static_cast<std::size_t>(sub_y - min_sub_y);
        const auto local_y = y - (sub_y * 16 + 64);
        for (int z = min_z; z <= max_z; ++z) {
            const auto row_index = static_cast<std::size_t>(y) *
                static_cast<std::size_t>(mSize.length) + static_cast<std::size_t>(z);
            if (!read_row_range(row_index, min_x, max_x, row_data)) {
                return Result<void>::failure("Schem palette BlockData 行读取失败");
            }
            const auto local_z = z - min_z;
            for (std::size_t x_offset = 0; x_offset < row_data.size(); ++x_offset) {
                const auto target = target_for_x[x_offset];
                if (target == kMissingTarget) continue;
                const auto palette_index = row_data[x_offset];
                if (mSharedAirPaletteFlags[palette_index]) continue;
                auto& subchunk = chunks[target].subchunks[sub_index];
                if (!subchunk) {
                    subchunk = std::make_unique<SubChunkPaletteData>();
                    subchunk->sub_y = sub_y;
                    subchunk->shared_palette = mSharedPaletteStates;
                    subchunk->indices.assign(4096, mSharedAirPaletteIndex);
                }
                const auto native_index = static_cast<std::size_t>(
                    local_x_for_x[x_offset] * 256 + local_y * 16 + local_z);
                subchunk->indices[native_index] =
                    static_cast<std::uint16_t>(palette_index);
            }
        }
    }

    for (std::size_t index = 0; index < positions.size(); ++index) {
        std::vector<SubChunkPaletteData> populated;
        populated.reserve(chunks[index].subchunks.size());
        for (auto& subchunk : chunks[index].subchunks) {
            if (subchunk) populated.push_back(std::move(*subchunk));
        }
        auto visited = visitor(positions[index], populated);
        if (!visited) return visited;
    }
    return Result<void>::success();
}

Result<void> SchemStructure::visit_chunks(std::span<const ChunkPos> positions, const ChunkVisitor& visitor) const
{
    if (!visitor) return Result<void>::failure("chunk visitor is empty");
    auto chunks = get_chunks(positions);
    if (!chunks) return Result<void>::failure(chunks.error());
    for (const auto position : positions) {
        const auto found = chunks.value().find(position);
        if (found == chunks.value().end()) continue;
        auto visited = visitor(position, found->second);
        if (!visited) return visited;
    }
    return Result<void>::success();
}

Result<NbtChunkMap> SchemStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<std::size_t> SchemStructure::count_non_air_blocks() const
{
    if (mNonAirCountValid) return Result<std::size_t>::success(mNonAirCount);
    if (mBlockDataDeferred) {
        std::size_t result = 0;
        const auto air_runtime_id = mRegistry.air_runtime_id();
        const auto* dense_palette = mDensePalette.empty() ? nullptr : mDensePalette.data();
        const auto consumed = consume_deferred_block_data(
            mSourcePath, mFormat, mBlockDataBytes,
            [&](auto& encoded) -> Result<void> {
                for (std::size_t i = 0; i < mBlockCount; ++i) {
                    std::uint32_t index = 0;
                    if (!encoded.read_varint_u32(index) || index > mMaxPaletteIndex) {
                        return Result<void>::failure("Schem BlockData varint 读取失败或 palette 索引越界");
                    }
                    const auto runtime_id = dense_palette && index < mDensePalette.size()
                        ? dense_palette[index]
                        : (mPalette.contains(index) ? mPalette.at(index) : mUnknownRuntimeId);
                    if (runtime_id != air_runtime_id) ++result;
                }
                std::uint8_t trailing = 0;
                if (encoded.get(trailing)) {
                    return Result<void>::failure("Schem BlockData 方块数超过 size");
                }
                return Result<void>::success();
            });
        if (!consumed) return Result<std::size_t>::failure(consumed.error());
        return Result<std::size_t>::success(result);
    }
    EncodedReader encoded(mBlockDataPath);
    if (!encoded.good()) return Result<std::size_t>::failure("无法打开 Schem 临时 BlockData 文件");
    const auto* dense_palette = mDensePalette.empty() ? nullptr : mDensePalette.data();
    const auto air_runtime_id = mRegistry.air_runtime_id();
    std::size_t result = 0;
    for (std::size_t i = 0; i < mBlockCount; ++i) {
        std::uint32_t index = 0;
        if (!read_encoded_index(encoded, index) || index > mMaxPaletteIndex)
            return Result<std::size_t>::failure("Schem BlockData varint 读取失败或 palette 索引越界");
        const auto runtime_id = dense_palette && index < mDensePalette.size() ? dense_palette[index] :
            (mPalette.count(index) ? mPalette.at(index) : mUnknownRuntimeId);
        if (runtime_id != air_runtime_id) ++result;
    }
    return Result<std::size_t>::success(result);
}

Result<void> SchemStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    if (mBlockDataDeferred && mMaxPaletteIndex > std::numeric_limits<std::uint16_t>::max()) {
        const auto unique = std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        const auto fallback_path = std::filesystem::temp_directory_path() /
            ("water_structure_schem_" + unique + ".blockdata");
        std::ofstream raw(fallback_path, std::ios::binary | std::ios::trunc);
        if (!raw) return Result<void>::failure("无法创建 Schem BlockData fallback 文件");
        const auto width = static_cast<std::size_t>(mOriginalSize.width);
        const auto rows = static_cast<std::size_t>(mOriginalSize.height) *
            static_cast<std::size_t>(mOriginalSize.length);
        mChunkOffsetCount = (width + kIndexStride - 1) / kIndexStride;
        mRowOffsets.assign(rows, 0);
        mChunkOffsets.assign(rows * mChunkOffsetCount, kMissingChunkOffset);
        std::uint64_t encoded_offset = 0;
        const auto materialized = consume_deferred_block_data(
            mSourcePath, mFormat, mBlockDataBytes,
            [&](auto& encoded) -> Result<void> {
                for (std::size_t index = 0; index < mBlockCount; ++index) {
                    std::uint32_t value = 0;
                    if (!encoded.read_varint_u32(value) || value > mMaxPaletteIndex) {
                        return Result<void>::failure("Schem BlockData varint 读取失败或 palette 索引越界");
                    }
                    const auto row = index / width;
                    const auto x = index % width;
                    if (x == 0) mRowOffsets[row] = encoded_offset;
                    if (x % kIndexStride == 0) {
                        const auto relative_offset = encoded_offset - mRowOffsets[row];
                        if (relative_offset < kMissingChunkOffset) {
                            mChunkOffsets[row * mChunkOffsetCount + x / kIndexStride] =
                                static_cast<std::uint16_t>(relative_offset);
                        }
                    }
                    std::uint64_t encoded_size = 0;
                    do {
                        const auto byte = static_cast<char>(
                            (value & 0x7fu) | (value >= 0x80u ? 0x80u : 0));
                        raw.put(byte);
                        ++encoded_size;
                        value >>= 7u;
                    } while (value != 0);
                    encoded_offset += encoded_size;
                }
                std::uint8_t trailing = 0;
                if (encoded.get(trailing)) {
                    return Result<void>::failure("Schem BlockData 方块数超过 size");
                }
                return raw ? Result<void>::success() :
                    Result<void>::failure("写入 Schem BlockData fallback 失败");
            });
        raw.close();
        if (!materialized) {
            std::error_code cleanup_error;
            std::filesystem::remove(fallback_path, cleanup_error);
            return materialized;
        }
        mBlockDataBytes = static_cast<std::size_t>(encoded_offset);
        mBlockDataPath = fallback_path;
        mBlockDataDeferred = false;
    }
    if (mOffset != BlockPos{} || mMaxPaletteIndex > std::numeric_limits<std::uint16_t>::max()) {
        return convert_to_world(*this, world, start, std::move(callbacks));
    }

    auto write_from_reader = [this, &world, start, callbacks](auto& encoded) mutable -> Result<void> {
    const auto width = static_cast<std::size_t>(mOriginalSize.width);
    const auto height = static_cast<std::size_t>(mOriginalSize.height);
    const auto length = static_cast<std::size_t>(mOriginalSize.length);
    const auto chunk_x_count = static_cast<std::size_t>(mOriginalSize.chunk_x_count());
    const auto chunk_z_count = static_cast<std::size_t>(mOriginalSize.chunk_z_count());
    const auto total_chunks = chunk_x_count * chunk_z_count;
    const auto air_runtime_id = mRegistry.air_runtime_id();
    const auto* dense_palette = mDensePalette.empty() ? nullptr : mDensePalette.data();
    const auto runtime_id_for = [this, dense_palette](std::uint16_t index) noexcept {
        if (dense_palette && index < mDensePalette.size()) return mDensePalette[index];
        const auto found = mPalette.find(index);
        return found == mPalette.end() ? mUnknownRuntimeId : found->second;
    };
    // The direct world stream is palette-index bounded to uint16. Resolve it
    // once so the 500M-block hot loop performs a plain array lookup instead of
    // repeatedly branching between dense and sparse palette representations.
    std::vector<std::uint32_t> runtime_palette(
        static_cast<std::size_t>(mMaxPaletteIndex) + 1, mUnknownRuntimeId);
    for (std::size_t index = 0; index < runtime_palette.size(); ++index) {
        runtime_palette[index] = runtime_id_for(static_cast<std::uint16_t>(index));
    }
    std::vector<std::uint8_t> palette_non_air(
        runtime_palette.size(), 1);
    for (std::size_t index = 0; index < palette_non_air.size(); ++index) {
        palette_non_air[index] = runtime_palette[index] != air_runtime_id;
    }
    if (callbacks.start) callbacks.start(total_chunks);

    const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr;
    const bool verify = std::getenv("WATER_STRUCTURE_VERIFY") != nullptr;
    double decode_ms = 0.0;
    double non_air_scan_ms = 0.0;
    double materialize_ms = 0.0;
    double save_ms = 0.0;
    std::uint64_t verification_checksum = 0;
    if (verify) {
        for (std::size_t chunk_z = 0; chunk_z < chunk_z_count; ++chunk_z) {
            for (std::size_t chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) {
                verification_checksum += static_cast<std::uint32_t>(chunk_x) +
                    static_cast<std::uint32_t>(chunk_z);
            }
        }
    }
    const auto elapsed_ms = [](const auto begin) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
    };

    const auto plane_size = width * length;
    std::vector<std::uint16_t> slab(plane_size * 16);
    std::vector<std::uint8_t> slab_non_air(chunk_x_count * chunk_z_count);
    const auto materialize_worker_count =
        schem_materialize_worker_count(chunk_x_count);
    detail::BoundedThreadPool materialize_pool(
        materialize_worker_count,
        materialize_worker_count * 2);
    for (std::size_t slab_y = 0; slab_y < height; slab_y += 16) {
        const auto layer_count = std::min<std::size_t>(16, height - slab_y);
        const auto decode_start = std::chrono::steady_clock::now();
        std::fill(slab_non_air.begin(), slab_non_air.end(), 0);
        const auto slab_value_count = plane_size * layer_count;
        if (!encoded.read_varints_u16(
            std::span<std::uint16_t>(slab.data(), slab_value_count),
            static_cast<std::uint16_t>(mMaxPaletteIndex))) {
            return Result<void>::failure(
                "Schem BlockData varint 读取失败或 palette 索引越界");
        }
        const auto non_air_scan_start = std::chrono::steady_clock::now();
        // Check one chunk column at a time and stop as soon as a non-air
        // value is found. Large builds are usually dense, so this avoids
        // rescanning the remaining layers after the first populated block;
        // sparse/air-only chunks retain the same exact result.
        for (std::size_t chunk_z = 0; chunk_z < chunk_z_count; ++chunk_z) {
            const auto source_z_begin = chunk_z * 16;
            const auto source_z_end = std::min(length, source_z_begin + 16);
            for (std::size_t chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) {
                auto& populated = slab_non_air[chunk_z * chunk_x_count + chunk_x];
                if (populated) continue;
                const auto source_x_begin = chunk_x * 16;
                const auto source_x_end = std::min(width, source_x_begin + 16);
                bool found_non_air = false;
                for (std::size_t local_y = 0;
                     local_y < layer_count && !found_non_air; ++local_y) {
                    for (std::size_t source_z = source_z_begin;
                         source_z < source_z_end && !found_non_air; ++source_z) {
                        const auto row_offset =
                            (local_y * length + source_z) * width;
                        for (std::size_t source_x = source_x_begin;
                             source_x < source_x_end; ++source_x) {
                            if (palette_non_air[slab[row_offset + source_x]]) {
                                found_non_air = true;
                                break;
                            }
                        }
                    }
                }
                populated = static_cast<std::uint8_t>(found_non_air);
            }
        }
        non_air_scan_ms += elapsed_ms(non_air_scan_start);
        decode_ms += elapsed_ms(decode_start);

        const auto target_sub_y = start.y + static_cast<std::int32_t>(slab_y / 16);
        if (target_sub_y < kOverworldMinY / 16 || target_sub_y > 19) {
            return Result<void>::failure(
                "目标 subchunk 超出 Overworld 高度范围: subY=" + std::to_string(target_sub_y));
        }
        for (std::size_t chunk_z = 0; chunk_z < chunk_z_count; ++chunk_z) {
            const auto source_z_begin = chunk_z * 16;
            const auto source_z_end = std::min(length, source_z_begin + 16);
            const auto source_z_count = source_z_end - source_z_begin;
            const auto materialize_start = std::chrono::steady_clock::now();
            std::vector<ChunkData> chunks(chunk_x_count);
            std::vector<std::uint64_t> materialized_checksums(chunk_x_count, 0);
            std::vector<std::future<Result<void>>> materialize_tasks;
            materialize_tasks.reserve(chunk_x_count);
            for (std::size_t chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) {
                if (!slab_non_air[chunk_z * chunk_x_count + chunk_x]) continue;
                materialize_tasks.push_back(materialize_pool.submit(
                    [&, chunk_x]() -> Result<void> {
                        const auto source_x_begin = chunk_x * 16;
                        const auto source_x_end = std::min(width, source_x_begin + 16);
                        SubChunkData sub_chunk;
                        sub_chunk.layer0.fill(air_runtime_id);
                        sub_chunk.layer1.fill(air_runtime_id);
                        for (std::size_t local_y = 0; local_y < layer_count; ++local_y) {
                            for (std::size_t local_z = 0; local_z < source_z_count; ++local_z) {
                                const auto source_z = source_z_begin + local_z;
                                const auto row_offset = (local_y * length + source_z) * width;
                                for (std::size_t source_x = source_x_begin;
                                     source_x < source_x_end; ++source_x) {
                                    const auto palette_index = slab[row_offset + source_x];
                                    if (!palette_non_air[palette_index]) continue;
                                    const auto runtime_id = runtime_palette[palette_index];
                                    const auto local_x = source_x - source_x_begin;
                                    // BedrockWorldAdapter can consume the native
                                    // x/y/z layout directly. Producing it here
                                    // avoids a second 4096-entry transpose for
                                    // every populated subchunk during encoding.
                                    sub_chunk.layer0[
                                        local_x * 256 + local_y * 16 + local_z] = runtime_id;
                                }
                            }
                        }
                        if (verify) {
                            auto checksum = static_cast<std::uint64_t>(
                                static_cast<std::int32_t>(slab_y / 16) + kOverworldMinY / 16);
                            for (const auto runtime_id : sub_chunk.layer0) checksum += runtime_id;
                            for (const auto runtime_id : sub_chunk.layer1) checksum += runtime_id;
                            materialized_checksums[chunk_x] = checksum;
                        }
                        chunks[chunk_x].layout = BlockLayerLayout::Native;
                        chunks[chunk_x].sub_chunks.emplace(target_sub_y, std::move(sub_chunk));
                        return Result<void>::success();
                    }));
            }
            for (auto& task : materialize_tasks) {
                try {
                    auto materialized = task.get();
                    if (!materialized) return materialized;
                } catch (const std::exception& error) {
                    return Result<void>::failure(
                        std::string("Schem chunk 实体化失败: ") + error.what());
                }
            }
            if (verify) {
                for (const auto checksum : materialized_checksums) {
                    verification_checksum += checksum;
                }
            }
            std::vector<ChunkWrite> writes;
            writes.reserve(chunk_x_count);
            for (std::size_t chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) {
                if (chunks[chunk_x].sub_chunks.empty()) continue;
                writes.push_back({
                    {
                        static_cast<std::int32_t>(chunk_x) + start.x,
                        static_cast<std::int32_t>(chunk_z) + start.z
                    },
                    &chunks[chunk_x]
                });
            }
            materialize_ms += elapsed_ms(materialize_start);

            const auto save_start = std::chrono::steady_clock::now();
            auto saved = world.save_chunks(writes);
            save_ms += elapsed_ms(save_start);
            if (!saved) return saved;

            if (slab_y + layer_count == height && callbacks.progress) {
                for (std::size_t chunk_x = 0; chunk_x < chunk_x_count; ++chunk_x) callbacks.progress();
            }
        }
    }
    // The streaming read intentionally defers varint validation until this
    // sequential pass.  Check that the expected block count consumed the
    // complete byte array, preserving the extra-varint rejection behavior.
    std::uint8_t trailing_byte = 0;
    if (encoded.get(trailing_byte)) {
        return Result<void>::failure("Schem BlockData 方块数超过 size");
    }
    if (profile) {
        std::cerr << "schem_world_profile source="
                  << (mBlockDataDeferred ? "gzip_direct" : "temp_file")
                  << " decode_ms=" << decode_ms
                  << " non_air_scan_ms=" << non_air_scan_ms
                  << " materialize_ms=" << materialize_ms
                  << " save_ms=" << save_ms
                  << '\n';
    }
    if (verify) std::cerr << "schem_world_checksum=" << verification_checksum << '\n';
    return Result<void>::success();
    };

    if (mBlockDataDeferred) {
        if (mSourcePath.empty()) {
            return Result<void>::failure("Schem 延迟 BlockData 缺少源文件路径");
        }
        return consume_deferred_block_data(
            mSourcePath, mFormat, mBlockDataBytes,
            [&write_from_reader](auto& encoded) -> Result<void> {
                return write_from_reader(encoded);
            });
    }

    EncodedReader encoded(mBlockDataPath);
    if (!encoded.good()) return Result<void>::failure("无法打开 Schem 临时 BlockData 文件");
    return write_from_reader(encoded);
}

Result<void> SchemStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(std::string(name()) + " 导出尚未迁移");
}

} // namespace water_structure
