#include "mianyang.hpp"

#include "nbt_text.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <streambuf>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::int32_t kMaxDimension = 1 << 20;
constexpr std::int32_t kMaxNamespaces = 1 << 16;
constexpr std::int32_t kMaxBlocks = 1 << 28;

class InflateStreamBuffer final : public std::streambuf {
public:
    InflateStreamBuffer(const std::filesystem::path& path, int window_bits,
                        std::string_view format)
        : mInput(path, std::ios::binary), mFormat(format)
    {
        if (!mInput) {
            throw std::runtime_error(
                "无法打开 " + mFormat + " 文件: " + path.string());
        }
        if (inflateInit2(&mStream, window_bits) != Z_OK) {
            throw std::runtime_error("初始化 " + mFormat + " 解压器失败");
        }
        mInitialized = true;
        setg(mDecoded.data(), mDecoded.data(), mDecoded.data());
    }

    ~InflateStreamBuffer() override
    {
        if (mInitialized) inflateEnd(&mStream);
    }

protected:
    int_type underflow() override
    {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (mFinished) return traits_type::eof();

        while (true) {
            if (mStream.avail_in == 0) {
                if (!mInputEof) {
                    mInput.read(reinterpret_cast<char*>(mCompressed.data()),
                        static_cast<std::streamsize>(mCompressed.size()));
                    const auto count = mInput.gcount();
                    if (count > 0) {
                        mStream.next_in = mCompressed.data();
                        mStream.avail_in = static_cast<uInt>(count);
                    } else {
                        if (mInput.bad()) {
                            throw std::runtime_error(
                                mFormat + " 读取失败，compressed offset " +
                                std::to_string(mStream.total_in));
                        }
                        // zlib may have returned Z_OK with a full output buffer exactly
                        // when it consumed the final input byte. Give it one empty-input
                        // call to report Z_STREAM_END before declaring truncation.
                        mInputEof = true;
                    }
                }
            }

            mStream.next_out = reinterpret_cast<Bytef*>(mDecoded.data());
            mStream.avail_out = static_cast<uInt>(mDecoded.size());
            const auto status = inflate(&mStream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                throw std::runtime_error(
                    mFormat + " 解压失败，compressed offset " +
                    std::to_string(mStream.total_in) + ": " +
                    (mStream.msg == nullptr ? std::to_string(status) : mStream.msg));
            }
            const auto produced = mDecoded.size() - mStream.avail_out;
            if (mInputEof && status != Z_STREAM_END && produced == 0) {
                throw std::runtime_error(
                    mFormat + " 压缩流被截断，compressed offset " +
                    std::to_string(mStream.total_in));
            }
            if (mDecodedBytes > kMaxDecodedBytes - produced) {
                throw std::runtime_error(mFormat + " 解压结果超过 2 GiB 限制");
            }
            mDecodedBytes += produced;
            if (status == Z_STREAM_END) mFinished = true;
            if (produced != 0) {
                setg(mDecoded.data(), mDecoded.data(), mDecoded.data() + produced);
                return traits_type::to_int_type(*gptr());
            }
            if (mFinished) return traits_type::eof();
        }
    }

private:
    std::ifstream mInput;
    std::string mFormat;
    z_stream mStream{};
    bool mInitialized = false;
    bool mInputEof = false;
    bool mFinished = false;
    std::size_t mDecodedBytes = 0;
    std::array<Bytef, kStreamChunk> mCompressed{};
    std::array<char, kStreamChunk> mDecoded{};
};

class InflateInputStream final : public std::istream {
public:
    InflateInputStream(const std::filesystem::path& path, int window_bits,
                       std::string_view format)
        : std::istream(nullptr), mBuffer(path, window_bits, format)
    {
        rdbuf(&mBuffer);
        clear();
        exceptions(std::ios::badbit);
    }

private:
    InflateStreamBuffer mBuffer;
};

template <typename Callback>
void parse_json_pass(const std::filesystem::path& path, bool compressed,
                     Callback&& callback)
{
    if (compressed) {
        InflateInputStream input(path, MAX_WBITS, "MianYangV3");
        const auto discarded = nlohmann::json::parse(input, std::forward<Callback>(callback));
        (void)discarded;
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法打开 MianYang JSON 文件: " + path.string());
    }
    const auto discarded = nlohmann::json::parse(input, std::forward<Callback>(callback));
    (void)discarded;
}

std::int32_t json_int32(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        throw std::runtime_error("字段 " + std::string(field) + " 不是整数");
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("字段 " + std::string(field) + " 超出 int32 范围");
    }
    return static_cast<std::int32_t>(number);
}

std::uint16_t json_uint16(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        throw std::runtime_error("字段 " + std::string(field) + " 不是整数");
    }
    const auto number = value.get<std::int64_t>();
    if (number < 0 || number > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("字段 " + std::string(field) + " 超出 uint16 范围");
    }
    return static_cast<std::uint16_t>(number);
}

std::int32_t checked_add(std::int32_t left, std::int32_t right, std::string_view field)
{
    const auto result = static_cast<std::int64_t>(left) + right;
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " 坐标溢出 int32");
    }
    return static_cast<std::int32_t>(result);
}

std::int32_t floor_mod64(std::int64_t value, std::int32_t divisor)
{
    auto remainder = value % divisor;
    if (remainder < 0) remainder += divisor;
    return static_cast<std::int32_t>(remainder);
}

class BinaryReader {
public:
    BinaryReader(std::istream& input, std::string_view format)
        : mInput(input), mFormat(format) {}

    std::size_t offset() const noexcept { return mPosition; }

    std::uint8_t u8(std::string_view field)
    {
        std::array<std::uint8_t, 1> bytes{};
        read_exact(std::span<std::uint8_t>(bytes), field);
        return bytes[0];
    }

    std::uint16_t u16(std::string_view field)
    {
        std::array<std::uint8_t, 2> bytes{};
        read_exact(std::span<std::uint8_t>(bytes), field);
        return static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(bytes[1] << 8);
    }

    std::int16_t i16(std::string_view field)
    {
        return static_cast<std::int16_t>(u16(field));
    }

    std::int32_t i32(std::string_view field)
    {
        std::array<std::uint8_t, 4> bytes{};
        read_exact(std::span<std::uint8_t>(bytes), field);
        const auto value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
        return static_cast<std::int32_t>(value);
    }

    std::string bytes(std::size_t count, std::string_view field)
    {
        std::string result(count, '\0');
        read_exact(std::span<char>(result.data(), result.size()), field);
        return result;
    }

    void skip(std::size_t count, std::string_view field)
    {
        std::array<char, kStreamChunk> scratch{};
        while (count != 0) {
            const auto amount = std::min(count, scratch.size());
            read_exact(std::span<char>(scratch.data(), amount), field);
            count -= amount;
        }
    }

private:
    template <typename Byte>
    void read_exact(std::span<Byte> output, std::string_view field)
    {
        const auto start = mPosition;
        auto* destination = reinterpret_cast<char*>(output.data());
        auto remaining = output.size_bytes();
        while (remaining != 0) {
            const auto amount = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()));
            mInput.read(destination, static_cast<std::streamsize>(amount));
            const auto read = mInput.gcount();
            if (read > 0) {
                destination += read;
                remaining -= static_cast<std::size_t>(read);
                mPosition += static_cast<std::size_t>(read);
            }
            if (static_cast<std::size_t>(std::max<std::streamsize>(read, 0)) != amount) {
                throw std::runtime_error(
                    std::string(mFormat) + " " + std::string(field) +
                    " 被截断，decoded offset " + std::to_string(start));
            }
        }
    }

    std::istream& mInput;
    std::string_view mFormat;
    std::size_t mPosition = 0;
};

void validate_dimension(std::int32_t value, std::string_view field)
{
    if (value <= 0) throw std::runtime_error(std::string(field) + " 必须为正数");
    if (value > kMaxDimension) {
        throw std::runtime_error(std::string(field) + " 超过安全上限 " + std::to_string(kMaxDimension));
    }
}

} // namespace

std::string_view MianYangStructure::name() const noexcept
{
    switch (mVersion) {
    case StructureId::MianYangV1: return "MianYangV1";
    case StructureId::MianYangV2: return "MianYangV2";
    case StructureId::MianYangV3: return "MianYangV3";
    case StructureId::MianYangV4: return "MianYangV4";
    default: return "MianYang";
    }
}

std::uint32_t MianYangStructure::runtime_id(std::string_view name, std::uint16_t data)
{
    if (const auto runtime = mRegistry.legacy_runtime_id(name, data)) return *runtime;
    if (const auto unknown = mRegistry.find("minecraft:unknown")) return *unknown;
    return mRegistry.register_state({ "minecraft:unknown", {}, 0 });
}

Result<void> MianYangStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    if (mVersion == StructureId::MianYangV1 || mVersion == StructureId::MianYangV2) {
        return read_json(path, false);
    }
    if (mVersion == StructureId::MianYangV3) return read_json(path, true);
    if (mVersion == StructureId::MianYangV4) return read_binary_v4(path);
    return Result<void>::failure("MianYang reader 收到无效版本");
}

Result<void> MianYangStructure::read_json(const std::filesystem::path& path, bool compressed)
{
    try {
        struct ChunkMetadata {
            std::int32_t start_x = 0;
            std::int32_t start_z = 0;
            std::size_t blocks_occurrences = 0;
            bool final_blocks_is_array = false;
        };

        std::vector<std::string> namespaces;
        std::vector<ChunkMetadata> chunks;
        bool root_is_object = false;
        bool saw_namespaces = false;
        bool saw_chunked_blocks = false;
        bool inside_namespaces = false;
        bool inside_chunked_blocks = false;
        bool inside_chunk = false;
        bool inside_blocks_array = false;
        std::string root_key;
        std::string chunk_key;
        nlohmann::json start_x_raw;
        nlohmann::json start_z_raw;
        bool has_start_x = false;
        bool has_start_z = false;
        std::size_t block_member_occurrences = 0;
        bool final_blocks_is_array = false;
        std::size_t chunked_blocks_occurrences = 0;

        // Metadata pass: keep only namespaces and one small record per chunk.  Every block
        // entry is discarded at depth 4, including for compressed V3 input.
        const auto inspect_callback = [&](int depth, nlohmann::json::parse_event_t event,
                                          nlohmann::json& parsed) -> bool {
            if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                root_is_object = true;
                return true;
            }
            if (depth == 0 && (event == nlohmann::json::parse_event_t::array_start ||
                    event == nlohmann::json::parse_event_t::value)) {
                return false;
            }
            if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                root_key = parsed.get<std::string>();
                return true;
            }
            if (depth == 1 && root_key == "namespaces" &&
                event == nlohmann::json::parse_event_t::array_start) {
                namespaces.clear();
                saw_namespaces = true;
                inside_namespaces = true;
                return true;
            }
            if (depth == 1 && root_key == "namespaces" &&
                (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start)) {
                namespaces.clear();
                saw_namespaces = false;
                return false;
            }
            if (inside_namespaces && depth == 2) {
                if (event == nlohmann::json::parse_event_t::value) {
                    if (!parsed.is_string() || parsed.get_ref<const std::string&>().empty()) {
                        throw std::runtime_error(
                            std::string(name()) + " namespace #" +
                            std::to_string(namespaces.size()) + " 无效");
                    }
                    if (namespaces.size() == static_cast<std::size_t>(kMaxNamespaces)) {
                        throw std::runtime_error(
                            std::string(name()) + " namespace 数量超过安全上限 " +
                            std::to_string(kMaxNamespaces));
                    }
                    namespaces.push_back(parsed.get<std::string>());
                    return false;
                }
                if (event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start) {
                    throw std::runtime_error(
                        std::string(name()) + " namespace #" +
                        std::to_string(namespaces.size()) + " 无效");
                }
            }
            if (inside_namespaces && depth == 1 &&
                event == nlohmann::json::parse_event_t::array_end) {
                inside_namespaces = false;
                return false;
            }
            if (depth == 1 && root_key == "chunkedBlocks" &&
                event == nlohmann::json::parse_event_t::array_start) {
                if (chunked_blocks_occurrences ==
                    std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error("chunkedBlocks occurrence count overflow");
                }
                ++chunked_blocks_occurrences;
                chunks.clear();
                saw_chunked_blocks = true;
                inside_chunked_blocks = true;
                return true;
            }
            if (depth == 1 && root_key == "chunkedBlocks" &&
                (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start)) {
                if (chunked_blocks_occurrences ==
                    std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error("chunkedBlocks occurrence count overflow");
                }
                ++chunked_blocks_occurrences;
                chunks.clear();
                saw_chunked_blocks = false;
                inside_chunked_blocks = false;
                return false;
            }
            if (inside_chunked_blocks && depth == 2 &&
                event == nlohmann::json::parse_event_t::object_start) {
                inside_chunk = true;
                inside_blocks_array = false;
                chunk_key.clear();
                start_x_raw = {};
                start_z_raw = {};
                has_start_x = false;
                has_start_z = false;
                block_member_occurrences = 0;
                final_blocks_is_array = false;
                return true;
            }
            if (inside_chunked_blocks && depth == 2 &&
                (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::array_start)) {
                throw std::runtime_error(
                    std::string(name()) + " chunk #" + std::to_string(chunks.size()) +
                    " 缺少 blocks");
            }
            if (inside_chunk && depth == 3 &&
                event == nlohmann::json::parse_event_t::key) {
                chunk_key = parsed.get<std::string>();
                return true;
            }
            if (inside_blocks_array && depth == 4 &&
                (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start ||
                    event == nlohmann::json::parse_event_t::object_end ||
                    event == nlohmann::json::parse_event_t::array_end)) {
                return false;
            }
            if (inside_blocks_array && depth == 3 &&
                event == nlohmann::json::parse_event_t::array_end) {
                inside_blocks_array = false;
                return false;
            }
            if (inside_chunk && depth == 3 &&
                (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start)) {
                if (chunk_key == "startX") {
                    has_start_x = true;
                    start_x_raw = event == nlohmann::json::parse_event_t::value
                        ? parsed
                        : (event == nlohmann::json::parse_event_t::object_start
                            ? nlohmann::json::object() : nlohmann::json::array());
                    return false;
                }
                if (chunk_key == "startZ") {
                    has_start_z = true;
                    start_z_raw = event == nlohmann::json::parse_event_t::value
                        ? parsed
                        : (event == nlohmann::json::parse_event_t::object_start
                            ? nlohmann::json::object() : nlohmann::json::array());
                    return false;
                }
                if (chunk_key == "blocks") {
                    if (block_member_occurrences ==
                        std::numeric_limits<std::size_t>::max()) {
                        throw std::runtime_error("blocks occurrence count overflow");
                    }
                    ++block_member_occurrences;
                    final_blocks_is_array =
                        event == nlohmann::json::parse_event_t::array_start;
                    inside_blocks_array = final_blocks_is_array;
                    return inside_blocks_array;
                }
                return false;
            }
            if (inside_chunk && depth == 3 &&
                (event == nlohmann::json::parse_event_t::object_end ||
                    event == nlohmann::json::parse_event_t::array_end)) {
                return false;
            }
            if (inside_chunk && depth == 2 &&
                event == nlohmann::json::parse_event_t::object_end) {
                if (block_member_occurrences == 0 || !final_blocks_is_array) {
                    throw std::runtime_error(
                        std::string(name()) + " chunk #" + std::to_string(chunks.size()) +
                        " 缺少 blocks");
                }
                chunks.push_back({
                    json_int32(has_start_x ? start_x_raw : nlohmann::json{}, "startX"),
                    json_int32(has_start_z ? start_z_raw : nlohmann::json{}, "startZ"),
                    block_member_occurrences,
                    final_blocks_is_array
                });
                inside_chunk = false;
                return false;
            }
            if (inside_chunked_blocks && depth == 1 &&
                event == nlohmann::json::parse_event_t::array_end) {
                inside_chunked_blocks = false;
                return false;
            }
            if (depth == 1 && (event == nlohmann::json::parse_event_t::value ||
                    event == nlohmann::json::parse_event_t::object_start ||
                    event == nlohmann::json::parse_event_t::array_start ||
                    event == nlohmann::json::parse_event_t::object_end ||
                    event == nlohmann::json::parse_event_t::array_end)) {
                return false;
            }
            return true;
        };
        parse_json_pass(path, compressed, inspect_callback);
        if (!root_is_object || !saw_chunked_blocks || chunks.empty() ||
            !saw_namespaces || namespaces.empty()) {
            return Result<void>::failure(
                std::string(name()) + " 缺少非空 chunkedBlocks/namespaces");
        }

        BlockPos minimum{
            std::numeric_limits<std::int32_t>::max(),
            std::numeric_limits<std::int32_t>::max(),
            std::numeric_limits<std::int32_t>::max() };
        BlockPos maximum{
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::min() };
        std::map<std::pair<std::size_t, std::uint16_t>, std::uint32_t> palette;

        auto stream_blocks = [&](const bool materialize) -> std::size_t {
            bool pass_inside_chunked_blocks = false;
            bool active_chunked_blocks = false;
            bool pass_inside_chunk = false;
            bool pass_inside_blocks_array = false;
            bool active_blocks_array = false;
            std::string pass_root_key;
            std::string pass_chunk_key;
            std::size_t root_occurrence = 0;
            std::size_t chunk_index = 0;
            std::size_t blocks_occurrence = 0;
            std::size_t block_index = 0;
            std::size_t total_blocks = 0;

            auto consume_entry = [&](const nlohmann::json& entry) {
                if (chunk_index >= chunks.size()) {
                    throw std::runtime_error(std::string(name()) + " changed while parsing");
                }
                const auto context = std::string(name()) + " chunk #" +
                    std::to_string(chunk_index) + " block #" +
                    std::to_string(block_index++);
                try {
                    if (!entry.is_array() || entry.size() < 5) {
                        throw std::runtime_error("条目长度小于 5");
                    }
                    const auto namespace_index = json_int32(entry[0], "方块索引");
                    if (namespace_index < 0 ||
                        static_cast<std::size_t>(namespace_index) >= namespaces.size()) {
                        throw std::runtime_error(
                            "namespace 索引越界: " + std::to_string(namespace_index));
                    }
                    const auto data = json_uint16(entry[1], "方块数据");
                    const BlockPos world{
                        checked_add(chunks[chunk_index].start_x,
                            json_int32(entry[2], "局部 x"), "x"),
                        json_int32(entry[3], "局部 y"),
                        checked_add(chunks[chunk_index].start_z,
                            json_int32(entry[4], "局部 z"), "z")
                    };
                    if (entry.size() >= 6 && !entry[5].is_string()) {
                        throw std::runtime_error("NBT 负载必须为字符串");
                    }
                    if (total_blocks == static_cast<std::size_t>(kMaxBlocks)) {
                        throw std::runtime_error(
                            std::string(name()) + " 方块数量超过安全上限 " +
                            std::to_string(kMaxBlocks));
                    }
                    ++total_blocks;

                    if (!materialize) {
                        minimum.x = std::min(minimum.x, world.x);
                        minimum.y = std::min(minimum.y, world.y);
                        minimum.z = std::min(minimum.z, world.z);
                        maximum.x = std::max(maximum.x, world.x);
                        maximum.y = std::max(maximum.y, world.y);
                        maximum.z = std::max(maximum.z, world.z);
                        return;
                    }

                    const auto palette_key = std::pair{
                        static_cast<std::size_t>(namespace_index), data };
                    auto runtime = palette.find(palette_key);
                    if (runtime == palette.end()) {
                        runtime = palette.emplace(palette_key,
                            runtime_id(namespaces[palette_key.first], data)).first;
                    }
                    NbtPayload nbt;
                    if (entry.size() >= 6) {
                        const auto parsed_nbt = parse_mianyang_nbt(
                            entry[5].get_ref<const std::string&>());
                        if (!parsed_nbt) {
                            throw std::runtime_error(
                                "坐标 (" + std::to_string(world.x) + "," +
                                std::to_string(world.y) + "," +
                                std::to_string(world.z) + ") NBT 解析失败: " +
                                parsed_nbt.error());
                        }
                        nbt = parsed_nbt.value();
                    }
                    const BlockPos local{
                        world.x - minimum.x,
                        world.y - minimum.y,
                        world.z - minimum.z
                    };
                    mStore.put(local, runtime->second);
                    if (!nbt.empty()) mStore.put_entity(local, std::move(nbt));
                } catch (const std::exception& error) {
                    throw std::runtime_error(context + ": " + error.what());
                }
            };

            const auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                                      nlohmann::json& parsed) -> bool {
                if (depth == 0 && event == nlohmann::json::parse_event_t::object_start) {
                    return true;
                }
                if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
                    pass_root_key = parsed.get<std::string>();
                    return true;
                }
                if (depth == 1 && pass_root_key == "chunkedBlocks" &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start)) {
                    ++root_occurrence;
                    pass_inside_chunked_blocks =
                        event == nlohmann::json::parse_event_t::array_start;
                    active_chunked_blocks = pass_inside_chunked_blocks &&
                        root_occurrence == chunked_blocks_occurrences;
                    return pass_inside_chunked_blocks;
                }
                if (pass_inside_chunked_blocks && !active_chunked_blocks && depth == 2 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                if (pass_inside_chunked_blocks && depth == 2 &&
                    event == nlohmann::json::parse_event_t::object_start) {
                    if (!active_chunked_blocks) return false;
                    if (chunk_index >= chunks.size()) {
                        throw std::runtime_error(std::string(name()) + " changed while parsing");
                    }
                    pass_inside_chunk = true;
                    pass_chunk_key.clear();
                    blocks_occurrence = 0;
                    block_index = 0;
                    return true;
                }
                if (pass_inside_chunk && depth == 3 &&
                    event == nlohmann::json::parse_event_t::key) {
                    pass_chunk_key = parsed.get<std::string>();
                    return true;
                }
                if (pass_inside_chunk && depth == 3 && pass_chunk_key == "blocks" &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start)) {
                    ++blocks_occurrence;
                    pass_inside_blocks_array =
                        event == nlohmann::json::parse_event_t::array_start;
                    active_blocks_array = pass_inside_blocks_array &&
                        blocks_occurrence == chunks[chunk_index].blocks_occurrences;
                    return pass_inside_blocks_array;
                }
                if (pass_inside_blocks_array && depth == 4) {
                    if (event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start) {
                        return active_blocks_array;
                    }
                    if (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end) {
                        if (active_blocks_array) consume_entry(parsed);
                        return false;
                    }
                }
                if (pass_inside_blocks_array && depth == 3 &&
                    event == nlohmann::json::parse_event_t::array_end) {
                    pass_inside_blocks_array = false;
                    active_blocks_array = false;
                    return false;
                }
                if (pass_inside_chunk && depth == 3 &&
                    (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                if (pass_inside_chunk && depth == 2 &&
                    event == nlohmann::json::parse_event_t::object_end) {
                    pass_inside_chunk = false;
                    ++chunk_index;
                    return false;
                }
                if (pass_inside_chunked_blocks && depth == 1 &&
                    event == nlohmann::json::parse_event_t::array_end) {
                    pass_inside_chunked_blocks = false;
                    active_chunked_blocks = false;
                    return false;
                }
                if (depth == 1 && (event == nlohmann::json::parse_event_t::value ||
                        event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start ||
                        event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end)) {
                    return false;
                }
                return true;
            };
            parse_json_pass(path, compressed, callback);
            if (chunk_index != chunks.size()) {
                throw std::runtime_error(std::string(name()) + " changed while parsing");
            }
            return total_blocks;
        };

        const auto block_count = stream_blocks(false);
        if (block_count == 0) {
            return Result<void>::failure(std::string(name()) + " 没有方块");
        }
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) {
            return Result<void>::failure(std::string(name()) + " 归一化尺寸溢出 int32");
        }
        mStore.set_size({
            static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) });
        const auto materialized_blocks = stream_blocks(true);
        if (materialized_blocks != block_count) {
            throw std::runtime_error(std::string(name()) + " changed while parsing");
        }
        mNonAirBlocks = mStore.count_non_air();
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("解析 " + std::string(name()) + " 失败: " + error.what());
    }
}

Result<void> MianYangStructure::read_binary_v4(const std::filesystem::path& path)
{
    try {
        InflateInputStream decoded(path, MAX_WBITS + 16, "MianYangV4");
        BinaryReader input(decoded, "MianYangV4");
        (void)input.i32("minX");
        (void)input.i32("minY");
        (void)input.i32("minZ");
        const auto width = input.i32("width");
        const auto height = input.i32("height");
        const auto length = input.i32("length");
        validate_dimension(width, "MianYangV4 width");
        validate_dimension(height, "MianYangV4 height");
        validate_dimension(length, "MianYangV4 length");
        const auto namespace_count = input.i32("namespace count");
        if (namespace_count <= 0 || namespace_count > kMaxNamespaces) {
            return Result<void>::failure("MianYangV4 namespace count 无效: " +
                std::to_string(namespace_count));
        }
        const auto block_count = input.i32("block count");
        if (block_count <= 0 || block_count > kMaxBlocks) {
            return Result<void>::failure("MianYangV4 block count 无效: " + std::to_string(block_count));
        }

        std::vector<std::string> namespaces;
        namespaces.reserve(static_cast<std::size_t>(namespace_count));
        for (std::int32_t i = 0; i < namespace_count; ++i) {
            const auto size = input.u16("namespace #" + std::to_string(i) + " length");
            if (size == 0) {
                return Result<void>::failure("MianYangV4 namespace #" + std::to_string(i) + " 长度为 0");
            }
            namespaces.push_back(input.bytes(size, "namespace #" + std::to_string(i) + " data"));
        }

        mStore.set_size({ width, height, length });
        std::map<std::pair<std::size_t, std::uint16_t>, std::uint32_t> palette;
        std::int64_t cursor_x = 0, cursor_y = 0, cursor_z = 0;
        for (std::int32_t i = 0; i < block_count; ++i) {
            const auto context = "block #" + std::to_string(i);
            cursor_x += input.i16(context + " dx");
            cursor_y += input.i16(context + " dy");
            cursor_z += input.i16(context + " dz");
            const BlockPos local{
                floor_mod64(cursor_x, width),
                floor_mod64(cursor_y, height),
                floor_mod64(cursor_z, length) };
            const auto namespace_index = input.u8(context + " namespace index");
            const auto data = input.u8(context + " aux");
            if (namespace_index >= namespaces.size()) {
                return Result<void>::failure(
                    "MianYangV4 " + context + " namespace 索引越界: " +
                    std::to_string(namespace_index) + ", decoded offset " +
                    std::to_string(input.offset()));
            }
            const auto nbt_length = input.i32(context + " NBT length");
            if (nbt_length < 0) {
                return Result<void>::failure("MianYangV4 " + context + " NBT 长度为负数，decoded offset " +
                    std::to_string(input.offset() - 4));
            }
            NbtPayload nbt;
            if (nbt_length > 0) {
                const auto text = input.bytes(static_cast<std::size_t>(nbt_length), context + " NBT data");
                const auto parsed = parse_mianyang_nbt(text);
                if (!parsed) {
                    return Result<void>::failure(
                        "MianYangV4 " + context + " 坐标 (" + std::to_string(local.x) + "," +
                        std::to_string(local.y) + "," + std::to_string(local.z) +
                        ") NBT 解析失败: " + parsed.error());
                }
                nbt = parsed.value();
            }
            const auto entity_length = input.i32(context + " entity length");
            if (entity_length < 0) {
                return Result<void>::failure("MianYangV4 " + context + " entity 长度为负数，decoded offset " +
                    std::to_string(input.offset() - 4));
            }
            input.skip(static_cast<std::size_t>(entity_length), context + " entity data");

            const auto key = std::pair{ static_cast<std::size_t>(namespace_index), static_cast<std::uint16_t>(data) };
            auto runtime = palette.find(key);
            if (runtime == palette.end()) {
                runtime = palette.emplace(key, runtime_id(namespaces[key.first], key.second)).first;
            }
            mStore.put(local, runtime->second);
            if (!nbt.empty()) mStore.put_entity(local, std::move(nbt));
        }
        mNonAirBlocks = mStore.count_non_air();
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("解析 MianYangV4 失败: " + std::string(error.what()));
    }
}

Result<void> MianYangStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> MianYangStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(std::string(name()) + " 没有 Go FromMCWorld capability");
}

} // namespace water_structure
