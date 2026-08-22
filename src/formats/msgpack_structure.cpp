#include "msgpack_structure.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kMaxInputBytes = 2ull * 1024 * 1024 * 1024;

// The public msgpack object API is convenient, but unpack() retains the whole
// input and allocates one object for every block.  BDS/NP are simple records, so
// decode just the small subset needed by the format directly from a bounded
// stream.  This keeps peak memory proportional to the sparse store rather than
// to the encoded file size.
constexpr std::size_t kMaxContainerItems = 8ull * 1024 * 1024;
constexpr std::size_t kMaxStringBytes = 16ull * 1024 * 1024;
constexpr unsigned kMaxNestingDepth = 128;

class StreamDecoder {
public:
    explicit StreamDecoder(std::ifstream& input, std::uint64_t size)
        : mInput(input), mSize(size) {}

    std::uint64_t position() const noexcept { return mPosition; }

    std::uint8_t byte(std::string_view field)
    {
        if (mPosition >= mSize) fail(field);
        char value = 0;
        mInput.read(&value, 1);
        if (mInput.gcount() != 1) fail(field);
        ++mPosition;
        return static_cast<std::uint8_t>(static_cast<unsigned char>(value));
    }

    std::uint64_t unsigned_be(unsigned width, std::string_view field)
    {
        if (width == 0 || width > 8 || mPosition > mSize || width > mSize - mPosition) fail(field);
        std::array<std::uint8_t, 8> bytes{};
        mInput.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(width));
        if (mInput.gcount() != static_cast<std::streamsize>(width)) fail(field);
        mPosition += width;
        std::uint64_t result = 0;
        for (unsigned index = 0; index < width; ++index) result = (result << 8u) | bytes[index];
        return result;
    }

    std::size_t array_length(std::string_view field)
    {
        const auto marker = byte(field);
        std::uint64_t count = 0;
        if ((marker & 0xf0u) == 0x90u) count = marker & 0x0fu;
        else if (marker == 0xdcu) count = unsigned_be(2, field);
        else if (marker == 0xddu) count = unsigned_be(4, field);
        else fail(std::string(field) + " is not an array");
        if (count > kMaxContainerItems) fail(std::string(field) + " exceeds item limit");
        return static_cast<std::size_t>(count);
    }

    std::string string(std::string_view field)
    {
        const auto marker = byte(field);
        std::uint64_t length = 0;
        if ((marker & 0xe0u) == 0xa0u) length = marker & 0x1fu;
        else if (marker == 0xd9u) length = unsigned_be(1, field);
        else if (marker == 0xdau) length = unsigned_be(2, field);
        else if (marker == 0xdbu) length = unsigned_be(4, field);
        else fail(std::string(field) + " is not a string");
        if (mPosition > mSize || length > kMaxStringBytes || length > mSize - mPosition) fail(std::string(field) + " exceeds length limit");
        std::string result(static_cast<std::size_t>(length), '\0');
        if (length != 0) {
            mInput.read(result.data(), static_cast<std::streamsize>(length));
            if (mInput.gcount() != static_cast<std::streamsize>(length)) fail(field);
            mPosition += length;
        }
        return result;
    }

    std::int64_t integer(std::string_view field)
    {
        const auto marker = byte(field);
        if (marker <= 0x7fu) return marker;
        if (marker >= 0xe0u) return static_cast<std::int8_t>(marker);
        if (marker == 0xccu) return checked_unsigned(unsigned_be(1, field), field);
        if (marker == 0xcdu) return checked_unsigned(unsigned_be(2, field), field);
        if (marker == 0xceu) return checked_unsigned(unsigned_be(4, field), field);
        if (marker == 0xcfu) return checked_unsigned(unsigned_be(8, field), field);
        if (marker == 0xd0u) return static_cast<std::int8_t>(unsigned_be(1, field));
        if (marker == 0xd1u) return static_cast<std::int16_t>(unsigned_be(2, field));
        if (marker == 0xd2u) return static_cast<std::int32_t>(unsigned_be(4, field));
        if (marker == 0xd3u) return static_cast<std::int64_t>(unsigned_be(8, field));
        fail(std::string(field) + " is not an integer");
    }

    bool boolean(std::string_view field)
    {
        const auto marker = byte(field);
        if (marker == 0xc2u) return false;
        if (marker == 0xc3u) return true;
        fail(std::string(field) + " is not a boolean");
    }

    enum class DataKind : std::uint8_t { Other, Integer, String };
    struct Data {
        DataKind kind = DataKind::Other;
        std::int64_t integer = 0;
        std::string string;
    };

    Data data(std::string_view field)
    {
        const auto marker = byte(field);
        if (marker <= 0x7fu || marker >= 0xe0u ||
            (marker >= 0xccu && marker <= 0xd3u)) {
            return { DataKind::Integer, integer_after(marker, field), {} };
        }
        if ((marker & 0xe0u) == 0xa0u || marker == 0xd9u || marker == 0xdau || marker == 0xdbu) {
            return { DataKind::String, 0, string_after(marker, field) };
        }
        skip_after(marker, field, 0);
        return {};
    }

    void skip_value(std::string_view field) { skip_value(field, 0); }

private:
    [[noreturn]] void fail(std::string_view field) const
    {
        throw std::runtime_error("MessagePack " + std::string(field) +
            " at offset " + std::to_string(mPosition));
    }

    static std::int64_t checked_unsigned(std::uint64_t value, std::string_view field)
    {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("MessagePack " + std::string(field) + " exceeds int64");
        }
        return static_cast<std::int64_t>(value);
    }

    std::int64_t integer_after(std::uint8_t marker, std::string_view field)
    {
        if (marker <= 0x7fu) return marker;
        if (marker >= 0xe0u) return static_cast<std::int8_t>(marker);
        if (marker == 0xccu) return checked_unsigned(unsigned_be(1, field), field);
        if (marker == 0xcdu) return checked_unsigned(unsigned_be(2, field), field);
        if (marker == 0xceu) return checked_unsigned(unsigned_be(4, field), field);
        if (marker == 0xcfu) return checked_unsigned(unsigned_be(8, field), field);
        if (marker == 0xd0u) return static_cast<std::int8_t>(unsigned_be(1, field));
        if (marker == 0xd1u) return static_cast<std::int16_t>(unsigned_be(2, field));
        if (marker == 0xd2u) return static_cast<std::int32_t>(unsigned_be(4, field));
        if (marker == 0xd3u) return static_cast<std::int64_t>(unsigned_be(8, field));
        fail(std::string(field) + " is not an integer");
    }

    std::string string_after(std::uint8_t marker, std::string_view field)
    {
        std::uint64_t length = 0;
        if ((marker & 0xe0u) == 0xa0u) length = marker & 0x1fu;
        else if (marker == 0xd9u) length = unsigned_be(1, field);
        else if (marker == 0xdau) length = unsigned_be(2, field);
        else if (marker == 0xdbu) length = unsigned_be(4, field);
        else fail(std::string(field) + " is not a string");
        if (mPosition > mSize || length > kMaxStringBytes || length > mSize - mPosition) fail(std::string(field) + " exceeds length limit");
        std::string result(static_cast<std::size_t>(length), '\0');
        if (length != 0) {
            mInput.read(result.data(), static_cast<std::streamsize>(length));
            if (mInput.gcount() != static_cast<std::streamsize>(length)) fail(field);
            mPosition += length;
        }
        return result;
    }

    void skip_bytes(std::uint64_t length, std::string_view field)
    {
        if (mPosition > mSize || length > mSize - mPosition || length > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) fail(field);
        mInput.seekg(static_cast<std::streamoff>(length), std::ios::cur);
        if (!mInput) fail(field);
        mPosition += length;
    }

    void skip_value(std::string_view field, unsigned depth)
    {
        skip_after(byte(field), field, depth);
    }

    void skip_after(std::uint8_t marker, std::string_view field, unsigned depth)
    {
        if (depth > kMaxNestingDepth) fail(std::string(field) + " nesting is too deep");
        if (marker <= 0x7fu || marker >= 0xe0u || marker == 0xc0u || marker == 0xc2u || marker == 0xc3u) return;
        if ((marker & 0xe0u) == 0xa0u) { skip_bytes(marker & 0x1fu, field); return; }
        if ((marker & 0xf0u) == 0x90u) {
            const auto count = marker & 0x0fu;
            for (std::size_t i = 0; i < count; ++i) skip_value(field, depth + 1);
            return;
        }
        if ((marker & 0xf0u) == 0x80u) {
            const auto count = marker & 0x0fu;
            for (std::size_t i = 0; i < count * 2u; ++i) skip_value(field, depth + 1);
            return;
        }
        switch (marker) {
        case 0xccu: case 0xd0u: case 0xcau: skip_bytes(marker == 0xcau ? 4 : 1, field); return;
        case 0xcbu: skip_bytes(8, field); return;
        case 0xc1u: fail("reserved marker");
        case 0xc4u: skip_bytes(unsigned_be(1, field), field); return;
        case 0xc5u: skip_bytes(unsigned_be(2, field), field); return;
        case 0xc6u: skip_bytes(unsigned_be(4, field), field); return;
        case 0xc7u: { const auto n = unsigned_be(1, field); skip_bytes(1 + n, field); return; }
        case 0xc8u: { const auto n = unsigned_be(2, field); skip_bytes(1 + n, field); return; }
        case 0xc9u: { const auto n = unsigned_be(4, field); skip_bytes(1 + n, field); return; }
        case 0xd9u: skip_bytes(unsigned_be(1, field), field); return;
        case 0xdau: skip_bytes(unsigned_be(2, field), field); return;
        case 0xdbu: skip_bytes(unsigned_be(4, field), field); return;
        case 0xdcu: { const auto n = unsigned_be(2, field); if (n > kMaxContainerItems) fail(field); for (std::uint64_t i=0;i<n;++i) skip_value(field, depth + 1); return; }
        case 0xddu: { const auto n = unsigned_be(4, field); if (n > kMaxContainerItems) fail(field); for (std::uint64_t i=0;i<n;++i) skip_value(field, depth + 1); return; }
        case 0xdeu: { const auto n = unsigned_be(2, field); if (n > kMaxContainerItems) fail(field); for (std::uint64_t i=0;i<n*2u;++i) skip_value(field, depth + 1); return; }
        case 0xdfu: { const auto n = unsigned_be(4, field); if (n > kMaxContainerItems) fail(field); for (std::uint64_t i=0;i<n*2u;++i) skip_value(field, depth + 1); return; }
        case 0xcdu: skip_bytes(2, field); return;
        case 0xceu: skip_bytes(4, field); return;
        case 0xcfu: skip_bytes(8, field); return;
        case 0xd1u: skip_bytes(2, field); return;
        case 0xd2u: skip_bytes(4, field); return;
        case 0xd3u: skip_bytes(8, field); return;
        case 0xd4u: skip_bytes(2, field); return;
        case 0xd5u: skip_bytes(3, field); return;
        case 0xd6u: skip_bytes(5, field); return;
        case 0xd7u: skip_bytes(9, field); return;
        case 0xd8u: skip_bytes(17, field); return;
        case 0xc0u: case 0xc2u: case 0xc3u: return;
        default: fail(std::string(field) + " has unsupported marker");
        }
    }

    std::ifstream& mInput;
    std::uint64_t mSize = 0;
    std::uint64_t mPosition = 0;
};

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool equal_fold(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

std::uint32_t unknown_runtime_id(RuntimeRegistry& registry)
{
    if (const auto unknown = registry.find("minecraft:unknown")) return *unknown;
    return registry.register_state({ "minecraft:unknown", {}, 0 });
}

std::uint32_t base_runtime_id(RuntimeRegistry& registry, const std::string& name)
{
    if (const auto found = registry.find(name)) return *found;
    if (const auto found = registry.compatible_java_runtime_id(name)) return *found;
    return unknown_runtime_id(registry);
}

std::optional<std::int64_t> decimal_string(std::string_view text)
{
    if (text.empty()) return std::nullopt;
    if (text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return std::nullopt;
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return result;
}

bool valid_state_string(std::string_view text)
{
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') return false;
    text.remove_prefix(1);
    text.remove_suffix(1);
    if (text.empty()) return true;
    bool quoted = false;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index < text.size() && text[index] == '"') quoted = !quoted;
        if (index < text.size() && (text[index] != ',' || quoted)) continue;
        const auto part = text.substr(begin, index - begin);
        const auto equal = part.find('=');
        if (equal == std::string_view::npos || trim(std::string(part.substr(0, equal))).empty()) {
            return false;
        }
        begin = index + 1;
    }
    return !quoted;
}

std::uint32_t legacy_runtime_id(
    RuntimeRegistry& registry, const std::string& name, std::int64_t data)
{
    if (const auto found = registry.legacy_runtime_id(name, static_cast<std::uint16_t>(data))) {
        return *found;
    }
    return unknown_runtime_id(registry);
}

std::uint32_t bds_runtime_id(
    RuntimeRegistry& registry, const std::string& name, const StreamDecoder::Data& data)
{
    if (data.kind == StreamDecoder::DataKind::Integer) {
        return legacy_runtime_id(registry, name, data.integer);
    }
    if (data.kind != StreamDecoder::DataKind::String) return base_runtime_id(registry, name);

    const auto encoded = trim(data.string);
    if (encoded.empty()) return base_runtime_id(registry, name);
    if (const auto numeric = decimal_string(encoded)) {
        return legacy_runtime_id(registry, name, *numeric);
    }
    if (!valid_state_string(encoded)) return base_runtime_id(registry, name);
    if (encoded == "[]") return base_runtime_id(registry, name);
    if (const auto found = registry.java_runtime_id(name + encoded)) return *found;
    if (const auto found = registry.compatible_java_runtime_id(name + encoded)) return *found;
    return unknown_runtime_id(registry);
}

std::int32_t coordinate(std::int64_t decoded, std::string_view field)
{
    if (decoded < std::numeric_limits<std::int32_t>::min() ||
        decoded > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(decoded);
}

} // namespace

std::string_view MsgpackStructure::name() const noexcept
{
    return mFormat == StructureId::BDS ? "BDS" : "NexusNP";
}

void MsgpackStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mChunkIndex.clear();
    const auto expanded = [](std::int32_t base, std::int32_t delta) noexcept {
        const auto magnitude = delta < 0
            ? -static_cast<std::int64_t>(delta)
            : static_cast<std::int64_t>(delta);
        const auto value = static_cast<std::int64_t>(base) + magnitude;
        return value > std::numeric_limits<std::int32_t>::max()
            ? std::numeric_limits<std::int32_t>::max()
            : static_cast<std::int32_t>(value);
    };
    mSize = {
        expanded(mOriginalSize.width, offset.x),
        expanded(mOriginalSize.height, offset.y),
        expanded(mOriginalSize.length, offset.z)
    };
}

Result<void> MsgpackStructure::read(const std::filesystem::path& path)
{
    mBlocks.clear();
    mChunkIndex.clear();
    mOriginalSize = {};
    mSize = {};
    mOffset = {};
    mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open MessagePack file: " + path.string());
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0 || static_cast<std::uint64_t>(end) > kMaxInputBytes) {
            throw std::runtime_error("MessagePack input exceeds 2 GiB");
        }
        const auto file_size = static_cast<std::uint64_t>(end);
        input.seekg(0, std::ios::beg);
        BlockPos minimum{}, maximum{};
        bool populated = false;
        auto add_bounds = [&](BlockPos pos) {
            if (!populated) { populated = true; minimum = maximum = pos; return; }
            minimum.x = std::min(minimum.x, pos.x); minimum.y = std::min(minimum.y, pos.y);
            minimum.z = std::min(minimum.z, pos.z); maximum.x = std::max(maximum.x, pos.x);
            maximum.y = std::max(maximum.y, pos.y); maximum.z = std::max(maximum.z, pos.z);
        };

        auto scan = [&](bool materialize) {
            input.clear();
            input.seekg(0, std::ios::beg);
            if (!input) throw std::runtime_error("cannot rewind MessagePack input");
            StreamDecoder decoder(input, file_size);
            const auto top_size = decoder.array_length("top-level array");
            if (top_size < (mFormat == StructureId::BDS ? 1u : 2u)) {
                throw std::runtime_error("top-level array is too short");
            }
            const auto block_count = decoder.array_length(
                mFormat == StructureId::BDS ? "BDS blocks" : "NP block_data");
            if (block_count == 0) throw std::runtime_error("block array is empty");

            for (std::size_t index = 0; index < block_count; ++index) {
                try {
                    const auto entry_size = decoder.array_length("block entry");
                    const auto minimum_fields = mFormat == StructureId::BDS ? 6u : 5u;
                    if (entry_size < minimum_fields) {
                        throw std::runtime_error("block entry is too short");
                    }
                    const auto raw_block_name = decoder.string("block name");
                    const auto block_name = trim(raw_block_name);
                    const BlockPos pos{
                        coordinate(decoder.integer("x"), "x"),
                        coordinate(decoder.integer("y"), "y"),
                        coordinate(decoder.integer("z"), "z") };
                    const auto data = decoder.data("block data");
                    if (mFormat == StructureId::NexusNP &&
                        data.kind != StreamDecoder::DataKind::Integer) {
                        throw std::runtime_error("block data is not an integer");
                    }
                    bool air = false;
                    if (mFormat == StructureId::BDS) air = decoder.boolean("air flag");
                    for (std::size_t extra = minimum_fields; extra < entry_size; ++extra) {
                        decoder.skip_value("block extension");
                    }

                    // Go trims only to detect an empty name. EqualFold is applied
                    // to the original string, so a space-padded air name remains a
                    // (usually unknown) non-air block.
                    if (air || block_name.empty() ||
                        equal_fold(raw_block_name, "minecraft:air")) continue;
                    if (!materialize) {
                        add_bounds(pos);
                        ++mNonAirBlocks;
                        continue;
                    }
                    const auto runtime = mFormat == StructureId::BDS
                        ? bds_runtime_id(mRegistry, block_name, data)
                        : legacy_runtime_id(mRegistry, block_name, data.integer);
                    mBlocks.push_back({ pos.x - minimum.x, pos.y - minimum.y,
                        pos.z - minimum.z, runtime });
                } catch (const std::exception& error) {
                    throw std::runtime_error(std::string(name()) + " block index " +
                        std::to_string(index) + ": " + error.what());
                }
            }
            for (std::size_t extra = 1; extra < top_size; ++extra) {
                decoder.skip_value("top-level extension");
            }
        };

        // The first pass establishes bounds and validates the stream. The second
        // pass writes directly into compact vector storage, avoiding a second
        // full map of every decoded block.
        scan(false);
        if (!populated) throw std::runtime_error("structure has no non-air blocks");
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (width <= 0 || height <= 0 || length <= 0 || width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() || length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("structure bounds are invalid");
        }
        mOriginalSize = { static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height), static_cast<std::int32_t>(length) };
        mSize = mOriginalSize;
        mBlocks.reserve(mNonAirBlocks);
        scan(true);
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse " + std::string(name()) + " failed: " + error.what());
    }
}

Result<ChunkMap> MsgpackStructure::get_chunks(
    std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, true);
}

Result<ChunkMap> MsgpackStructure::get_chunks_layer0(
    std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, false);
}

Result<ChunkMap> MsgpackStructure::get_chunks_impl(
    std::span<const ChunkPos> positions, bool include_layer1) const
{
    ChunkMap result;
    for (const auto position : positions) result.emplace(position, ChunkData{});
    for (const auto& block : mBlocks) {
        const auto x = static_cast<std::int64_t>(block.x) + mOffset.x;
        const auto y = static_cast<std::int64_t>(block.y) + mOffset.y;
        const auto z = static_cast<std::int64_t>(block.z) + mOffset.z;
        if (x < std::numeric_limits<std::int32_t>::min() ||
            x > std::numeric_limits<std::int32_t>::max() ||
            y < std::numeric_limits<std::int32_t>::min() ||
            y > std::numeric_limits<std::int32_t>::max() ||
            z < std::numeric_limits<std::int32_t>::min() ||
            z > std::numeric_limits<std::int32_t>::max()) {
            return Result<ChunkMap>::failure(
                std::string(name()) + " offset moves block outside int32 range");
        }
    }
    if (!mChunkIndex.ensure(mBlocks, mOffset, [](const Block& block) {
            return BlockPos{ block.x, block.y, block.z };
        })) {
        return Result<ChunkMap>::failure(
            std::string(name()) + " chunk index exceeds uint32 capacity");
    }
    const auto air = mRegistry.air_runtime_id();
    for (auto& [chunk_position, chunk] : result) {
        const auto* indices = mChunkIndex.find(chunk_position);
        if (!indices) continue;
        for (const auto index : *indices) {
            const auto& block = mBlocks[index];
            const auto x = static_cast<std::int64_t>(block.x) + mOffset.x;
            const auto y = static_cast<std::int64_t>(block.y) + mOffset.y;
            const auto z = static_cast<std::int64_t>(block.z) + mOffset.z;
            const auto storage_y = y + kOverworldMinY;
            const auto sub_y_64 = floor_div64(storage_y, 16);
            if (sub_y_64 < std::numeric_limits<std::int32_t>::min() ||
                sub_y_64 > std::numeric_limits<std::int32_t>::max()) {
                return Result<ChunkMap>::failure(
                    std::string(name()) + " subchunk Y exceeds int32 range");
            }
            const auto sub_y = static_cast<std::int32_t>(sub_y_64);
            const auto chunk_min_x = static_cast<std::int64_t>(chunk_position.x) * 16;
            const auto chunk_min_z = static_cast<std::int64_t>(chunk_position.z) * 16;
            const auto local_x = x - chunk_min_x;
            const auto local_y = storage_y - sub_y_64 * 16;
            const auto local_z = z - chunk_min_z;
            if (local_x < 0 || local_x >= 16 || local_y < 0 || local_y >= 16 ||
                local_z < 0 || local_z >= 16) {
                return Result<ChunkMap>::failure(
                    std::string(name()) + " block materializes outside subchunk: world=(" +
                    std::to_string(x) + "," + std::to_string(y) + "," +
                    std::to_string(z) + "), chunk=(" +
                    std::to_string(chunk_position.x) + "," +
                    std::to_string(chunk_position.z) + "), subY=" +
                    std::to_string(sub_y) + ", local=(" +
                    std::to_string(local_x) + "," + std::to_string(local_y) + "," +
                    std::to_string(local_z) + ")");
            }
            auto [subchunk, inserted] = chunk.sub_chunks.try_emplace(sub_y);
            if (inserted) {
                subchunk->second.layer0.fill(air);
                if (include_layer1) subchunk->second.layer1.fill(air);
            }
            subchunk->second.layer0[static_cast<std::size_t>(
                (local_y * 16 + local_z) * 16 + local_x)] = block.runtime;
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> MsgpackStructure::get_chunk_nbt(
    std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto position : positions) {
        result.emplace(position, std::vector<BlockEntity>{});
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> MsgpackStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> MsgpackStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(std::string(name()) + " file writer is not implemented yet");
}

} // namespace water_structure
