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
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::int32_t kMaxDimension = 1 << 20;
constexpr std::int32_t kMaxNamespaces = 1 << 16;
constexpr std::int32_t kMaxBlocks = 1 << 28;

struct PendingBlock {
    BlockPos world{};
    std::uint32_t runtime_id = 0;
    NbtPayload nbt;
};

std::vector<std::uint8_t> inflate_file(
    const std::filesystem::path& path, int window_bits, std::string_view format)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法打开 " + std::string(format) + " 文件: " + path.string());

    z_stream stream{};
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        throw std::runtime_error("初始化 " + std::string(format) + " 解压器失败");
    }
    struct Guard {
        z_stream* stream;
        ~Guard() { inflateEnd(stream); }
    } guard{ &stream };

    std::array<std::uint8_t, kStreamChunk> compressed{};
    std::array<std::uint8_t, kStreamChunk> decoded{};
    std::vector<std::uint8_t> output;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0) {
            input.read(reinterpret_cast<char*>(compressed.data()),
                static_cast<std::streamsize>(compressed.size()));
            const auto count = input.gcount();
            if (count <= 0) {
                throw std::runtime_error(
                    std::string(format) + " 压缩流被截断，compressed offset " +
                    std::to_string(stream.total_in));
            }
            stream.next_in = compressed.data();
            stream.avail_in = static_cast<uInt>(count);
        }
        stream.next_out = decoded.data();
        stream.avail_out = static_cast<uInt>(decoded.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::runtime_error(
                std::string(format) + " 解压失败，compressed offset " +
                std::to_string(stream.total_in) + ": " +
                (stream.msg == nullptr ? std::to_string(status) : stream.msg));
        }
        const auto produced = decoded.size() - stream.avail_out;
        if (output.size() > kMaxDecodedBytes - produced) {
            throw std::runtime_error(std::string(format) + " 解压结果超过 2 GiB 限制");
        }
        output.insert(output.end(), decoded.begin(), decoded.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    return output;
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
    BinaryReader(std::span<const std::uint8_t> data, std::string_view format)
        : mData(data), mFormat(format) {}

    std::size_t offset() const noexcept { return mPosition; }

    std::uint8_t u8(std::string_view field)
    {
        require(1, field);
        return mData[mPosition++];
    }

    std::uint16_t u16(std::string_view field)
    {
        require(2, field);
        const auto value = static_cast<std::uint16_t>(mData[mPosition]) |
            static_cast<std::uint16_t>(mData[mPosition + 1] << 8);
        mPosition += 2;
        return value;
    }

    std::int16_t i16(std::string_view field)
    {
        return static_cast<std::int16_t>(u16(field));
    }

    std::int32_t i32(std::string_view field)
    {
        require(4, field);
        const auto value = static_cast<std::uint32_t>(mData[mPosition]) |
            (static_cast<std::uint32_t>(mData[mPosition + 1]) << 8) |
            (static_cast<std::uint32_t>(mData[mPosition + 2]) << 16) |
            (static_cast<std::uint32_t>(mData[mPosition + 3]) << 24);
        mPosition += 4;
        return static_cast<std::int32_t>(value);
    }

    std::string bytes(std::size_t count, std::string_view field)
    {
        require(count, field);
        std::string result(reinterpret_cast<const char*>(mData.data() + mPosition), count);
        mPosition += count;
        return result;
    }

    void skip(std::size_t count, std::string_view field)
    {
        require(count, field);
        mPosition += count;
    }

private:
    void require(std::size_t count, std::string_view field) const
    {
        if (count > mData.size() - mPosition) {
            throw std::runtime_error(
                std::string(mFormat) + " " + std::string(field) +
                " 被截断，decoded offset " + std::to_string(mPosition));
        }
    }

    std::span<const std::uint8_t> mData;
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
        nlohmann::json document;
        if (compressed) {
            const auto bytes = inflate_file(path, MAX_WBITS, "MianYangV3");
            document = nlohmann::json::parse(bytes.begin(), bytes.end());
        } else {
            std::ifstream input(path, std::ios::binary);
            if (!input) return Result<void>::failure("无法打开 " + std::string(name()) + " 文件: " + path.string());
            document = nlohmann::json::parse(input);
        }
        if (!document.is_object() || !document.contains("chunkedBlocks") ||
            !document["chunkedBlocks"].is_array() || document["chunkedBlocks"].empty() ||
            !document.contains("namespaces") || !document["namespaces"].is_array() ||
            document["namespaces"].empty()) {
            return Result<void>::failure(std::string(name()) + " 缺少非空 chunkedBlocks/namespaces");
        }

        std::vector<std::string> namespaces;
        namespaces.reserve(document["namespaces"].size());
        for (std::size_t i = 0; i < document["namespaces"].size(); ++i) {
            const auto& value = document["namespaces"][i];
            if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
                return Result<void>::failure(
                    std::string(name()) + " namespace #" + std::to_string(i) + " 无效");
            }
            namespaces.push_back(value.get<std::string>());
        }

        std::map<std::pair<std::size_t, std::uint16_t>, std::uint32_t> palette;
        std::vector<PendingBlock> blocks;
        BlockPos minimum{
            std::numeric_limits<std::int32_t>::max(),
            std::numeric_limits<std::int32_t>::max(),
            std::numeric_limits<std::int32_t>::max() };
        BlockPos maximum{
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::min() };

        for (std::size_t chunk_index = 0; chunk_index < document["chunkedBlocks"].size(); ++chunk_index) {
            const auto& chunk = document["chunkedBlocks"][chunk_index];
            if (!chunk.is_object() || !chunk.contains("blocks") || !chunk["blocks"].is_array()) {
                return Result<void>::failure(
                    std::string(name()) + " chunk #" + std::to_string(chunk_index) + " 缺少 blocks");
            }
            const auto start_x = json_int32(chunk.value("startX", nlohmann::json{}), "startX");
            const auto start_z = json_int32(chunk.value("startZ", nlohmann::json{}), "startZ");
            for (std::size_t block_index = 0; block_index < chunk["blocks"].size(); ++block_index) {
                const auto& entry = chunk["blocks"][block_index];
                const auto context = std::string(name()) + " chunk #" + std::to_string(chunk_index) +
                    " block #" + std::to_string(block_index);
                if (!entry.is_array() || entry.size() < 5) {
                    return Result<void>::failure(context + " 条目长度小于 5");
                }
                const auto namespace_index = json_int32(entry[0], "方块索引");
                if (namespace_index < 0 || static_cast<std::size_t>(namespace_index) >= namespaces.size()) {
                    return Result<void>::failure(context + " namespace 索引越界: " + std::to_string(namespace_index));
                }
                const auto data = json_uint16(entry[1], "方块数据");
                const auto local_x = json_int32(entry[2], "局部 x");
                const auto local_y = json_int32(entry[3], "局部 y");
                const auto local_z = json_int32(entry[4], "局部 z");
                const BlockPos world{
                    checked_add(start_x, local_x, "x"),
                    local_y,
                    checked_add(start_z, local_z, "z") };
                const auto key = std::pair{ static_cast<std::size_t>(namespace_index), data };
                auto runtime = palette.find(key);
                if (runtime == palette.end()) {
                    runtime = palette.emplace(key, runtime_id(namespaces[key.first], data)).first;
                }
                NbtPayload nbt;
                if (entry.size() >= 6) {
                    if (!entry[5].is_string()) {
                        return Result<void>::failure(context + " NBT 负载必须为字符串");
                    }
                    const auto parsed = parse_mianyang_nbt(entry[5].get_ref<const std::string&>());
                    if (!parsed) {
                        return Result<void>::failure(
                            context + " 坐标 (" + std::to_string(world.x) + "," +
                            std::to_string(world.y) + "," + std::to_string(world.z) +
                            ") NBT 解析失败: " + parsed.error());
                    }
                    nbt = parsed.value();
                }
                minimum.x = std::min(minimum.x, world.x);
                minimum.y = std::min(minimum.y, world.y);
                minimum.z = std::min(minimum.z, world.z);
                maximum.x = std::max(maximum.x, world.x);
                maximum.y = std::max(maximum.y, world.y);
                maximum.z = std::max(maximum.z, world.z);
                blocks.push_back({ world, runtime->second, std::move(nbt) });
                if (runtime->second != mRegistry.air_runtime_id()) ++mNonAirBlocks;
            }
        }
        if (blocks.empty()) return Result<void>::failure(std::string(name()) + " 没有方块");
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
        for (auto& block : blocks) {
            const BlockPos local{
                block.world.x - minimum.x,
                block.world.y - minimum.y,
                block.world.z - minimum.z };
            mStore.put(local, block.runtime_id);
            if (!block.nbt.empty()) mStore.put_entity(local, std::move(block.nbt));
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("解析 " + std::string(name()) + " 失败: " + error.what());
    }
}

Result<void> MianYangStructure::read_binary_v4(const std::filesystem::path& path)
{
    try {
        const auto bytes = inflate_file(path, MAX_WBITS + 16, "MianYangV4");
        BinaryReader input(bytes, "MianYangV4");
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
            if (runtime->second != mRegistry.air_runtime_id()) ++mNonAirBlocks;
        }
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
