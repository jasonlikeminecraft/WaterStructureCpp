#include "tibi.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace water_structure {
namespace {

constexpr std::size_t kHeaderSize = 15;
constexpr std::size_t kAbsoluteMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::size_t kInflateWindowBytes = 64u * 1024u;
constexpr std::size_t kMinimumMemoryBudget = 512u * 1024u;
constexpr std::size_t kReaderWorkingSetReserve = 256u * 1024u;
constexpr std::size_t kEstimatedRuntimeCacheEntryBytes = 64;
constexpr std::uint64_t kMaxTableEntries = 4'000'000;
constexpr std::uint64_t kMaxCommands = 4'000'000;
constexpr std::uint64_t kMaxMaterializedBlocksPerRequest = 16'000'000;
constexpr std::size_t kMaxRuntimeCacheEntries = 65'536;
constexpr std::size_t kMaxIndexEntries = 500'000;
constexpr std::size_t kMaxStateProperties = 128;

std::optional<std::size_t> legacy_decoded_limit()
{
    const auto* configured = std::getenv("WATER_STRUCTURE_TIBI_MAX_DECODED_MB");
    if (!configured || *configured == '\0') return std::nullopt;
    std::uint64_t megabytes = 0;
    const auto text = std::string_view(configured);
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), megabytes);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        megabytes == 0) return std::nullopt;
    const auto bytes = megabytes >
            static_cast<std::uint64_t>(kAbsoluteMaxDecodedBytes / (1024 * 1024))
        ? kAbsoluteMaxDecodedBytes
        : static_cast<std::size_t>(megabytes * 1024 * 1024);
    return std::max<std::size_t>(bytes, kHeaderSize + 64);
}

struct InflateSummary {
    std::uint64_t decoded_bytes = 0;
    std::uint64_t compressed_bytes = 0;
    std::array<std::uint8_t, kHeaderSize> header{};
    std::size_t header_bytes = 0;
};

template <typename Consumer>
InflateSummary inflate_raw_file(
    const std::filesystem::path& path,
    std::size_t max_decoded_bytes,
    Consumer&& consumer)
{
    std::error_code size_error;
    const auto compressed_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        throw std::runtime_error("cannot stat TIBI file: " + path.string());
    }
    if (compressed_size == 0) {
        throw std::runtime_error("TIBI compressed size is invalid");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open TIBI file: " + path.string());
    }

    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("cannot initialize TIBI raw DEFLATE decoder");
    }
    struct Guard {
        z_stream* stream;
        ~Guard() { inflateEnd(stream); }
    } guard{ &stream };

    std::vector<std::uint8_t> input_window(kInflateWindowBytes);
    std::vector<std::uint8_t> output_window(kInflateWindowBytes);
    InflateSummary summary;
    bool reached_eof = false;
    while (true) {
        if (stream.avail_in == 0 && !reached_eof) {
            input.read(reinterpret_cast<char*>(input_window.data()),
                static_cast<std::streamsize>(input_window.size()));
            const auto read = input.gcount();
            if (read < 0 || input.bad()) {
                throw std::runtime_error("TIBI input read failed at compressed offset " +
                    std::to_string(summary.compressed_bytes));
            }
            if (read == 0) {
                reached_eof = true;
            } else {
                stream.next_in = input_window.data();
                stream.avail_in = static_cast<uInt>(read);
                reached_eof = static_cast<std::size_t>(read) < input_window.size();
            }
        }

        stream.next_out = output_window.data();
        stream.avail_out = static_cast<uInt>(output_window.size());
        const auto input_before = stream.avail_in;
        const auto status = inflate(&stream, Z_NO_FLUSH);
        summary.compressed_bytes += input_before - stream.avail_in;
        const auto produced = output_window.size() - stream.avail_out;
        if (produced > max_decoded_bytes || summary.decoded_bytes >
                static_cast<std::uint64_t>(max_decoded_bytes - produced)) {
            throw std::runtime_error(
                "TIBI decoded payload exceeds " +
                std::to_string(max_decoded_bytes / (1024 * 1024)) + " MiB");
        }

        if (produced != 0) {
            const auto header_remaining = kHeaderSize - summary.header_bytes;
            const auto header_copy = std::min(produced, header_remaining);
            if (header_copy != 0) {
                std::copy_n(output_window.begin(), header_copy,
                    summary.header.begin() + static_cast<std::ptrdiff_t>(summary.header_bytes));
                summary.header_bytes += header_copy;
            }
            consumer(std::span<std::uint8_t>(output_window.data(), produced),
                summary.decoded_bytes);
            summary.decoded_bytes += produced;
        }

        if (status == Z_STREAM_END) break;
        if (status != Z_OK) {
            if (status == Z_BUF_ERROR && reached_eof && stream.avail_in == 0) {
                throw std::runtime_error("TIBI raw DEFLATE is truncated at compressed offset " +
                    std::to_string(summary.compressed_bytes));
            }
            throw std::runtime_error("TIBI raw DEFLATE failed at compressed offset " +
                std::to_string(summary.compressed_bytes));
        }
        if (produced == 0 && input_before == stream.avail_in) {
            if (reached_eof && stream.avail_in == 0) {
                throw std::runtime_error("TIBI raw DEFLATE is truncated at compressed offset " +
                    std::to_string(summary.compressed_bytes));
            }
            throw std::runtime_error("TIBI raw DEFLATE made no progress at compressed offset " +
                std::to_string(summary.compressed_bytes));
        }
        if (produced == 0 && stream.avail_in == 0 && reached_eof) {
            throw std::runtime_error("TIBI raw DEFLATE is truncated at compressed offset " +
                std::to_string(summary.compressed_bytes));
        }
    }
    return summary;
}

std::filesystem::path make_spool_directory(const std::filesystem::path& requested_directory)
{
    std::error_code error;
    auto directory = requested_directory;
    if (directory.empty()) {
        directory = std::filesystem::temp_directory_path(error);
        if (error) {
            throw std::runtime_error("TIBI reader cannot resolve the temporary directory");
        }
    } else {
        std::filesystem::create_directories(directory, error);
        if (error) {
            throw std::runtime_error("TIBI reader cannot create temporary directory: " +
                directory.string());
        }
    }
    if (!std::filesystem::is_directory(directory, error) || error) {
        throw std::runtime_error("TIBI temporary path is not a directory: " +
            directory.string());
    }

    static std::atomic<std::uint64_t> sequence{ 0 };
    std::uint64_t entropy = 0;
    try {
        std::random_device device;
        entropy = (static_cast<std::uint64_t>(device()) << 32u) ^ device();
    } catch (...) {
        entropy = static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }
    const auto timestamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = directory /
            ("water_structure_tibi_" + std::to_string(timestamp) + "_" +
             std::to_string(entropy) + "_" + std::to_string(id));
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error) {
            throw std::runtime_error("TIBI reader cannot create temporary spool directory");
        }
        error.clear();
    }
    throw std::runtime_error("TIBI reader cannot allocate a unique temporary spool path");
}

struct TemporarySpool {
    std::filesystem::path directory;
    std::filesystem::path path;

    ~TemporarySpool()
    {
        std::error_code error;
        if (!path.empty()) std::filesystem::remove(path, error);
        error.clear();
        if (!directory.empty()) std::filesystem::remove(directory, error);
    }
};

struct PayloadSlice {
    std::uint32_t absolute_offset = 0;
    std::uint32_t length = 0;
};

class PayloadReader {
public:
    PayloadReader(const std::filesystem::path& path,
                  std::uint64_t base_offset,
                  std::size_t payload_size)
        : mInput(path, std::ios::binary),
          mBaseOffset(base_offset),
          mPayloadSize(payload_size),
          mBuffer(kInflateWindowBytes)
    {
        if (!mInput) {
            throw std::runtime_error("cannot open TIBI temporary spool for parsing");
        }
        mInput.seekg(static_cast<std::streamoff>(mBaseOffset));
        if (!mInput) {
            throw std::runtime_error("cannot seek TIBI temporary spool to payload");
        }
    }

    std::uint64_t varint(std::string_view context)
    {
        std::uint64_t result = 0;
        unsigned shift = 0;
        const auto start = mOffset;
        while (true) {
            if (mOffset >= mPayloadSize) {
                throw std::runtime_error(std::string(context) +
                    " has truncated varint at payload offset " + std::to_string(start));
            }
            const auto byte = read_byte();
            if (shift == 63 && (byte & 0xfeu) != 0) {
                throw std::runtime_error(std::string(context) +
                    " varint overflows at payload offset " + std::to_string(start));
            }
            result |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) return result;
            shift += 7;
            if (shift > 63) {
                throw std::runtime_error(std::string(context) +
                    " varint overflows at payload offset " + std::to_string(start));
            }
        }
    }

    PayloadSlice string(std::string_view context)
    {
        const auto length = varint(std::string(context) + " length");
        if (length > mPayloadSize - mOffset) {
            throw std::runtime_error(std::string(context) +
                " is truncated at payload offset " + std::to_string(mOffset));
        }
        const PayloadSlice result{
            static_cast<std::uint32_t>(mBaseOffset + mOffset),
            static_cast<std::uint32_t>(length)
        };
        skip(static_cast<std::size_t>(length));
        return result;
    }

private:
    std::uint8_t read_byte()
    {
        if (mBufferPosition == mBufferSize) refill();
        const auto value = mBuffer[mBufferPosition++];
        ++mOffset;
        return value;
    }

    void refill()
    {
        const auto remaining = mPayloadSize - mOffset;
        const auto requested = std::min(remaining, mBuffer.size());
        mInput.read(reinterpret_cast<char*>(mBuffer.data()),
            static_cast<std::streamsize>(requested));
        if (mInput.gcount() != static_cast<std::streamsize>(requested)) {
            throw std::runtime_error("TIBI temporary spool is truncated at payload offset " +
                std::to_string(mOffset));
        }
        mBufferPosition = 0;
        mBufferSize = requested;
    }

    void skip(std::size_t length)
    {
        const auto buffered = mBufferSize - mBufferPosition;
        if (length <= buffered) {
            mBufferPosition += length;
            mOffset += length;
            return;
        }
        mOffset += length;
        mInput.clear();
        mInput.seekg(static_cast<std::streamoff>(mBaseOffset + mOffset));
        if (!mInput) {
            throw std::runtime_error("cannot seek TIBI temporary spool at payload offset " +
                std::to_string(mOffset));
        }
        mBufferPosition = 0;
        mBufferSize = 0;
    }

    std::ifstream mInput;
    std::uint64_t mBaseOffset = 0;
    std::size_t mPayloadSize = 0;
    std::size_t mOffset = 0;
    std::vector<std::uint8_t> mBuffer;
    std::size_t mBufferPosition = 0;
    std::size_t mBufferSize = 0;
};

class TableReader {
public:
    explicit TableReader(const std::filesystem::path& path)
        : mInput(path, std::ios::binary)
    {
        if (!mInput) {
            throw std::runtime_error("cannot open TIBI temporary spool table reader");
        }
    }

    std::pair<std::string_view, std::string_view> read_pair(
        const PayloadSlice& block,
        const PayloadSlice& property,
        std::string& scratch)
    {
        if (static_cast<std::size_t>(property.length) >
                std::numeric_limits<std::size_t>::max() - block.length) {
            throw std::runtime_error("TIBI block/property table pair size overflows");
        }
        const auto total = static_cast<std::size_t>(block.length) + property.length;
        if (total == 0) return { std::string_view{}, std::string_view{} };
        scratch.resize(total);
        read_at(block, scratch.data(), "block");
        read_at(property, scratch.data() + block.length, "property");
        return {
            std::string_view(scratch.data(), block.length),
            std::string_view(scratch.data() + block.length, property.length)
        };
    }

private:
    void read_at(const PayloadSlice& slice, char* destination, std::string_view name)
    {
        if (slice.length == 0) return;
        mInput.clear();
        mInput.seekg(static_cast<std::streamoff>(slice.absolute_offset));
        if (!mInput) {
            throw std::runtime_error("cannot seek TIBI " + std::string(name) +
                " table entry in temporary spool");
        }
        mInput.read(destination, static_cast<std::streamsize>(slice.length));
        if (mInput.gcount() != static_cast<std::streamsize>(slice.length)) {
            throw std::runtime_error("TIBI " + std::string(name) +
                " table entry is truncated in temporary spool");
        }
    }

    std::ifstream mInput;
};

std::array<std::uint8_t, 16> md5(std::span<const std::uint8_t> input)
{
    static constexpr std::array<std::uint32_t, 64> shifts{
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    static constexpr std::array<std::uint32_t, 64> constants{
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };

    const auto padded_size = (input.size() + 9 + 63) & ~std::size_t{63};
    std::vector<std::uint8_t> padded(padded_size, 0);
    std::copy(input.begin(), input.end(), padded.begin());
    padded[input.size()] = 0x80;
    const auto bit_length = static_cast<std::uint64_t>(input.size()) * 8;
    for (int index = 0; index < 8; ++index) {
        padded[padded_size - 8 + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(bit_length >> (index * 8));
    }

    std::uint32_t h0 = 0x67452301;
    std::uint32_t h1 = 0xefcdab89;
    std::uint32_t h2 = 0x98badcfe;
    std::uint32_t h3 = 0x10325476;
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 16> words{};
        for (std::size_t word = 0; word < words.size(); ++word) {
            const auto base = offset + word * 4;
            words[word] = static_cast<std::uint32_t>(padded[base]) |
                (static_cast<std::uint32_t>(padded[base + 1]) << 8) |
                (static_cast<std::uint32_t>(padded[base + 2]) << 16) |
                (static_cast<std::uint32_t>(padded[base + 3]) << 24);
        }
        auto a = h0;
        auto b = h1;
        auto c = h2;
        auto d = h3;
        for (std::uint32_t index = 0; index < 64; ++index) {
            std::uint32_t function = 0;
            std::uint32_t word = 0;
            if (index < 16) {
                function = (b & c) | (~b & d);
                word = index;
            } else if (index < 32) {
                function = (d & b) | (~d & c);
                word = (5 * index + 1) % 16;
            } else if (index < 48) {
                function = b ^ c ^ d;
                word = (3 * index + 5) % 16;
            } else {
                function = c ^ (b | ~d);
                word = (7 * index) % 16;
            }
            const auto previous_d = d;
            d = c;
            c = b;
            b += std::rotl(a + function + constants[index] + words[word],
                static_cast<int>(shifts[index]));
            a = previous_d;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
    }

    std::array<std::uint8_t, 16> result{};
    const std::array hash{ h0, h1, h2, h3 };
    for (std::size_t word = 0; word < hash.size(); ++word) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            result[word * 4 + byte] =
                static_cast<std::uint8_t>(hash[word] >> (byte * 8));
        }
    }
    return result;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::vector<BlockStateProperty>> parse_states(std::string_view text)
{
    if (text.empty()) return std::vector<BlockStateProperty>{};
    if (text.front() != '[' || text.back() != ']') return std::nullopt;
    text.remove_prefix(1);
    text.remove_suffix(1);
    std::vector<BlockStateProperty> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        auto comma = std::string_view::npos;
        bool quoted = false;
        bool escaped = false;
        for (auto index = begin; index < text.size(); ++index) {
            const auto character = text[index];
            if (quoted && escaped) {
                escaped = false;
            } else if (quoted && character == '\\') {
                escaped = true;
            } else if (character == '"') {
                quoted = !quoted;
            } else if (character == ',' && !quoted) {
                comma = index;
                break;
            }
        }
        if (quoted || escaped) return std::nullopt;
        const auto part = text.substr(begin,
            comma == std::string_view::npos ? text.size() - begin : comma - begin);
        if (!part.empty()) {
            if (result.size() >= kMaxStateProperties) return std::nullopt;
            const auto equal = part.find('=');
            if (equal == std::string_view::npos || equal == 0) return std::nullopt;
            BlockStateProperty property;
            property.name = trim(std::string(part.substr(0, equal)));
            property.value = trim(std::string(part.substr(equal + 1)));
            if (property.value.size() >= 2 && property.value.front() == '"' &&
                property.value.back() == '"') {
                property.value = property.value.substr(1, property.value.size() - 2);
            }
            if (property.value == "true" || property.value == "false") {
                property.type = BlockStateValueType::Byte;
                property.value = property.value == "true" ? "1" : "0";
            } else {
                std::int32_t number = 0;
                const auto parsed = std::from_chars(property.value.data(),
                    property.value.data() + property.value.size(), number);
                property.type = parsed.ec == std::errc{} &&
                    parsed.ptr == property.value.data() + property.value.size()
                    ? BlockStateValueType::Int : BlockStateValueType::String;
            }
            result.push_back(std::move(property));
        }
        if (comma == std::string_view::npos) break;
        begin = comma + 1;
    }
    return result;
}

std::uint32_t runtime_id(RuntimeRegistry& registry,
                         std::string_view block_view,
                         std::string_view property_view)
{
    auto block = trim(std::string(block_view));
    if (block.empty()) return registry.air_runtime_id();
    auto name = block;
    std::string state_text;
    if (const auto open = block.find('['); open != std::string::npos) {
        const auto close = block.find_last_of(']');
        if (close > open) {
            name = trim(block.substr(0, open));
            state_text = block.substr(open, close - open + 1);
        }
    }
    std::istringstream property_stream{ std::string(property_view) };
    std::string first;
    if (property_stream >> first) {
        int legacy = 0;
        const auto parsed = std::from_chars(first.data(), first.data() + first.size(), legacy);
        if (parsed.ec == std::errc{} && parsed.ptr == first.data() + first.size()) {
            if (const auto value = registry.legacy_runtime_id(name, static_cast<std::uint16_t>(legacy))) {
                return *value;
            }
        }
    }
    if (!state_text.empty()) {
        if (const auto states = parse_states(state_text)) {
            if (const auto value = registry.find(name, *states)) return *value;
            if (const auto value = registry.compatible_java_runtime_id(name + state_text)) return *value;
        }
    }
    if (const auto value = registry.find(name)) return *value;
    if (const auto value = registry.find("minecraft:unknown")) return *value;
    return registry.air_runtime_id();
}

std::int32_t coordinate(std::uint64_t value, std::size_t command_index, std::string_view name)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("TIBI command #" + std::to_string(command_index) + " " +
            std::string(name) + " coordinate exceeds int32");
    }
    return static_cast<std::int32_t>(value);
}

void set_block(ChunkMap& chunks, BlockPos position, std::uint32_t runtime, std::uint32_t air)
{
    const auto chunk_position = block_to_chunk(position);
    const auto chunk = chunks.find(chunk_position);
    if (chunk == chunks.end()) return;
    const auto sub_y = floor_div(position.y - 64, 16);
    auto [sub, inserted] = chunk->second.sub_chunks.try_emplace(sub_y);
    if (inserted) {
        sub->second.layer0.fill(air);
        sub->second.layer1.fill(air);
    }
    const auto local_x = position.x - chunk_position.x * 16;
    const auto local_y = position.y - (sub_y * 16 + 64);
    const auto local_z = position.z - chunk_position.z * 16;
    sub->second.layer0[static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x)] = runtime;
}

} // namespace

void TibiReader::set_streaming_options(
    std::size_t soft_memory_budget_bytes,
    bool allow_temporary_spool,
    std::filesystem::path temporary_directory,
    std::size_t temporary_file_limit_bytes)
{
    mSoftMemoryBudgetBytes = soft_memory_budget_bytes == 0
        ? 450u * 1024u * 1024u
        : soft_memory_budget_bytes;
    mAllowTemporarySpool = allow_temporary_spool;
    mTemporaryDirectory = std::move(temporary_directory);
    mTemporaryFileLimitBytes = temporary_file_limit_bytes;
    mStreamingOptionsConfigured = true;
    mCommandIndex.clear();
    mBroadCommands.clear();
    mCommandIndexReady = false;
}

void TibiReader::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mCommandIndex.clear();
    mBroadCommands.clear();
    mCommandIndexReady = false;
    const auto expanded = [](std::int32_t base, std::int32_t delta) noexcept {
        const auto magnitude = delta < 0 ? -static_cast<std::int64_t>(delta)
                                         : static_cast<std::int64_t>(delta);
        const auto value = static_cast<std::int64_t>(base) + magnitude;
        return static_cast<std::int32_t>(std::min<std::int64_t>(
            value, std::numeric_limits<std::int32_t>::max()));
    };
    mSize = {
        expanded(mOriginalSize.width, offset.x),
        expanded(mOriginalSize.height, offset.y),
        expanded(mOriginalSize.length, offset.z)
    };
}

Result<void> TibiReader::read(const std::filesystem::path& path)
{
    std::vector<Command>{}.swap(mCommands);
    mOrigin = {};
    mOffset = {};
    mOriginalSize = {};
    mSize = {};
    mNonAirBlocks = 0;
    decltype(mCommandIndex){}.swap(mCommandIndex);
    std::vector<std::uint32_t>{}.swap(mBroadCommands);
    mCommandIndexReady = false;
    try {
        if (!mAllowTemporarySpool) {
            throw std::runtime_error(
                "capability error: TIBI reader requires a seekable temporary decoded spool "
                "because its XOR key depends on decoded length; allow_temporary_spool=false");
        }
        if (mSoftMemoryBudgetBytes < kMinimumMemoryBudget) {
            throw std::runtime_error(
                "TIBI reader soft_memory_budget must be at least 512 KiB");
        }

        auto decoded_limit = mTemporaryFileLimitBytes == 0
            ? kAbsoluteMaxDecodedBytes
            : mTemporaryFileLimitBytes;
        if (!mStreamingOptionsConfigured) {
            if (const auto legacy = legacy_decoded_limit()) decoded_limit = *legacy;
        }
        decoded_limit = std::min(decoded_limit, kAbsoluteMaxDecodedBytes);
        if (decoded_limit < kHeaderSize + 1) {
            throw std::runtime_error(
                "TIBI temporary_file_limit is too small for the decoded header and payload");
        }

        const auto first_pass = inflate_raw_file(path, decoded_limit,
            [](std::span<std::uint8_t>, std::uint64_t) {});
        if (first_pass.decoded_bytes < kHeaderSize ||
            first_pass.header_bytes != kHeaderSize) {
            throw std::runtime_error("TIBI decoded payload is shorter than 15-byte header");
        }
        const auto payload_size_u64 = first_pass.decoded_bytes - kHeaderSize;
        if (payload_size_u64 > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("TIBI decoded payload exceeds addressable size");
        }
        const auto payload_size = static_cast<std::size_t>(payload_size_u64);

        std::vector<std::uint8_t> key_material(
            first_pass.header.begin(), first_pass.header.end());
        const auto suffix = std::string("TIBI_2025/5/19-Start") +
            std::to_string(payload_size_u64);
        key_material.insert(key_material.end(), suffix.begin(), suffix.end());
        const auto key = md5(key_material);

        TemporarySpool temporary;
        temporary.directory = make_spool_directory(mTemporaryDirectory);
        temporary.path = temporary.directory / "decoded.bin";
        std::ofstream spool(temporary.path, std::ios::binary | std::ios::trunc);
        if (!spool) {
            throw std::runtime_error("TIBI reader cannot create temporary decoded spool: " +
                temporary.path.string());
        }
        const auto second_pass = inflate_raw_file(path, decoded_limit,
            [&](std::span<std::uint8_t> bytes, std::uint64_t decoded_offset) {
                if (decoded_offset > first_pass.decoded_bytes || bytes.size() >
                        first_pass.decoded_bytes - decoded_offset) {
                    throw std::runtime_error(
                        "TIBI input changed between raw DEFLATE passes (decoded size grew)");
                }
                auto begin = std::size_t{ 0 };
                if (decoded_offset < kHeaderSize) {
                    begin = static_cast<std::size_t>(std::min<std::uint64_t>(
                        bytes.size(), kHeaderSize - decoded_offset));
                }
                for (auto index = begin; index < bytes.size(); ++index) {
                    const auto absolute = decoded_offset + index;
                    bytes[index] ^= key[static_cast<std::size_t>(
                        (absolute - kHeaderSize) % key.size())];
                }
                spool.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                if (!spool) {
                    throw std::runtime_error("TIBI temporary decoded spool write failed");
                }
            });
        if (second_pass.decoded_bytes != first_pass.decoded_bytes ||
            second_pass.header != first_pass.header) {
            throw std::runtime_error(
                "TIBI input changed between raw DEFLATE passes");
        }
        spool.flush();
        if (!spool) {
            throw std::runtime_error("TIBI temporary decoded spool flush failed");
        }
        spool.close();
        if (!spool) {
            throw std::runtime_error("TIBI temporary decoded spool close failed");
        }

        auto accounted_memory = kReaderWorkingSetReserve;
        const auto account_vector = [&](std::uint64_t count, std::size_t element_size,
                                        std::string_view context) {
            if (count > std::numeric_limits<std::size_t>::max() / element_size) {
                throw std::runtime_error(std::string(context) +
                    " allocation size overflows");
            }
            const auto bytes = static_cast<std::size_t>(count) * element_size;
            if (bytes > mSoftMemoryBudgetBytes -
                    std::min(accounted_memory, mSoftMemoryBudgetBytes)) {
                throw std::runtime_error(std::string(context) +
                    " exceeds TIBI soft_memory_budget");
            }
            accounted_memory += bytes;
        };

        PayloadReader reader(temporary.path, kHeaderSize, payload_size);
        const auto block_count = reader.varint("TIBI block table count");
        if (block_count > kMaxTableEntries ||
            block_count > first_pass.decoded_bytes) {
            throw std::runtime_error("TIBI block table count is invalid or exceeds limit");
        }
        account_vector(block_count, sizeof(PayloadSlice), "TIBI block table");
        std::vector<PayloadSlice> blocks;
        blocks.reserve(static_cast<std::size_t>(block_count));
        for (std::uint64_t index = 0; index < block_count; ++index) {
            reader.varint("TIBI block table line #" + std::to_string(index));
            blocks.push_back(reader.string("TIBI block table entry #" + std::to_string(index)));
        }
        const auto property_count = reader.varint("TIBI property table count");
        if (property_count > kMaxTableEntries ||
            property_count > first_pass.decoded_bytes) {
            throw std::runtime_error("TIBI property table count is invalid or exceeds limit");
        }
        account_vector(property_count, sizeof(PayloadSlice), "TIBI property table");
        std::vector<PayloadSlice> properties;
        properties.reserve(static_cast<std::size_t>(property_count));
        for (std::uint64_t index = 0; index < property_count; ++index) {
            reader.varint("TIBI property table line #" + std::to_string(index));
            properties.push_back(reader.string("TIBI property table entry #" + std::to_string(index)));
        }
        const auto command_count = reader.varint("TIBI command count");
        if (command_count > kMaxCommands ||
            command_count > first_pass.decoded_bytes) {
            throw std::runtime_error("TIBI command count is invalid or exceeds limit");
        }
        account_vector(command_count, sizeof(Command), "TIBI command table");
        std::vector<Command> commands;
        commands.reserve(static_cast<std::size_t>(command_count));
        auto min_x = std::numeric_limits<std::int32_t>::max();
        auto min_y = min_x;
        auto min_z = min_x;
        auto max_x = std::numeric_limits<std::int32_t>::min();
        auto max_y = max_x;
        auto max_z = max_x;
        std::uint64_t non_air = 0;
        const auto uncommitted_memory = mSoftMemoryBudgetBytes - accounted_memory;
        const auto runtime_cache_limit = std::min(
            kMaxRuntimeCacheEntries,
            uncommitted_memory / (2 * kEstimatedRuntimeCacheEntryBytes));
        accounted_memory += runtime_cache_limit * kEstimatedRuntimeCacheEntryBytes;
        const auto maximum_table_pair_bytes =
            (mSoftMemoryBudgetBytes - accounted_memory) / 4;
        std::unordered_map<std::uint64_t, std::uint32_t> runtime_cache;
        if (runtime_cache_limit != 0) {
            runtime_cache.reserve(std::min<std::size_t>(1024, runtime_cache_limit));
        }
        TableReader table_reader(temporary.path);
        std::string table_scratch;
        for (std::size_t index = 0; index < command_count; ++index) {
            Command command;
            command.type = reader.varint("TIBI command #" + std::to_string(index) + " type");
            const auto block_index = reader.varint("TIBI command #" + std::to_string(index) + " block index");
            command.first.x = coordinate(reader.varint("TIBI command x"), index, "x");
            command.first.y = coordinate(reader.varint("TIBI command y"), index, "y");
            command.first.z = coordinate(reader.varint("TIBI command z"), index, "z");
            command.second = command.first;
            std::uint64_t property_index = 0;
            if (command.type == 1) {
                command.second.x = coordinate(reader.varint("TIBI fill dx"), index, "dx");
                command.second.y = coordinate(reader.varint("TIBI fill dy"), index, "dy");
                command.second.z = coordinate(reader.varint("TIBI fill dz"), index, "dz");
                property_index = reader.varint("TIBI command #" + std::to_string(index) + " property index");
            } else {
                property_index = reader.varint("TIBI command #" + std::to_string(index) + " property index");
            }
            if (block_index >= blocks.size() || property_index >= properties.size()) {
                throw std::runtime_error("TIBI command #" + std::to_string(index) +
                    " references an out-of-range table index");
            }
            const auto runtime_key = block_index << 32u | property_index;
            if (const auto cached = runtime_cache.find(runtime_key);
                cached != runtime_cache.end()) {
                command.runtime_id = cached->second;
            } else {
                const auto& block = blocks[static_cast<std::size_t>(block_index)];
                const auto& property = properties[static_cast<std::size_t>(property_index)];
                if (static_cast<std::size_t>(property.length) >
                        std::numeric_limits<std::size_t>::max() - block.length ||
                    static_cast<std::size_t>(block.length) + property.length >
                        maximum_table_pair_bytes) {
                    throw std::runtime_error("TIBI command #" + std::to_string(index) +
                        " block/property text exceeds soft_memory_budget");
                }
                const auto [block_text, property_text] =
                    table_reader.read_pair(block, property, table_scratch);
                command.runtime_id = runtime_id(
                    mRegistry, block_text, property_text);
                if (runtime_cache.size() < runtime_cache_limit) {
                    runtime_cache.emplace(runtime_key, command.runtime_id);
                }
            }
            if (command.type == 0 || command.type == 1) {
                min_x = std::min({ min_x, command.first.x, command.second.x });
                min_y = std::min({ min_y, command.first.y, command.second.y });
                min_z = std::min({ min_z, command.first.z, command.second.z });
                max_x = std::max({ max_x, command.first.x, command.second.x });
                max_y = std::max({ max_y, command.first.y, command.second.y });
                max_z = std::max({ max_z, command.first.z, command.second.z });
                if (command.runtime_id != mRegistry.air_runtime_id()) {
                    std::uint64_t volume = 1;
                    if (command.type == 1) {
                        const auto go_span = [](std::int32_t first, std::int32_t second) {
                            const auto value = static_cast<std::int64_t>(second) - first + 1;
                            return static_cast<std::uint64_t>(value < 0 ? -value : value);
                        };
                        const auto x = go_span(command.first.x, command.second.x);
                        const auto y = go_span(command.first.y, command.second.y);
                        const auto z = go_span(command.first.z, command.second.z);
                        if (x != 0 && y > std::numeric_limits<std::uint64_t>::max() / x) {
                            volume = std::numeric_limits<std::uint64_t>::max();
                        } else {
                            volume = x * y;
                            if (z != 0 && volume > std::numeric_limits<std::uint64_t>::max() / z) {
                                volume = std::numeric_limits<std::uint64_t>::max();
                            } else {
                                volume *= z;
                            }
                        }
                    }
                    non_air = volume > std::numeric_limits<std::uint64_t>::max() - non_air
                        ? std::numeric_limits<std::uint64_t>::max() : non_air + volume;
                }
            }
            commands.push_back(command);
        }
        if (min_x == std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("TIBI has no setblock or fill commands");
        }
        const auto dimension = [](std::int32_t minimum, std::int32_t maximum) {
            const auto value = static_cast<std::int64_t>(maximum) - minimum + 1;
            if (value <= 0 || value > std::numeric_limits<std::int32_t>::max()) {
                throw std::runtime_error("TIBI dimensions exceed int32");
            }
            return static_cast<std::int32_t>(value);
        };
        mOrigin = { min_x, min_y, min_z };
        mOriginalSize = { dimension(min_x, max_x), dimension(min_y, max_y), dimension(min_z, max_z) };
        mSize = mOriginalSize;
        mNonAirBlocks = non_air > std::numeric_limits<std::size_t>::max()
            ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(non_air);
        mCommands = std::move(commands);
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse TIBI failed: " + std::string(error.what()));
    }
}

Result<ChunkMap> TibiReader::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap chunks;
    for (const auto position : positions) chunks.emplace(position, ChunkData{});
    const auto air = mRegistry.air_runtime_id();
    const auto local = [&](BlockPos value) {
        return BlockPos{
            value.x - mOrigin.x + mOffset.x,
            value.y - mOrigin.y + mOffset.y,
            value.z - mOrigin.z + mOffset.z
        };
    };

    if (!mCommandIndexReady) {
        const std::scoped_lock index_lock(mCommandIndexMutex);
        if (!mCommandIndexReady) {
        mCommandIndex.clear();
        mBroadCommands.clear();
        std::size_t index_entries = 0;
        // An unordered_map entry plus its vector bookkeeping is substantially
        // larger than the 32-bit command index itself.  Keep the index within
        // a small fraction of the configured reader budget; oversized fills
        // remain in the bounded broad-command list and are clipped to the
        // requested chunk at materialization time.
        const auto budget_index_entries = mSoftMemoryBudgetBytes / 64u;
        const auto max_index_entries = std::min<std::size_t>(
            kMaxIndexEntries, std::max<std::size_t>(1024, budget_index_entries));
        for (std::size_t index = 0; index < mCommands.size(); ++index) {
            const auto& command = mCommands[index];
            if (command.type != 0 && command.type != 1) continue;
            const auto first = local(command.first);
            const auto second = local(command.second);
            const auto chunk_x1 = floor_div64(std::min<std::int64_t>(first.x, second.x), 16);
            const auto chunk_x2 = floor_div64(std::max<std::int64_t>(first.x, second.x), 16);
            const auto chunk_z1 = floor_div64(std::min<std::int64_t>(first.z, second.z), 16);
            const auto chunk_z2 = floor_div64(std::max<std::int64_t>(first.z, second.z), 16);
            const auto span_x = chunk_x2 - chunk_x1 + 1;
            const auto span_z = chunk_z2 - chunk_z1 + 1;
            const auto covered = span_x <= 0 || span_z <= 0
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(span_x) >
                    std::numeric_limits<std::uint64_t>::max() /
                        static_cast<std::uint64_t>(span_z)
                    ? std::numeric_limits<std::uint64_t>::max()
                    : static_cast<std::uint64_t>(span_x) * static_cast<std::uint64_t>(span_z);
            if (covered > 4096 || covered > max_index_entries -
                    std::min(index_entries, max_index_entries)) {
                mBroadCommands.push_back(static_cast<std::uint32_t>(index));
                continue;
            }
            if (chunk_x1 < std::numeric_limits<std::int32_t>::min() ||
                chunk_x2 > std::numeric_limits<std::int32_t>::max() ||
                chunk_z1 < std::numeric_limits<std::int32_t>::min() ||
                chunk_z2 > std::numeric_limits<std::int32_t>::max()) {
                mBroadCommands.push_back(static_cast<std::uint32_t>(index));
                continue;
            }
            index_entries += static_cast<std::size_t>(covered);
            for (auto z = chunk_z1; z <= chunk_z2; ++z) {
                for (auto x = chunk_x1; x <= chunk_x2; ++x) {
                    mCommandIndex[{
                        static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)
                    }].push_back(
                        static_cast<std::uint32_t>(index));
                    if (x == chunk_x2) break;
                }
                if (z == chunk_z2) break;
            }
        }
            mCommandIndexReady = true;
        }
    }

    std::uint64_t materialized = 0;
    for (const auto& [chunk_position, _] : chunks) {
        const auto process = [&](std::uint32_t command_index) -> Result<void> {
            const auto& command = mCommands[command_index];
            if (command.runtime_id == air) return Result<void>::success();
            if (command.type == 0) {
                set_block(chunks, local(command.first), command.runtime_id, air);
                if (++materialized > kMaxMaterializedBlocksPerRequest) {
                    return Result<void>::failure(
                        "TIBI 请求展开量超过限制");
                }
                return Result<void>::success();
            }
            if (command.type != 1) return Result<void>::success();

            const auto first = local(command.first);
            const auto second = local(command.second);
            const auto fill_min_x = std::min<std::int64_t>(first.x, second.x);
            const auto fill_max_x = std::max<std::int64_t>(first.x, second.x);
            const auto fill_min_y = std::min<std::int64_t>(first.y, second.y);
            const auto fill_max_y = std::max<std::int64_t>(first.y, second.y);
            const auto fill_min_z = std::min<std::int64_t>(first.z, second.z);
            const auto fill_max_z = std::max<std::int64_t>(first.z, second.z);
            const auto chunk_min_x = static_cast<std::int64_t>(chunk_position.x) * 16;
            const auto chunk_min_z = static_cast<std::int64_t>(chunk_position.z) * 16;
            const auto min_x = std::max(fill_min_x, chunk_min_x);
            const auto max_x = std::min(fill_max_x, chunk_min_x + 15);
            const auto min_z = std::max(fill_min_z, chunk_min_z);
            const auto max_z = std::min(fill_max_z, chunk_min_z + 15);
            if (min_x > max_x || min_z > max_z) {
                return Result<void>::success();
            }
            const auto width = static_cast<std::uint64_t>(max_x - min_x + 1);
            const auto depth = static_cast<std::uint64_t>(max_z - min_z + 1);
            const auto height = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(fill_max_y) - fill_min_y + 1);
            const auto volume = width * depth * height;
            if (volume > kMaxMaterializedBlocksPerRequest - materialized) {
                return Result<void>::failure("TIBI fill 请求展开量超过限制");
            }
            materialized += volume;
            for (std::int64_t x = min_x; x <= max_x; ++x) {
                for (std::int64_t y = fill_min_y; y <= fill_max_y; ++y) {
                    for (std::int64_t z = min_z; z <= max_z; ++z) {
                        set_block(chunks, {
                            static_cast<std::int32_t>(x),
                            static_cast<std::int32_t>(y),
                            static_cast<std::int32_t>(z)
                        }, command.runtime_id, air);
                    }
                }
            }
            return Result<void>::success();
        };

        const auto indexed = mCommandIndex.find(chunk_position);
        const auto* local_commands = indexed == mCommandIndex.end()
            ? nullptr : &indexed->second;
        std::size_t local_index = 0;
        std::size_t broad_index = 0;
        while ((local_commands && local_index < local_commands->size()) ||
               broad_index < mBroadCommands.size()) {
            std::uint32_t command_index = 0;
            if (broad_index == mBroadCommands.size() ||
                (local_commands && local_index < local_commands->size() &&
                 (*local_commands)[local_index] < mBroadCommands[broad_index])) {
                command_index = (*local_commands)[local_index++];
            } else {
                command_index = mBroadCommands[broad_index++];
            }
            if (const auto result = process(command_index); !result) {
                return Result<ChunkMap>::failure(result.error());
            }
        }
    }
    return Result<ChunkMap>::success(std::move(chunks));
}

Result<NbtChunkMap> TibiReader::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto position : positions) result.emplace(position, std::vector<BlockEntity>{});
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> TibiReader::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> TibiReader::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("TIBI has no Go FromMCWorld capability");
}

} // namespace water_structure
