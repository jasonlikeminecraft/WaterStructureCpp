#include "tibi.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace water_structure {
namespace {

constexpr std::size_t kHeaderSize = 15;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open TIBI file: " + path.string());
    const auto end = input.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > kMaxDecodedBytes) {
        throw std::runtime_error("TIBI compressed size is invalid");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("TIBI input is truncated at compressed offset " +
        std::to_string(static_cast<std::size_t>(input.gcount())));
    return bytes;
}

std::vector<std::uint8_t> inflate_raw(std::span<const std::uint8_t> input)
{
    if (input.size() > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("TIBI compressed payload exceeds zlib input limit");
    }
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("cannot initialize TIBI raw DEFLATE decoder");
    }
    struct Guard {
        z_stream* stream;
        ~Guard() { inflateEnd(stream); }
    } guard{ &stream };
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    std::vector<std::uint8_t> output;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::runtime_error("TIBI raw DEFLATE failed at compressed offset " +
                std::to_string(stream.total_in));
        }
        const auto produced = buffer.size() - stream.avail_out;
        if (output.size() > kMaxDecodedBytes - produced) {
            throw std::runtime_error("TIBI decoded payload exceeds 2 GiB");
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));
        if (produced == 0 && stream.avail_in == 0 && status != Z_STREAM_END) {
            throw std::runtime_error("TIBI raw DEFLATE is truncated at compressed offset " +
                std::to_string(stream.total_in));
        }
    }
    return output;
}

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

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::uint8_t> bytes) : mBytes(bytes) {}

    std::uint64_t varint(std::string_view context)
    {
        std::uint64_t result = 0;
        unsigned shift = 0;
        const auto start = mOffset;
        while (true) {
            if (mOffset >= mBytes.size()) {
                throw std::runtime_error(std::string(context) + " has truncated varint at payload offset " +
                    std::to_string(start));
            }
            const auto byte = mBytes[mOffset++];
            if (shift == 63 && (byte & 0xfeu) != 0) {
                throw std::runtime_error(std::string(context) + " varint overflows at payload offset " +
                    std::to_string(start));
            }
            result |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) return result;
            shift += 7;
            if (shift > 63) {
                throw std::runtime_error(std::string(context) + " varint overflows at payload offset " +
                    std::to_string(start));
            }
        }
    }

    std::string string(std::string_view context)
    {
        const auto length = varint(std::string(context) + " length");
        if (length > mBytes.size() - mOffset) {
            throw std::runtime_error(std::string(context) + " is truncated at payload offset " +
                std::to_string(mOffset));
        }
        const auto begin = reinterpret_cast<const char*>(mBytes.data() + mOffset);
        mOffset += static_cast<std::size_t>(length);
        return std::string(begin, static_cast<std::size_t>(length));
    }

private:
    std::span<const std::uint8_t> mBytes;
    std::size_t mOffset = 0;
};

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
        const auto comma = text.find(',', begin);
        const auto part = text.substr(begin,
            comma == std::string_view::npos ? text.size() - begin : comma - begin);
        if (!part.empty()) {
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

std::uint32_t runtime_id(RuntimeRegistry& registry, std::string block, const std::string& property)
{
    block = trim(std::move(block));
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
    std::istringstream property_stream(property);
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

void TibiReader::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> TibiReader::read(const std::filesystem::path& path)
{
    mCommands.clear();
    mOrigin = {};
    mOffset = {};
    mOriginalSize = {};
    mSize = {};
    mNonAirBlocks = 0;
    try {
        auto decoded = inflate_raw(read_file(path));
        if (decoded.size() < kHeaderSize) {
            throw std::runtime_error("TIBI decoded payload is shorter than 15-byte header");
        }
        std::vector<std::uint8_t> key_material(decoded.begin(), decoded.begin() + kHeaderSize);
        const auto suffix = std::string("TIBI_2025/5/19-Start") +
            std::to_string(decoded.size() - kHeaderSize);
        key_material.insert(key_material.end(), suffix.begin(), suffix.end());
        const auto key = md5(key_material);
        for (std::size_t index = kHeaderSize; index < decoded.size(); ++index) {
            decoded[index] ^= key[(index - kHeaderSize) % key.size()];
        }
        PayloadReader reader(std::span<const std::uint8_t>(decoded).subspan(kHeaderSize));
        const auto block_count = reader.varint("TIBI block table count");
        if (block_count > decoded.size()) throw std::runtime_error("TIBI block table count is invalid");
        std::vector<std::string> blocks;
        blocks.reserve(static_cast<std::size_t>(block_count));
        for (std::uint64_t index = 0; index < block_count; ++index) {
            reader.varint("TIBI block table line #" + std::to_string(index));
            blocks.push_back(reader.string("TIBI block table entry #" + std::to_string(index)));
        }
        const auto property_count = reader.varint("TIBI property table count");
        if (property_count > decoded.size()) throw std::runtime_error("TIBI property table count is invalid");
        std::vector<std::string> properties;
        properties.reserve(static_cast<std::size_t>(property_count));
        for (std::uint64_t index = 0; index < property_count; ++index) {
            reader.varint("TIBI property table line #" + std::to_string(index));
            properties.push_back(reader.string("TIBI property table entry #" + std::to_string(index)));
        }
        const auto command_count = reader.varint("TIBI command count");
        if (command_count > decoded.size()) throw std::runtime_error("TIBI command count is invalid");
        mCommands.reserve(static_cast<std::size_t>(command_count));
        auto min_x = std::numeric_limits<std::int32_t>::max();
        auto min_y = min_x;
        auto min_z = min_x;
        auto max_x = std::numeric_limits<std::int32_t>::min();
        auto max_y = max_x;
        auto max_z = max_x;
        std::uint64_t non_air = 0;
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
            command.runtime_id = runtime_id(mRegistry, blocks[static_cast<std::size_t>(block_index)],
                properties[static_cast<std::size_t>(property_index)]);
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
            mCommands.push_back(command);
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
    for (const auto& command : mCommands) {
        if (command.runtime_id == air) continue;
        const auto local = [&](BlockPos value) {
            return BlockPos{
                value.x - mOrigin.x + mOffset.x,
                value.y - mOrigin.y + mOffset.y,
                value.z - mOrigin.z + mOffset.z
            };
        };
        if (command.type == 0) {
            set_block(chunks, local(command.first), command.runtime_id, air);
        } else if (command.type == 1) {
            const auto first = local(command.first);
            const auto second = local(command.second);
            const auto fill_min_x = std::min(first.x, second.x);
            const auto fill_max_x = std::max(first.x, second.x);
            const auto fill_min_y = std::min(first.y, second.y);
            const auto fill_max_y = std::max(first.y, second.y);
            const auto fill_min_z = std::min(first.z, second.z);
            const auto fill_max_z = std::max(first.z, second.z);
            for (const auto& [chunk_position, _] : chunks) {
                const auto chunk_min_x = chunk_position.x * 16;
                const auto chunk_min_z = chunk_position.z * 16;
                const auto min_x = std::max(fill_min_x, chunk_min_x);
                const auto max_x = std::min(fill_max_x, chunk_min_x + 15);
                const auto min_z = std::max(fill_min_z, chunk_min_z);
                const auto max_z = std::min(fill_max_z, chunk_min_z + 15);
                if (min_x > max_x || min_z > max_z) continue;
                for (std::int64_t x = min_x; x <= max_x; ++x) {
                    for (std::int64_t y = fill_min_y; y <= fill_max_y; ++y) {
                        for (std::int64_t z = min_z; z <= max_z; ++z) {
                            set_block(chunks, {
                                static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                static_cast<std::int32_t>(z)
                            }, command.runtime_id, air);
                        }
                    }
                }
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
