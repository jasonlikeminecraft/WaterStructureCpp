#include "construction.hpp"

#include <WaterStructure/world.hpp>

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <nbt_tags.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::string_view kMagic = "constrct";
constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kIndexEntrySize = 23;

using SectionIndex = ConstructionReaderState::SectionIndex;

const nbt::value* find_value(const nbt::tag_compound& compound, std::string_view key)
{
    const auto text = std::string(key);
    return compound.has_key(text) ? &compound.at(text) : nullptr;
}

std::optional<std::int32_t> int_value(const nbt::value* value)
{
    if (!value) return std::nullopt;
    switch (value->get_type()) {
    case nbt::tag_type::Byte: return value->as<nbt::tag_byte>().get();
    case nbt::tag_type::Short: return value->as<nbt::tag_short>().get();
    case nbt::tag_type::Int: return value->as<nbt::tag_int>().get();
    case nbt::tag_type::Long: return static_cast<std::int32_t>(value->as<nbt::tag_long>().get());
    default: return std::nullopt;
    }
}

std::optional<std::string> string_value(const nbt::value* value)
{
    if (!value || value->get_type() != nbt::tag_type::String) return std::nullopt;
    return value->as<nbt::tag_string>().get();
}

std::uint32_t read_be_u32(std::span<const std::uint8_t, 4> bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) |
        static_cast<std::uint32_t>(bytes[3]);
}

std::int32_t read_le_i32(const std::uint8_t* bytes)
{
    const auto value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    return static_cast<std::int32_t>(value);
}

std::vector<std::uint8_t> read_exact(
    std::ifstream& input, std::uint64_t offset, std::size_t length, std::string_view context)
{
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        length > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error(std::string(context) + " range exceeds stream limits at file offset " +
            std::to_string(offset));
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) throw std::runtime_error(std::string(context) + " seek offset " + std::to_string(offset) + " failed");
    std::vector<std::uint8_t> bytes(length);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (input.gcount() != static_cast<std::streamsize>(length)) {
        throw std::runtime_error(std::string(context) + " truncated at file offset " +
            std::to_string(offset + static_cast<std::uint64_t>(std::max<std::streamsize>(0, input.gcount()))));
    }
    return bytes;
}

// Presents one validated file range as a forward-only stream.  Payloads are
// read in fixed-size windows, so a large compressed section never needs a
// same-sized input allocation.
class FileRangeStreamBuf final : public std::streambuf {
public:
    FileRangeStreamBuf(std::ifstream& input, std::uint64_t offset,
        std::uint64_t length, std::string_view context)
        : mInput(input), mRemaining(length), mOffset(offset), mContext(context)
    {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::runtime_error(mContext + " range exceeds stream limits at file offset " +
                std::to_string(offset));
        }
        mInput.clear();
        mInput.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!mInput) {
            throw std::runtime_error(mContext + " seek offset " + std::to_string(offset) + " failed");
        }
    }

protected:
    int_type underflow() override
    {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (mRemaining == 0) return traits_type::eof();
        const auto requested = static_cast<std::streamsize>(std::min<std::uint64_t>(
            mRemaining, static_cast<std::uint64_t>(mBuffer.size())));
        mInput.read(mBuffer.data(), requested);
        const auto count = mInput.gcount();
        if (count <= 0) {
            throw std::runtime_error(mContext + " truncated at file offset " +
                std::to_string(mOffset));
        }
        const auto consumed = static_cast<std::uint64_t>(count);
        mRemaining -= consumed;
        mOffset += consumed;
        setg(mBuffer.data(), mBuffer.data(), mBuffer.data() + count);
        return traits_type::to_int_type(*gptr());
    }

private:
    std::ifstream& mInput;
    std::uint64_t mRemaining = 0;
    std::uint64_t mOffset = 0;
    std::string mContext;
    std::array<char, kStreamChunk> mBuffer{};
};

// Incrementally inflates gzip/zlib bytes from another streambuf.  libnbt reads
// this stream directly, eliminating both the compressed-payload vector and the
// fully decoded byte vector used by the previous implementation.
class InflateStreamBuf final : public std::streambuf {
public:
    InflateStreamBuf(std::istream& source, int window_bits, std::string_view context)
        : mSource(source), mContext(context)
    {
        if (inflateInit2(&mStream, window_bits) != Z_OK) {
            throw std::runtime_error(mContext + " inflate initialization failed");
        }
        mInitialized = true;
    }

    ~InflateStreamBuf() override
    {
        if (mInitialized) inflateEnd(&mStream);
    }

    InflateStreamBuf(const InflateStreamBuf&) = delete;
    InflateStreamBuf& operator=(const InflateStreamBuf&) = delete;

protected:
    int_type underflow() override
    {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (mFinished) return traits_type::eof();

        while (true) {
            if (mStream.avail_in == 0 && !mSourceFinished) refill_input();

            mStream.next_out = reinterpret_cast<Bytef*>(mOutput.data());
            mStream.avail_out = static_cast<uInt>(mOutput.size());
            const auto before_in = mStream.avail_in;
            const auto status = inflate(&mStream, Z_NO_FLUSH);
            const auto produced = mOutput.size() - mStream.avail_out;
            if (mDecodedBytes > std::numeric_limits<std::uint64_t>::max() - produced) {
                throw std::runtime_error(mContext + " decoded byte count overflow");
            }
            mDecodedBytes += produced;

            if (status == Z_STREAM_END) mFinished = true;
            else if (status == Z_BUF_ERROR && mSourceFinished && mStream.avail_in == 0) {
                throw std::runtime_error(mContext + " compressed payload is truncated at offset " +
                    std::to_string(mStream.total_in));
            } else if (status != Z_OK) {
                throw_inflate_error(status);
            }

            if (produced != 0) {
                setg(mOutput.data(), mOutput.data(),
                    mOutput.data() + static_cast<std::ptrdiff_t>(produced));
                return traits_type::to_int_type(*gptr());
            }
            if (mFinished) return traits_type::eof();
            if (mSourceFinished && mStream.avail_in == 0) {
                throw std::runtime_error(mContext + " compressed payload is truncated at offset " +
                    std::to_string(mStream.total_in));
            }
            if (before_in == mStream.avail_in) {
                throw std::runtime_error(mContext + " inflate made no progress at compressed offset " +
                    std::to_string(mStream.total_in));
            }
        }
    }

private:
    void refill_input()
    {
        mSource.read(reinterpret_cast<char*>(mInput.data()),
            static_cast<std::streamsize>(mInput.size()));
        const auto count = mSource.gcount();
        if (count <= 0) {
            mSourceFinished = true;
            return;
        }
        mStream.next_in = mInput.data();
        mStream.avail_in = static_cast<uInt>(count);
    }

    [[noreturn]] void throw_inflate_error(int status) const
    {
        throw std::runtime_error(mContext + " inflate failed at compressed offset " +
            std::to_string(mStream.total_in) + ": " +
            (mStream.msg == nullptr ? std::to_string(status) : mStream.msg));
    }

    std::istream& mSource;
    std::string mContext;
    z_stream mStream{};
    bool mInitialized = false;
    bool mSourceFinished = false;
    bool mFinished = false;
    std::uint64_t mDecodedBytes = 0;
    std::array<Bytef, kStreamChunk> mInput{};
    std::array<char, kStreamChunk> mOutput{};
};

void drain_stream(std::istream& input)
{
    std::array<char, kStreamChunk> discard{};
    while (input.read(discard.data(), static_cast<std::streamsize>(discard.size())) ||
        input.gcount() != 0) {
    }
}

std::unique_ptr<nbt::tag_compound> parse_big_endian_compound(
    std::istream& input, std::string_view context)
{
    try {
        auto [_, root] = nbt::io::read_compound(input, endian::big);
        return std::move(root);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(context) + " NBT parse failed: " + error.what());
    }
}

std::unique_ptr<nbt::tag_compound> read_big_endian_compound(
    std::ifstream& file, std::uint64_t offset, std::uint64_t length,
    std::string_view context)
{
    const auto prefix_length = static_cast<std::size_t>(std::min<std::uint64_t>(length, 2));
    const auto prefix = read_exact(file, offset, prefix_length, context);
    const auto gzip = prefix.size() == 2 && prefix[0] == 0x1f && prefix[1] == 0x8b;
    const auto zlib_stream = prefix.size() == 2 && prefix[0] == 0x78;

    FileRangeStreamBuf range_buffer(file, offset, length, context);
    std::istream range_input(&range_buffer);
    range_input.exceptions(std::ios::badbit);
    if (!gzip && !zlib_stream) {
        auto root = parse_big_endian_compound(range_input, context);
        drain_stream(range_input);
        return root;
    }

    InflateStreamBuf inflate_buffer(range_input,
        gzip ? MAX_WBITS + 16 : MAX_WBITS, context);
    std::istream inflated_input(&inflate_buffer);
    inflated_input.exceptions(std::ios::badbit);
    auto root = parse_big_endian_compound(inflated_input, context);
    // Parsing the root may finish before zlib has consumed its trailer.  Drain
    // the stream so CRC/truncation errors remain observable without retaining
    // the decoded bytes.
    drain_stream(inflated_input);
    return root;
}

std::uint64_t checked_product3(std::uint64_t first, std::uint64_t second,
    std::uint64_t third, std::string_view context)
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (first != 0 && second > maximum / first) {
        throw std::runtime_error(std::string(context) + " volume overflow");
    }
    const auto product = first * second;
    if (product != 0 && third > maximum / product) {
        throw std::runtime_error(std::string(context) + " volume overflow");
    }
    return product * third;
}

std::vector<SectionIndex> parse_index(const nbt::tag_byte_array& raw)
{
    if (raw.size() == 0 || raw.size() % kIndexEntrySize != 0) {
        throw std::runtime_error("Construction section index length is invalid: " +
            std::to_string(raw.size()));
    }
    std::vector<SectionIndex> result;
    result.reserve(raw.size() / kIndexEntrySize);
    const auto* data = reinterpret_cast<const std::uint8_t*>(raw.get().data());
    for (std::size_t offset = 0; offset < raw.size(); offset += kIndexEntrySize) {
        result.push_back({
            read_le_i32(data + offset), read_le_i32(data + offset + 4),
            read_le_i32(data + offset + 8), data[offset + 12], data[offset + 13], data[offset + 14],
            read_le_i32(data + offset + 15), read_le_i32(data + offset + 19)
        });
    }
    return result;
}

std::unique_ptr<nbt::tag> normalize_nbt(const nbt::value& value)
{
    switch (value.get_type()) {
    case nbt::tag_type::Byte:
        return std::make_unique<nbt::tag_int>(value.as<nbt::tag_byte>().get());
    case nbt::tag_type::Short:
        return std::make_unique<nbt::tag_int>(value.as<nbt::tag_short>().get());
    case nbt::tag_type::Int:
        return std::make_unique<nbt::tag_int>(value.as<nbt::tag_int>().get());
    case nbt::tag_type::Long:
        return std::make_unique<nbt::tag_int>(static_cast<std::int32_t>(value.as<nbt::tag_long>().get()));
    case nbt::tag_type::Float:
        return std::make_unique<nbt::tag_double>(value.as<nbt::tag_float>().get());
    case nbt::tag_type::Double:
        return std::make_unique<nbt::tag_double>(value.as<nbt::tag_double>().get());
    case nbt::tag_type::Byte_Array:
        return value.get().clone();
    case nbt::tag_type::String:
        return value.get().clone();
    case nbt::tag_type::List: {
        auto result = std::make_unique<nbt::tag_list>();
        for (const auto& item : value.as<nbt::tag_list>()) {
            result->push_back(nbt::value_initializer(normalize_nbt(item)));
        }
        return result;
    }
    case nbt::tag_type::Compound: {
        auto result = std::make_unique<nbt::tag_compound>();
        for (const auto& [key, item] : value.as<nbt::tag_compound>()) {
            result->put(key, nbt::value_initializer(normalize_nbt(item)));
        }
        return result;
    }
    case nbt::tag_type::Int_Array:
    case nbt::tag_type::Long_Array:
        return value.get().clone();
    default:
        throw std::runtime_error("Construction NBT contains unsupported tag type");
    }
}

NbtPayload serialize_compound(const nbt::tag_compound& compound)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", compound, output, endian::little);
    const auto bytes = output.str();
    return NbtPayload(bytes.begin(), bytes.end());
}

std::vector<BlockStateProperty> palette_property_variants(
    std::string name, const nbt::value& value)
{
    std::vector<BlockStateProperty> result;
    auto add_numeric = [&](std::string text) {
        result.push_back({ name, BlockStateValueType::Int, std::move(text) });
    };
    switch (value.get_type()) {
    case nbt::tag_type::Byte:
        add_numeric(std::to_string(value.as<nbt::tag_byte>().get()));
        break;
    case nbt::tag_type::Short:
        add_numeric(std::to_string(value.as<nbt::tag_short>().get()));
        break;
    case nbt::tag_type::Int:
        add_numeric(std::to_string(value.as<nbt::tag_int>().get()));
        break;
    case nbt::tag_type::Long:
        add_numeric(std::to_string(static_cast<std::int32_t>(value.as<nbt::tag_long>().get())));
        break;
    case nbt::tag_type::String:
        result.push_back({ name, BlockStateValueType::String, value.as<nbt::tag_string>().get() });
        break;
    default:
        break;
    }
    return result;
}

std::uint32_t palette_runtime_id(RuntimeRegistry& registry, const nbt::tag_compound& entry)
{
    const auto name_space = string_value(find_value(entry, "namespace"));
    const auto block_name = string_value(find_value(entry, "blockname"));
    if (!name_space || !block_name) throw std::runtime_error("Construction palette entry is missing namespace/blockname");
    const auto name = *name_space + ":" + *block_name;
    const auto default_runtime = registry.find(name);
    std::vector<std::pair<std::string, std::vector<BlockStateProperty>>> property_options;
    if (const auto* raw = find_value(entry, "properties"); raw && raw->get_type() == nbt::tag_type::Compound) {
        for (const auto& [property_name, value] : raw->as<nbt::tag_compound>()) {
            if (property_name == "__version__") continue;
            auto variants = palette_property_variants(property_name, value);
            if (!variants.empty()) property_options.emplace_back(property_name, std::move(variants));
        }
    }
    std::vector<BlockStateProperty> properties;
    std::function<std::optional<std::uint32_t>(std::size_t)> try_properties = [&](std::size_t index) {
        if (index == property_options.size()) return registry.find(name, properties);
        for (const auto& property : property_options[index].second) {
            properties.push_back(property);
            if (const auto runtime = try_properties(index + 1)) return runtime;
            properties.pop_back();
        }
        return std::optional<std::uint32_t>{};
    };
    if (const auto runtime = try_properties(0)) return *runtime;
    // Go's StateToRuntimeID retries the block's registered default property set.
    if (default_runtime) return *default_runtime;
    if (const auto unknown = registry.find("minecraft:unknown")) return *unknown;
    return registry.register_state({ "minecraft:unknown", {}, 0 });
}

struct SectionBlocks {
    const nbt::tag_byte_array* bytes = nullptr;
    const nbt::tag_int_array* ints = nullptr;
    const nbt::tag_long_array* longs = nullptr;

    std::size_t size() const noexcept
    {
        if (bytes) return bytes->size();
        if (ints) return ints->size();
        if (longs) return longs->size();
        return 0;
    }

    std::int32_t operator[](std::size_t index) const
    {
        if (bytes) return static_cast<std::uint8_t>(bytes->get()[index]);
        if (ints) return ints->get()[index];
        if (longs) return static_cast<std::int32_t>(longs->get()[index]);
        throw std::out_of_range("Construction blocks view is empty");
    }
};

SectionBlocks section_blocks(const nbt::tag_compound& section)
{
    const auto array_type = int_value(find_value(section, "blocks_array_type"));
    const auto* blocks = find_value(section, "blocks");
    if (!array_type || !blocks) return {};
    if (*array_type == 7 && blocks->get_type() == nbt::tag_type::Byte_Array) {
        return { &blocks->as<nbt::tag_byte_array>(), nullptr, nullptr };
    } else if (*array_type == 11 && blocks->get_type() == nbt::tag_type::Int_Array) {
        return { nullptr, &blocks->as<nbt::tag_int_array>(), nullptr };
    } else if (*array_type == 12 && blocks->get_type() == nbt::tag_type::Long_Array) {
        return { nullptr, nullptr, &blocks->as<nbt::tag_long_array>() };
    } else {
        throw std::runtime_error("Construction blocks type does not match blocks_array_type " +
            std::to_string(*array_type));
    }
}

std::array<std::int32_t, 3> section_shape(const nbt::tag_compound& section)
{
    for (const auto key : { "shape", "size" }) {
        const auto* raw = find_value(section, key);
        if (!raw || raw->get_type() != nbt::tag_type::List) continue;
        const auto& values = raw->as<nbt::tag_list>();
        if (values.size() != 3) continue;
        const auto x = int_value(&values.at(0));
        const auto y = int_value(&values.at(1));
        const auto z = int_value(&values.at(2));
        if (x && y && z) return { *x, *y, *z };
    }
    return {};
}

} // namespace

Result<void> ConstructionReader::read(const std::filesystem::path& path)
{
    mStore.clear();
    mState = {};
    mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open Construction file: " + path.string());
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < static_cast<std::streamoff>(kMagic.size() * 2 + 5)) {
            throw std::runtime_error("file is truncated at offset " + std::to_string(std::max<std::streamoff>(0, end)));
        }
        const auto file_size = static_cast<std::uint64_t>(end);
        const auto header = read_exact(input, 0, kMagic.size() + 1, "Construction header");
        if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
            throw std::runtime_error("invalid header magic at offset 0");
        }
        if (header[kMagic.size()] != 0) {
            throw std::runtime_error("unsupported format version " + std::to_string(header[kMagic.size()]) +
                " at offset " + std::to_string(kMagic.size()));
        }
        const auto footer = read_exact(input, file_size - 12, 12, "Construction footer");
        if (std::memcmp(footer.data() + 4, kMagic.data(), kMagic.size()) != 0) {
            throw std::runtime_error("invalid footer magic at offset " + std::to_string(file_size - kMagic.size()));
        }
        std::array<std::uint8_t, 4> pointer_bytes{};
        std::copy_n(footer.begin(), 4, pointer_bytes.begin());
        const auto metadata_start = static_cast<std::uint64_t>(read_be_u32(pointer_bytes));
        const auto metadata_end = file_size - 12;
        if (metadata_start < kMagic.size() + 1 || metadata_start >= metadata_end) {
            throw std::runtime_error("invalid metadata pointer " + std::to_string(metadata_start) +
                " at file offset " + std::to_string(file_size - 12));
        }
        auto metadata = read_big_endian_compound(input, metadata_start,
            metadata_end - metadata_start, "Construction metadata");

        const auto* index_value = find_value(*metadata, "section_index_table");
        const auto* palette_value = find_value(*metadata, "block_palette");
        if (!index_value || index_value->get_type() != nbt::tag_type::Byte_Array) {
            throw std::runtime_error("Construction metadata section_index_table is missing or invalid");
        }
        if (!palette_value || palette_value->get_type() != nbt::tag_type::List ||
            palette_value->as<nbt::tag_list>().size() == 0) {
            throw std::runtime_error("Construction metadata block_palette is missing or empty");
        }
        auto sections = parse_index(index_value->as<nbt::tag_byte_array>());

        std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_y = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_z = std::numeric_limits<std::int32_t>::max();
        std::int32_t max_x = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_y = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_z = std::numeric_limits<std::int32_t>::min();
        if (const auto* boxes = find_value(*metadata, "selection_boxes");
            boxes && boxes->get_type() == nbt::tag_type::Int_Array) {
            const auto& values = boxes->as<nbt::tag_int_array>();
            for (std::size_t index = 0; index + 5 < values.size(); index += 6) {
                min_x = std::min(min_x, values[index]);
                min_y = std::min(min_y, values[index + 1]);
                min_z = std::min(min_z, values[index + 2]);
                max_x = std::max(max_x, values[index + 3]);
                max_y = std::max(max_y, values[index + 4]);
                max_z = std::max(max_z, values[index + 5]);
            }
        }
        if (max_x <= min_x || max_y <= min_y || max_z <= min_z) {
            min_x = min_y = min_z = std::numeric_limits<std::int32_t>::max();
            max_x = max_y = max_z = std::numeric_limits<std::int32_t>::min();
            for (const auto& section : sections) {
                if (section.shape_x == 0 || section.shape_y == 0 || section.shape_z == 0) continue;
                const auto end_x = static_cast<std::int64_t>(section.start_x) + section.shape_x;
                const auto end_y = static_cast<std::int64_t>(section.start_y) + section.shape_y;
                const auto end_z = static_cast<std::int64_t>(section.start_z) + section.shape_z;
                if (end_x > std::numeric_limits<std::int32_t>::max() ||
                    end_y > std::numeric_limits<std::int32_t>::max() ||
                    end_z > std::numeric_limits<std::int32_t>::max()) {
                    throw std::runtime_error("Construction section bounds overflow int32");
                }
                min_x = std::min(min_x, section.start_x);
                min_y = std::min(min_y, section.start_y);
                min_z = std::min(min_z, section.start_z);
                max_x = std::max(max_x, static_cast<std::int32_t>(end_x));
                max_y = std::max(max_y, static_cast<std::int32_t>(end_y));
                max_z = std::max(max_z, static_cast<std::int32_t>(end_z));
            }
        }
        if (max_x <= min_x || max_y <= min_y || max_z <= min_z) {
            throw std::runtime_error("Construction bounds are invalid");
        }
        const auto width64 = static_cast<std::int64_t>(max_x) - min_x;
        const auto height64 = static_cast<std::int64_t>(max_y) - min_y;
        const auto length64 = static_cast<std::int64_t>(max_z) - min_z;
        if (width64 <= 0 || height64 <= 0 || length64 <= 0 ||
            width64 > std::numeric_limits<std::int32_t>::max() ||
            height64 > std::numeric_limits<std::int32_t>::max() ||
            length64 > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("Construction size exceeds int32 range");
        }
        const auto volume64 = checked_product3(static_cast<std::uint64_t>(width64),
            static_cast<std::uint64_t>(height64), static_cast<std::uint64_t>(length64),
            "Construction size");
        if (volume64 > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("Construction size volume exceeds addressable range");
        }
        const Size size{ static_cast<std::int32_t>(width64),
            static_cast<std::int32_t>(height64), static_cast<std::int32_t>(length64) };
        mStore.set_size(size);

        std::vector<std::uint32_t> palette;
        palette.reserve(palette_value->as<nbt::tag_list>().size());
        for (const auto& raw : palette_value->as<nbt::tag_list>()) {
            if (raw.get_type() != nbt::tag_type::Compound) {
                throw std::runtime_error("Construction palette entry is not a compound");
            }
            palette.push_back(palette_runtime_id(mRegistry, raw.as<nbt::tag_compound>()));
        }
        const auto unknown = mRegistry.find("minecraft:unknown").value_or(
            mRegistry.register_state({ "minecraft:unknown", {}, 0 }));
        // The section index and runtime palette are now self-contained.  Drop
        // the metadata DOM before opening section payloads so the two NBT trees
        // do not contribute to peak memory at the same time.
        metadata.reset();

        for (std::size_t section_index = 0; section_index < sections.size(); ++section_index) {
            const auto& entry = sections[section_index];
            if (entry.length <= 0 || entry.shape_x == 0 || entry.shape_y == 0 || entry.shape_z == 0) continue;
            if (entry.position < 0) {
                throw std::runtime_error("Construction section #" + std::to_string(section_index) +
                    " has invalid range at file offset " + std::to_string(entry.position));
            }
            const auto section_start = static_cast<std::uint64_t>(entry.position);
            const auto section_length = static_cast<std::uint64_t>(entry.length);
            if (section_start > file_size || section_length > file_size - section_start) {
                throw std::runtime_error("Construction section #" + std::to_string(section_index) +
                    " has invalid range at file offset " + std::to_string(entry.position));
            }
            const auto section_context = "Construction section #" + std::to_string(section_index);
            const auto section = read_big_endian_compound(input,
                section_start, section_length, section_context);
            const auto blocks = section_blocks(*section);
            const auto parsed_shape = section_shape(*section);
            if (parsed_shape[0] > 0 && parsed_shape[1] > 0 && parsed_shape[2] > 0) {
                const auto expected = checked_product3(
                    static_cast<std::uint64_t>(parsed_shape[0]),
                    static_cast<std::uint64_t>(parsed_shape[1]),
                    static_cast<std::uint64_t>(parsed_shape[2]), section_context);
                if (expected != blocks.size()) {
                    throw std::runtime_error("Construction section #" + std::to_string(section_index) +
                        " block count mismatch");
                }
            }
            const auto indexed_volume = static_cast<std::size_t>(entry.shape_x) * entry.shape_y * entry.shape_z;
            if (blocks.size() != 0 && blocks.size() < indexed_volume) {
                throw std::runtime_error("Construction section #" + std::to_string(section_index) +
                    " blocks are truncated at index " + std::to_string(blocks.size()));
            }
            if (blocks.size() != 0) {
                for (std::int32_t x = 0; x < entry.shape_x; ++x) {
                    for (std::int32_t y = 0; y < entry.shape_y; ++y) {
                        for (std::int32_t z = 0; z < entry.shape_z; ++z) {
                            const auto index = static_cast<std::size_t>(
                                (x * entry.shape_y + y) * entry.shape_z + z);
                            const auto palette_index = blocks[index];
                            const auto runtime = palette_index >= 0 &&
                                static_cast<std::size_t>(palette_index) < palette.size()
                                ? palette[static_cast<std::size_t>(palette_index)] : unknown;
                            if (runtime == mRegistry.air_runtime_id()) continue;
                            ++mNonAirBlocks;
                        }
                    }
                }
            }

            const auto* entities = find_value(*section, "block_entities");
            if (!entities || entities->get_type() != nbt::tag_type::List) continue;
            for (const auto& raw : entities->as<nbt::tag_list>()) {
                if (raw.get_type() != nbt::tag_type::Compound) continue;
                const auto& entity = raw.as<nbt::tag_compound>();
                const auto x = int_value(find_value(entity, "x"));
                const auto y = int_value(find_value(entity, "y"));
                const auto z = int_value(find_value(entity, "z"));
                if (!x || !y || !z) continue;
                auto normalized = std::make_unique<nbt::tag_compound>();
                if (const auto* nbt_value = find_value(entity, "nbt");
                    nbt_value && nbt_value->get_type() == nbt::tag_type::Compound) {
                    normalized = std::unique_ptr<nbt::tag_compound>(
                        static_cast<nbt::tag_compound*>(normalize_nbt(*nbt_value).release()));
                }
                if (!normalized->has_key("id")) {
                    const auto name_space = string_value(find_value(entity, "namespace")).value_or("");
                    const auto base_name = string_value(find_value(entity, "base_name")).value_or("");
                    if (!base_name.empty()) {
                        normalized->put("id", nbt::value_initializer(nbt::tag_string(
                            name_space.empty() ? base_name : name_space + ":" + base_name)));
                    }
                }
                mStore.put_entity({ *x - min_x, *y - min_y, *z - min_z }, serialize_compound(*normalized));
            }
        }
        mState.source_path = path;
        mState.original_size = size;
        mState.minimum = { min_x, min_y, min_z };
        mState.sections = std::move(sections);
        mState.palette = std::move(palette);
        mState.unknown_runtime = unknown;
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse Construction failed: " + std::string(error.what()));
    }
}

Result<ChunkMap> ConstructionReader::materialize_chunks(
    std::span<const ChunkPos> positions, bool include_layer1) const
{
    try {
        if (mState.source_path.empty()) {
            throw std::runtime_error("Construction source has not been read");
        }

        ChunkMap result;
        result.reserve(positions.size());
        for (const auto position : positions) result.emplace(position, ChunkData{});
        if (result.empty()) return Result<ChunkMap>::success(std::move(result));

        std::ifstream input(mState.source_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot reopen Construction file: " +
                mState.source_path.string());
        }
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0) throw std::runtime_error("cannot determine Construction file size");
        const auto file_size = static_cast<std::uint64_t>(end);
        const auto source_size = mState.original_size;
        const auto structure_offset = mStore.offset();
        const auto air = mRegistry.air_runtime_id();

        // Preserve the Go/baseline placement order: section-index order is
        // authoritative, later non-air placements overwrite earlier ones,
        // and air entries do not clear an already materialized block.
        for (std::size_t section_index = 0;
            section_index < mState.sections.size(); ++section_index) {
            const auto& entry = mState.sections[section_index];
            if (entry.length <= 0 || entry.shape_x == 0 ||
                entry.shape_y == 0 || entry.shape_z == 0) {
                continue;
            }

            const auto local_min_x = static_cast<std::int64_t>(entry.start_x) -
                mState.minimum.x;
            const auto local_min_z = static_cast<std::int64_t>(entry.start_z) -
                mState.minimum.z;
            const auto local_max_x = local_min_x + entry.shape_x - 1;
            const auto local_max_z = local_min_z + entry.shape_z - 1;
            const auto clipped_min_x = std::max<std::int64_t>(0, local_min_x);
            const auto clipped_min_z = std::max<std::int64_t>(0, local_min_z);
            const auto clipped_max_x = std::min<std::int64_t>(
                static_cast<std::int64_t>(source_size.width) - 1, local_max_x);
            const auto clipped_max_z = std::min<std::int64_t>(
                static_cast<std::int64_t>(source_size.length) - 1, local_max_z);
            if (clipped_min_x > clipped_max_x || clipped_min_z > clipped_max_z) continue;

            const auto min_chunk_x = floor_div64(
                clipped_min_x + structure_offset.x, 16);
            const auto max_chunk_x = floor_div64(
                clipped_max_x + structure_offset.x, 16);
            const auto min_chunk_z = floor_div64(
                clipped_min_z + structure_offset.z, 16);
            const auto max_chunk_z = floor_div64(
                clipped_max_z + structure_offset.z, 16);
            const auto requested = std::ranges::any_of(positions, [&](ChunkPos position) {
                return static_cast<std::int64_t>(position.x) >= min_chunk_x &&
                    static_cast<std::int64_t>(position.x) <= max_chunk_x &&
                    static_cast<std::int64_t>(position.z) >= min_chunk_z &&
                    static_cast<std::int64_t>(position.z) <= max_chunk_z;
            });
            if (!requested) continue;

            if (entry.position < 0) {
                throw std::runtime_error("Construction section #" +
                    std::to_string(section_index) + " has negative file offset");
            }
            const auto section_start = static_cast<std::uint64_t>(entry.position);
            const auto section_length = static_cast<std::uint64_t>(entry.length);
            if (section_start > file_size || section_length > file_size - section_start) {
                throw std::runtime_error("Construction section #" +
                    std::to_string(section_index) + " has invalid range at file offset " +
                    std::to_string(entry.position));
            }
            const auto context = "Construction section #" + std::to_string(section_index);
            const auto section = read_big_endian_compound(
                input, section_start, section_length, context);
            const auto blocks = section_blocks(*section);
            const auto parsed_shape = section_shape(*section);
            if (parsed_shape[0] > 0 && parsed_shape[1] > 0 && parsed_shape[2] > 0) {
                const auto expected = checked_product3(
                    static_cast<std::uint64_t>(parsed_shape[0]),
                    static_cast<std::uint64_t>(parsed_shape[1]),
                    static_cast<std::uint64_t>(parsed_shape[2]), context);
                if (expected != blocks.size()) {
                    throw std::runtime_error(context + " block count mismatch");
                }
            }
            const auto indexed_volume = static_cast<std::size_t>(entry.shape_x) *
                entry.shape_y * entry.shape_z;
            if (blocks.size() != 0 && blocks.size() < indexed_volume) {
                throw std::runtime_error(context + " blocks are truncated at index " +
                    std::to_string(blocks.size()));
            }
            if (blocks.size() == 0) continue;

            for (std::int32_t x = 0; x < entry.shape_x; ++x) {
                const auto local_x = local_min_x + x;
                if (local_x < 0 || local_x >= source_size.width) continue;
                const auto world_x = local_x + structure_offset.x;
                if (world_x < std::numeric_limits<std::int32_t>::min() ||
                    world_x > std::numeric_limits<std::int32_t>::max()) {
                    throw std::runtime_error(context + " X coordinate exceeds int32 range");
                }
                const auto chunk_x = floor_div64(world_x, 16);
                const auto layer_x = floor_mod64(world_x, 16);
                for (std::int32_t z = 0; z < entry.shape_z; ++z) {
                    const auto local_z = local_min_z + z;
                    if (local_z < 0 || local_z >= source_size.length) continue;
                    const auto world_z = local_z + structure_offset.z;
                    if (world_z < std::numeric_limits<std::int32_t>::min() ||
                        world_z > std::numeric_limits<std::int32_t>::max()) {
                        throw std::runtime_error(context + " Z coordinate exceeds int32 range");
                    }
                    const auto chunk_z = floor_div64(world_z, 16);
                    if (chunk_x < std::numeric_limits<std::int32_t>::min() ||
                        chunk_x > std::numeric_limits<std::int32_t>::max() ||
                        chunk_z < std::numeric_limits<std::int32_t>::min() ||
                        chunk_z > std::numeric_limits<std::int32_t>::max()) {
                        throw std::runtime_error(context + " chunk coordinate exceeds int32 range");
                    }
                    const ChunkPos chunk_position{
                        static_cast<std::int32_t>(chunk_x),
                        static_cast<std::int32_t>(chunk_z)
                    };
                    const auto output = result.find(chunk_position);
                    if (output == result.end()) continue;
                    const auto layer_z = floor_mod64(world_z, 16);

                    for (std::int32_t y = 0; y < entry.shape_y; ++y) {
                        const auto local_y = static_cast<std::int64_t>(entry.start_y) -
                            mState.minimum.y + y;
                        if (local_y < 0 || local_y >= source_size.height) continue;
                        const auto index = static_cast<std::size_t>(
                            (x * entry.shape_y + y) * entry.shape_z + z);
                        const auto palette_index = blocks[index];
                        const auto runtime = palette_index >= 0 &&
                            static_cast<std::size_t>(palette_index) < mState.palette.size()
                            ? mState.palette[static_cast<std::size_t>(palette_index)]
                            : mState.unknown_runtime;
                        if (runtime == air) continue;

                        const auto world_y = local_y + structure_offset.y;
                        if (world_y < std::numeric_limits<std::int32_t>::min() ||
                            world_y > std::numeric_limits<std::int32_t>::max()) {
                            throw std::runtime_error(context + " Y coordinate exceeds int32 range");
                        }
                        const auto sub_y64 = floor_div64(world_y - 64, 16);
                        if (sub_y64 < std::numeric_limits<std::int32_t>::min() ||
                            sub_y64 > std::numeric_limits<std::int32_t>::max()) {
                            throw std::runtime_error(context + " subchunk Y exceeds int32 range");
                        }
                        const auto layer_y = floor_mod64(world_y - 64, 16);
                        auto [subchunk, inserted] = output->second.sub_chunks.try_emplace(
                            static_cast<std::int32_t>(sub_y64));
                        if (inserted) {
                            subchunk->second.layer0.fill(air);
                            if (include_layer1) subchunk->second.layer1.fill(air);
                        }
                        const auto layer_index = static_cast<std::size_t>(
                            (layer_y * 16 + layer_z) * 16 + layer_x);
                        subchunk->second.layer0[layer_index] = runtime;
                    }
                }
            }
        }
        return Result<ChunkMap>::success(std::move(result));
    } catch (const std::exception& error) {
        return Result<ChunkMap>::failure(
            "materialize Construction chunks failed: " + std::string(error.what()));
    }
}

Result<ChunkMap> ConstructionReader::get_chunks(
    std::span<const ChunkPos> positions) const
{
    return materialize_chunks(positions, true);
}

Result<ChunkMap> ConstructionReader::get_chunks_layer0(
    std::span<const ChunkPos> positions) const
{
    return materialize_chunks(positions, false);
}

Result<void> ConstructionReader::visit_chunks(
    std::span<const ChunkPos> positions, const ChunkVisitor& visitor) const
{
    if (!visitor) return Result<void>::failure("Construction chunk visitor is empty");
    auto chunks = materialize_chunks(positions, true);
    if (!chunks) return Result<void>::failure(chunks.error());
    for (const auto position : positions) {
        const auto found = chunks.value().find(position);
        if (found == chunks.value().end()) continue;
        auto visited = visitor(position, found->second);
        if (!visited) return visited;
    }
    return Result<void>::success();
}

Result<void> ConstructionReader::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> ConstructionReader::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("Construction has no Go FromMCWorld capability");
}

} // namespace water_structure
