#include "bdx.hpp"
#include "../core/bounded_thread_pool.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <brotli/decode.h>
#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string_view>
#include <unordered_set>

namespace water_structure {
namespace {

constexpr std::size_t kBrotliBufferSize = 1024 * 1024;
constexpr std::size_t kMinimumDecodedLimit = 64ull * 1024 * 1024;
constexpr std::size_t kMaximumDecodedLimit = 64ull * 1024 * 1024 * 1024;
constexpr std::size_t kMaximumExpansionRatio = 4096;

std::size_t decoded_limit(std::uintmax_t compressed_size)
{
    const auto scaled = compressed_size > kMaximumDecodedLimit / kMaximumExpansionRatio
        ? kMaximumDecodedLimit
        : static_cast<std::size_t>(compressed_size) * kMaximumExpansionRatio;
    return std::clamp(scaled, kMinimumDecodedLimit, kMaximumDecodedLimit);
}

struct BrotliProfile {
    std::chrono::steady_clock::duration input_duration{};
    std::chrono::steady_clock::duration decode_duration{};
    std::uint64_t input_calls = 0;
    std::uint64_t decode_calls = 0;
};

class BrotliStreamBuffer final : public std::streambuf {
public:
    BrotliStreamBuffer(
        std::istream& input,
        std::size_t output_limit,
        BrotliProfile* profile = nullptr)
        : mInput(input),
          mOutputLimit(output_limit),
          mProfile(profile),
          mDecoder(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr),
              &BrotliDecoderDestroyInstance)
    {
        if (!mDecoder) throw std::runtime_error("无法创建 BDX Brotli decoder");
        setg(nullptr, nullptr, nullptr);
    }

    std::size_t offset() const noexcept
    {
        const auto unread = gptr() == nullptr
            ? 0
            : static_cast<std::size_t>(egptr() - gptr());
        return mProduced - unread;
    }

    const std::string& error() const noexcept { return mError; }

    std::size_t unread() const noexcept
    {
        return gptr() == nullptr ? 0 : static_cast<std::size_t>(egptr() - gptr());
    }

    std::optional<std::uint8_t> try_u8()
    {
        if (gptr() == nullptr || gptr() == egptr()) {
            if (underflow() == traits_type::eof()) return std::nullopt;
        }
        const auto value = static_cast<std::uint8_t>(*gptr());
        gbump(1);
        return value;
    }

    bool read_exact(std::span<std::uint8_t> output)
    {
        while (!output.empty()) {
            if (gptr() == nullptr || gptr() == egptr()) {
                if (underflow() == traits_type::eof()) return false;
            }
            const auto count = std::min(output.size(), unread());
            std::memcpy(output.data(), gptr(), count);
            gbump(static_cast<int>(count));
            output = output.subspan(count);
        }
        return true;
    }

    bool skip(std::size_t count)
    {
        while (count != 0) {
            if (gptr() == nullptr || gptr() == egptr()) {
                if (underflow() == traits_type::eof()) return false;
            }
            const auto next = std::min(count, unread());
            gbump(static_cast<int>(next));
            count -= next;
        }
        return true;
    }

    bool skip_cstring()
    {
        while (true) {
            if (gptr() == nullptr || gptr() == egptr()) {
                if (underflow() == traits_type::eof()) return false;
            }
            const auto count = unread();
            const auto* end = static_cast<const char*>(std::memchr(gptr(), 0, count));
            if (end != nullptr) {
                gbump(static_cast<int>(end - gptr()) + 1);
                return true;
            }
            gbump(static_cast<int>(count));
        }
    }

    std::span<const std::uint8_t> contiguous() const noexcept
    {
        if (gptr() == nullptr || gptr() == egptr()) return {};
        return {
            reinterpret_cast<const std::uint8_t*>(gptr()),
            static_cast<std::size_t>(egptr() - gptr())
        };
    }

    std::span<const std::uint8_t> refill_contiguous()
    {
        if ((gptr() == nullptr || gptr() == egptr()) &&
            underflow() == traits_type::eof()) return {};
        return contiguous();
    }

    void consume(std::size_t count) noexcept
    {
        gbump(static_cast<int>(count));
    }

protected:
    int_type underflow() override
    {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (mFinished || !mError.empty()) return traits_type::eof();

        while (true) {
            if (mAvailableInput == 0 && !mInputFinished) {
                const auto input_start = mProfile
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                mInput.read(
                    reinterpret_cast<char*>(mInputBuffer.data()),
                    static_cast<std::streamsize>(mInputBuffer.size()));
                if (mProfile) {
                    mProfile->input_duration += std::chrono::steady_clock::now() - input_start;
                    ++mProfile->input_calls;
                }
                const auto count = mInput.gcount();
                if (count > 0) {
                    mNextInput = mInputBuffer.data();
                    mAvailableInput = static_cast<std::size_t>(count);
                } else {
                    mInputFinished = true;
                    if (!mInput.eof()) {
                        mError = "读取 BDX Brotli 输入失败";
                        return traits_type::eof();
                    }
                }
            }

            auto available_output = mOutputBuffer.size();
            auto* next_output = mOutputBuffer.data();
            const auto decode_start = mProfile
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const auto status = BrotliDecoderDecompressStream(
                mDecoder.get(),
                &mAvailableInput,
                &mNextInput,
                &available_output,
                &next_output,
                nullptr);
            if (mProfile) {
                mProfile->decode_duration += std::chrono::steady_clock::now() - decode_start;
                ++mProfile->decode_calls;
            }
            const auto produced = mOutputBuffer.size() - available_output;
            if (produced > mOutputLimit - std::min(mOutputLimit, mProduced)) {
                mError = "BDX Brotli 解压输出超过限制 " +
                    std::to_string(mOutputLimit) + " bytes";
                return traits_type::eof();
            }
            mProduced += produced;
            if (status == BROTLI_DECODER_RESULT_SUCCESS) mFinished = true;
            if (produced != 0) {
                auto* begin = reinterpret_cast<char*>(mOutputBuffer.data());
                setg(begin, begin, begin + produced);
                return traits_type::to_int_type(*gptr());
            }

            if (status == BROTLI_DECODER_RESULT_SUCCESS) return traits_type::eof();
            if (status == BROTLI_DECODER_RESULT_ERROR) {
                const auto code = BrotliDecoderGetErrorCode(mDecoder.get());
                const auto* message = BrotliDecoderErrorString(code);
                mError = std::string("BDX Brotli 解压失败: ") +
                    (message ? message : "unknown error");
                return traits_type::eof();
            }
            if (status == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT &&
                mAvailableInput == 0 && mInputFinished) {
                mError = "BDX Brotli 数据截断";
                return traits_type::eof();
            }
        }
    }

private:
    using Decoder = std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>;

    std::istream& mInput;
    std::size_t mOutputLimit = 0;
    BrotliProfile* mProfile = nullptr;
    Decoder mDecoder;
    std::vector<std::uint8_t> mInputBuffer =
        std::vector<std::uint8_t>(kBrotliBufferSize);
    std::vector<std::uint8_t> mOutputBuffer =
        std::vector<std::uint8_t>(kBrotliBufferSize);
    const std::uint8_t* mNextInput = nullptr;
    std::size_t mAvailableInput = 0;
    std::size_t mProduced = 0;
    bool mInputFinished = false;
    bool mFinished = false;
    std::string mError;
};

class DecodedInput final {
public:
    DecodedInput(
        std::istream& compressed,
        std::size_t output_limit,
        BrotliProfile* profile = nullptr)
        : mBuffer(compressed, output_limit, profile),
          mStream(&mBuffer)
    {
    }

    std::size_t offset() const noexcept { return mBuffer.offset(); }
    std::istream& stream() noexcept { return mStream; }

    std::optional<std::uint8_t> try_u8()
    {
        const auto value = mBuffer.try_u8();
        if (value) return value;
        if (!mBuffer.error().empty()) throw std::runtime_error(mBuffer.error());
        return std::nullopt;
    }

    std::uint8_t u8(const char* field)
    {
        const auto value = try_u8();
        if (!value) throw std::runtime_error(std::string(field) + " truncated");
        return *value;
    }

    std::int8_t i8(const char* field)
    {
        return static_cast<std::int8_t>(u8(field));
    }

    std::uint16_t u16(const char* field)
    {
        std::array<std::uint8_t, 2> bytes{};
        read_exact(bytes, field);
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[0]) << 8u | bytes[1]);
    }

    std::uint16_t u16_le(const char* field)
    {
        std::array<std::uint8_t, 2> bytes{};
        read_exact(bytes, field);
        return static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(bytes[1]) << 8u;
    }

    std::int16_t i16(const char* field)
    {
        return static_cast<std::int16_t>(u16(field));
    }

    std::uint32_t u32(const char* field)
    {
        std::array<std::uint8_t, 4> bytes{};
        read_exact(bytes, field);
        return static_cast<std::uint32_t>(bytes[0]) << 24u |
            static_cast<std::uint32_t>(bytes[1]) << 16u |
            static_cast<std::uint32_t>(bytes[2]) << 8u |
            bytes[3];
    }

    std::uint32_t u32_le(const char* field)
    {
        std::array<std::uint8_t, 4> bytes{};
        read_exact(bytes, field);
        return static_cast<std::uint32_t>(bytes[0]) |
            static_cast<std::uint32_t>(bytes[1]) << 8u |
            static_cast<std::uint32_t>(bytes[2]) << 16u |
            static_cast<std::uint32_t>(bytes[3]) << 24u;
    }

    std::int32_t i32(const char* field)
    {
        return static_cast<std::int32_t>(u32(field));
    }

    std::string cstring(const char* field)
    {
        std::string value;
        while (true) {
            const auto character = try_u8();
            if (!character) throw std::runtime_error(std::string(field) + " truncated");
            if (*character == 0) return value;
            value.push_back(static_cast<char>(*character));
        }
    }

    void skip_cstring(const char* field)
    {
        if (mBuffer.skip_cstring()) return;
        if (!mBuffer.error().empty()) throw std::runtime_error(mBuffer.error());
        throw std::runtime_error(std::string(field) + " truncated");
    }

    void skip(std::size_t count, const char* field)
    {
        if (mBuffer.skip(count)) return;
        if (!mBuffer.error().empty()) throw std::runtime_error(mBuffer.error());
        throw std::runtime_error(std::string(field) + " truncated");
    }

    void throw_decoder_error() const
    {
        if (!mBuffer.error().empty()) throw std::runtime_error(mBuffer.error());
    }

    std::span<const std::uint8_t> contiguous() const noexcept
    {
        return mBuffer.contiguous();
    }

    std::span<const std::uint8_t> refill_contiguous()
    {
        const auto bytes = mBuffer.refill_contiguous();
        if (bytes.empty() && !mBuffer.error().empty()) {
            throw std::runtime_error(mBuffer.error());
        }
        return bytes;
    }

    void consume(std::size_t count) noexcept
    {
        mBuffer.consume(count);
    }

private:
    void read_exact(std::span<std::uint8_t> output, const char* field)
    {
        if (mBuffer.read_exact(output)) return;
        if (!mBuffer.error().empty()) throw std::runtime_error(mBuffer.error());
        throw std::runtime_error(std::string(field) + " truncated");
    }

    BrotliStreamBuffer mBuffer;
    std::istream mStream;
};

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string unquote(std::string value)
{
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return value;
    std::string result;
    result.reserve(value.size() - 2);
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\' && i + 2 < value.size()) ++i;
        result.push_back(value[i]);
    }
    return result;
}

std::vector<BlockStateProperty> parse_block_states(std::string_view encoded)
{
    const auto open = encoded.find_first_of("[{");
    const auto close = encoded.find_last_of("]}");
    if (open == std::string_view::npos) return {};
    const auto contents = encoded.substr(open + 1,
        (close == std::string_view::npos ? encoded.size() : close) - open - 1);
    std::vector<BlockStateProperty> result;
    std::size_t start = 0;
    bool quoted = false;
    char separator_mode = 0;
    for (std::size_t index = 0; index <= contents.size(); ++index) {
        if (index < contents.size() && contents[index] == '"' &&
            (index == 0 || contents[index - 1] != '\\')) quoted = !quoted;
        if (index < contents.size() && (contents[index] != ',' || quoted)) continue;
        const auto part = contents.substr(start, index - start);
        std::size_t separator = std::string_view::npos;
        bool key_quoted = false;
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (part[i] == '"' && (i == 0 || part[i - 1] != '\\')) key_quoted = !key_quoted;
            if ((part[i] == '=' || part[i] == ':') && !key_quoted) {
                separator = i;
                break;
            }
        }
        if (separator == std::string_view::npos) {
            if (!trim(part).empty()) throw std::runtime_error("BDX block states 缺少 ':' 或 '='");
        } else {
            if (separator_mode == 0) separator_mode = part[separator];
            else if (separator_mode != part[separator]) {
                throw std::runtime_error("BDX block states 混用了 ':' 和 '='");
            }
            BlockStateProperty property;
            property.name = unquote(trim(part.substr(0, separator)));
            auto value = trim(part.substr(separator + 1));
            auto boolean = value;
            std::transform(boolean.begin(), boolean.end(), boolean.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (boolean == "true" || boolean == "false") {
                property.type = BlockStateValueType::Byte;
                property.value = boolean == "true" ? "1" : "0";
            } else if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                property.type = BlockStateValueType::String;
                property.value = unquote(std::move(value));
            } else {
                std::int32_t integer = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), integer);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                    throw std::runtime_error("BDX block state 值无效: " + value);
                }
                property.type = BlockStateValueType::Int;
                property.value = std::to_string(integer);
            }
            result.push_back(std::move(property));
        }
        start = index + 1;
    }
    return result;
}

struct CommandBlockData {
    std::uint32_t mode = 0;
    std::string command;
    std::string custom_name;
    std::string last_output;
    std::int32_t tick_delay = 0;
    bool execute_on_first_tick = false;
    bool track_output = false;
    bool conditional = false;
    bool needs_redstone = false;
};

CommandBlockData read_command_block_data(DecodedInput& input)
{
    CommandBlockData result;
    result.mode = input.u32("command block mode");
    result.command = input.cstring("command block command");
    result.custom_name = input.cstring("command block custom name");
    result.last_output = input.cstring("command block last output");
    result.tick_delay = input.i32("command block tick delay");
    result.execute_on_first_tick = input.u8("command block execute flag") != 0;
    result.track_output = input.u8("command block track flag") != 0;
    result.conditional = input.u8("command block conditional flag") != 0;
    result.needs_redstone = input.u8("command block redstone flag") != 0;
    return result;
}

void skip_command_block_data(DecodedInput& input)
{
    input.skip(4, "command block mode");
    input.skip_cstring("command block command");
    input.skip_cstring("command block custom name");
    input.skip_cstring("command block last output");
    input.skip(8, "command block data");
}

std::unique_ptr<nbt::tag_compound> command_block_nbt(const CommandBlockData& data)
{
    auto result = std::make_unique<nbt::tag_compound>();
    result->operator[]("Command") = nbt::tag_string(data.command);
    result->operator[]("CustomName") = nbt::tag_string(data.custom_name);
    result->operator[]("LastOutput") = nbt::tag_string(data.last_output);
    result->operator[]("TickDelay") = nbt::tag_int(data.tick_delay);
    result->operator[]("ExecuteOnFirstTick") = nbt::tag_byte(data.execute_on_first_tick ? 1 : 0);
    result->operator[]("TrackOutput") = nbt::tag_byte(data.track_output ? 1 : 0);
    result->operator[]("conditionalMode") = nbt::tag_byte(data.conditional ? 1 : 0);
    result->operator[]("auto") = nbt::tag_byte(data.needs_redstone ? 0 : 1);
    result->operator[]("id") = nbt::tag_string("CommandBlock");
    return result;
}

std::unique_ptr<nbt::tag_compound> read_chest_nbt(
    DecodedInput& input)
{
    const auto count = input.u8("chest slot count");
    nbt::tag_list items(nbt::tag_type::Compound);
    for (std::uint16_t i = 0; i < count; ++i) {
        nbt::tag_compound item;
        item["Name"] = nbt::tag_string(input.cstring("chest item name"));
        item["Count"] = nbt::tag_byte(input.u8("chest item count"));
        item["Damage"] = nbt::tag_short(static_cast<std::int16_t>(input.u16("chest item damage")));
        item["Slot"] = nbt::tag_byte(input.u8("chest item slot"));
        items.push_back(std::move(item));
    }
    auto result = std::make_unique<nbt::tag_compound>();
    result->operator[]("Items") = std::move(items);
    return result;
}

void skip_chest_nbt(DecodedInput& input)
{
    const auto count = input.u8("chest slot count");
    for (std::uint16_t i = 0; i < count; ++i) {
        input.skip_cstring("chest item name");
        input.skip(4, "chest item data");
    }
}

void skip_nbt_payload(DecodedInput& input, std::uint8_t type, unsigned depth)
{
    if (depth > 128) throw std::runtime_error("BDX NBT nesting too deep");
    switch (type) {
    case 1: input.skip(1, "NBT byte"); return;
    case 2: input.skip(2, "NBT short"); return;
    case 3: input.skip(4, "NBT int"); return;
    case 4: input.skip(8, "NBT long"); return;
    case 5: input.skip(4, "NBT float"); return;
    case 6: input.skip(8, "NBT double"); return;
    case 7: input.skip(input.u32_le("NBT byte array length"), "NBT byte array"); return;
    case 8: input.skip(input.u16_le("NBT string length"), "NBT string"); return;
    case 9: {
        const auto element_type = input.u8("NBT list type");
        const auto count = input.u32_le("NBT list length");
        for (std::uint32_t i = 0; i < count; ++i) skip_nbt_payload(input, element_type, depth + 1);
        return;
    }
    case 10: {
        while (true) {
            const auto element_type = input.u8("NBT compound type");
            if (element_type == 0) return;
            input.skip(input.u16_le("NBT compound key length"), "NBT compound key");
            skip_nbt_payload(input, element_type, depth + 1);
        }
    }
    case 11: input.skip(input.u32_le("NBT int array length") * 4ull, "NBT int array"); return;
    case 12: input.skip(input.u32_le("NBT long array length") * 8ull, "NBT long array"); return;
    default: throw std::runtime_error("BDX NBT type invalid: " + std::to_string(type));
    }
}

void skip_nbt_compound(DecodedInput& input)
{
    const auto type = input.u8("NBT root type");
    if (type != 10) throw std::runtime_error("BDX NBT root is not compound");
    input.skip(input.u16_le("NBT root name length"), "NBT root name");
    skip_nbt_payload(input, type, 0);
}

std::unique_ptr<nbt::tag_compound> read_block_nbt(
    DecodedInput& input)
{
    try {
        auto [_, compound] = nbt::io::read_compound(input.stream(), endian::little);
        input.throw_decoder_error();
        return std::move(compound);
    } catch (...) {
        input.throw_decoder_error();
        throw;
    }
}

NbtPayload serialize_compound(const nbt::tag_compound& compound)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", compound, output, endian::little);
    const auto bytes = output.str();
    return NbtPayload(bytes.begin(), bytes.end());
}

std::string block_entity_id(std::string_view block_name)
{
    if (block_name == "minecraft:blast_furnace" || block_name == "minecraft:lit_blast_furnace") return "BlastFurnace";
    if (block_name == "minecraft:furnace" || block_name == "minecraft:lit_furnace") return "Furnace";
    if (block_name == "minecraft:smoker" || block_name == "minecraft:lit_smoker") return "Smoker";
    if (block_name == "minecraft:chest" || block_name == "minecraft:trapped_chest") return "Chest";
    if (block_name == "minecraft:hopper") return "Hopper";
    if (block_name == "minecraft:dispenser") return "Dispenser";
    if (block_name == "minecraft:dropper") return "Dropper";
    if (block_name == "minecraft:barrel") return "Barrel";
    if (block_name == "minecraft:crafter") return "Crafter";
    if (block_name.find("shulker_box") != std::string_view::npos) return "ShulkerBox";
    return {};
}

} // namespace

void BdxStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mChunkIndex.clear();
    mSize = {
        mOriginalSize.width + std::abs(offset.x),
        mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z)
    };
}

Result<void> BdxStructure::read(const std::filesystem::path& path)
{
    const auto read_start = std::chrono::steady_clock::now();
    mSourcePath = path;
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 BDX 文件: " + path.string());
    try {
        std::array<char, 3> file_header{};
        input.read(file_header.data(), static_cast<std::streamsize>(file_header.size()));
        if (input.gcount() != static_cast<std::streamsize>(file_header.size()) ||
            std::string_view(file_header.data(), file_header.size()) != "BD@") {
            return Result<void>::failure("BDX 文件头不是 BD@");
        }
        std::error_code size_error;
        const auto file_size = std::filesystem::file_size(path, size_error);
        if (size_error || file_size < file_header.size()) {
            throw std::runtime_error("无法确定 BDX 文件大小: " + size_error.message());
        }
        const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr;
        const bool detail_profile =
            std::getenv("WATER_STRUCTURE_PROFILE_DETAIL") != nullptr;
        BrotliProfile brotli_profile;
        DecodedInput decoded(
            input,
            decoded_limit(file_size - file_header.size()),
            detail_profile ? &brotli_profile : nullptr);
        if (decoded.u8("metadata magic") != 'B' ||
            decoded.u8("metadata magic") != 'D' ||
            decoded.u8("metadata magic") != 'X') {
            return Result<void>::failure("BDX metadata 头不是 BDX");
        }
        mAuthor = decoded.cstring("BDX author");
        (void)decoded.u8("BDX version");

        std::vector<std::string> constants;
        constants.reserve(256);
        std::unordered_map<std::uint32_t, std::uint32_t> legacy_runtime_cache;
        std::unordered_map<std::uint32_t, std::uint32_t> state_runtime_cache;
        std::array<std::uint32_t, 256> state_direct_keys{};
        std::array<std::uint32_t, 256> state_direct_values{};
        std::array<bool, 256> state_direct_valid{};
        std::uint8_t cached_pool_id = std::numeric_limits<std::uint8_t>::max();
        std::vector<std::uint32_t> cached_pool;
        std::int32_t x = 0, y = 0, z = 0;
        std::int32_t min_x = 0, min_y = 0, min_z = 0;
        std::int32_t max_x = 0, max_y = 0, max_z = 0;
        std::uint64_t command_index = 0;
        std::uint64_t pool_placements = 0;
        std::uint64_t state_placements = 0;
        std::uint64_t legacy_placements = 0;
        std::uint64_t z_run_blocks = 0;
        std::uint64_t z_run_count = 0;
        std::uint64_t sampled_z_runs = 0;
        std::uint64_t sampled_z_blocks = 0;
        std::chrono::steady_clock::duration sampled_z_resolve_duration{};
        std::chrono::steady_clock::duration sampled_z_consumer_duration{};
        std::chrono::steady_clock::duration sampled_z_count_duration{};
        constexpr std::uint64_t kDetailedProfileSampleMask = 4095;
        std::array<std::uint64_t, 256> command_counts{};
        bool terminated = false;
        const auto profile_interval = std::chrono::seconds(5);
        auto next_profile = read_start + profile_interval;
        mPoolId = 0;
        mBlocks.clear();
        mBlockEntities.clear();
        mNonAirBlocks = 0;
        mBlocksLoaded = false;
        const auto air_runtime_id = mRegistry.air_runtime_id();

        auto constant = [&](std::uint16_t id) -> const std::string* {
            return id < constants.size() ? &constants[id] : nullptr;
        };
        auto legacy_runtime = [&](std::uint16_t name_id, std::uint16_t block_data) {
            const auto key = static_cast<std::uint32_t>(name_id) << 16u | block_data;
            if (const auto cached = legacy_runtime_cache.find(key);
                cached != legacy_runtime_cache.end()) return cached->second;
            const auto* name = constant(name_id);
            if (!name) return air_runtime_id;
            const auto runtime = mRegistry.legacy_runtime_id(*name, block_data)
                .value_or(mRegistry.find(*name).value_or(air_runtime_id));
            legacy_runtime_cache.emplace(key, runtime);
            return runtime;
        };
        auto resolve_state_runtime = [&](std::uint16_t name_id, std::string_view states) {
            const auto* name = constant(name_id);
            if (!name) return air_runtime_id;
            BlockState state;
            state.name = *name;
            state.states = parse_block_states(states);
            if (const auto runtime = mRegistry.legacy_state_runtime_id(state.name, state.states)) return *runtime;
            if (const auto runtime = mRegistry.find_compatible(std::move(state))) return *runtime;
            return mRegistry.legacy_runtime_id(*name, 0)
                .value_or(mRegistry.find(*name).value_or(air_runtime_id));
        };
        auto constant_state_runtime = [&](std::uint16_t name_id, std::uint16_t states_id) {
            const auto key = static_cast<std::uint32_t>(name_id) << 16u | states_id;
            const auto slot = static_cast<std::size_t>(key * 2654435761u) &
                (state_direct_keys.size() - 1);
            if (state_direct_valid[slot] && state_direct_keys[slot] == key) {
                return state_direct_values[slot];
            }
            if (const auto cached = state_runtime_cache.find(key);
                cached != state_runtime_cache.end()) {
                state_direct_valid[slot] = true;
                state_direct_keys[slot] = key;
                state_direct_values[slot] = cached->second;
                return cached->second;
            }
            const auto* encoded = constant(states_id);
            const auto runtime = encoded
                ? resolve_state_runtime(name_id, *encoded)
                : air_runtime_id;
            state_runtime_cache.emplace(key, runtime);
            state_direct_valid[slot] = true;
            state_direct_keys[slot] = key;
            state_direct_values[slot] = runtime;
            return runtime;
        };
        auto pool_runtime = [&](std::uint32_t index) {
            if (cached_pool_id != mPoolId) {
                cached_pool_id = mPoolId;
                cached_pool = mRegistry.legacy_pool_snapshot(mPoolId);
            }
            if (index >= cached_pool.size()) {
                throw std::runtime_error(
                    "runtime pool " + std::to_string(mPoolId) + " index " + std::to_string(index) + " 越界");
            }
            return cached_pool[index];
        };
        auto store_entity = [&](std::unique_ptr<nbt::tag_compound> entity, std::uint32_t runtime_id) {
            if (!entity || !mCaptureEntities) return;
            if (const auto state = mRegistry.state(runtime_id)) {
                if (const auto id = block_entity_id(state->name); !id.empty()) {
                    entity->operator[]("id") = nbt::tag_string(id);
                }
            }
            mBlockEntities[{ x, y, z }] = serialize_compound(*entity);
        };
        auto place = [&](std::uint32_t runtime_id, std::unique_ptr<nbt::tag_compound> entity) {
            store_entity(std::move(entity), runtime_id);
            if (runtime_id == air_runtime_id) return;
            if (mMaterializeBlocks) mBlocks.push_back({ x, y, z, runtime_id });
            if (mBlockConsumer) mBlockConsumer({ x, y, z }, runtime_id);
            ++mNonAirBlocks;
        };
        auto update_bounds = [&] {
            min_x = std::min(min_x, x); min_y = std::min(min_y, y); min_z = std::min(min_z, z);
            max_x = std::max(max_x, x); max_y = std::max(max_y, y); max_z = std::max(max_z, z);
        };
        auto report_progress = [&] {
            if (!profile || std::chrono::steady_clock::now() < next_profile) return;
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - read_start).count();
            std::cerr << "BDX progress: mode=" << (mBoundsOnly ? "bounds" : "full")
                      << " commands=" << command_index
                      << " decoded_bytes=" << decoded.offset()
                      << " blocks=" << mNonAirBlocks
                      << " pool=" << pool_placements
                      << " state=" << state_placements
                      << " legacy=" << legacy_placements
                      << " z_run_blocks=" << z_run_blocks
                      << " z_runs=" << z_run_count
                      << " ids=5:" << command_counts[5]
                      << ",7:" << command_counts[7]
                      << ",8:" << command_counts[8]
                      << ",9:" << command_counts[9]
                      << ",14:" << command_counts[14]
                      << ",16:" << command_counts[16]
                      << ",18:" << command_counts[18]
                      << ",19:" << command_counts[19]
                      << ",28:" << command_counts[28]
                      << ",29:" << command_counts[29]
                      << ",30:" << command_counts[30]
                      << ",32:" << command_counts[32]
                      << " elapsed_ms=" << elapsed_ms << '\n';
            next_profile = std::chrono::steady_clock::now() + profile_interval;
        };

        const auto signed_be16 = [](const std::uint8_t* bytes) {
            return static_cast<std::int16_t>(
                static_cast<std::uint16_t>(bytes[0]) << 8u | bytes[1]);
        };
        const auto signed_be32 = [](const std::uint8_t* bytes) {
            return static_cast<std::int32_t>(
                static_cast<std::uint32_t>(bytes[0]) << 24u |
                static_cast<std::uint32_t>(bytes[1]) << 16u |
                static_cast<std::uint32_t>(bytes[2]) << 8u |
                bytes[3]);
        };
        std::vector<std::uint32_t> z_run_runtimes;
        z_run_runtimes.reserve(kBrotliBufferSize / 6);

        while (true) {
            if (mBoundsOnly) {
                const auto bytes = decoded.refill_contiguous();
                std::size_t cursor = 0;
                while (cursor < bytes.size()) {
                    const auto id = bytes[cursor];
                    std::size_t length = 0;
                    bool metadata = false;
                    switch (id) {
                    case 5: case 7: length = 5; break;
                    case 6: case 20: case 22: case 24: length = 3; break;
                    case 8: case 9: case 14: case 15: case 16: case 17:
                    case 18: case 19: length = 1; break;
                    case 12: case 21: case 23: case 25: case 33: length = 5; break;
                    case 28: case 29: case 30: length = 2; break;
                    case 31: length = 2; metadata = true; break;
                    case 32: length = 3; break;
                    default: length = 0; break;
                    }
                    if (length == 0 || length > bytes.size() - cursor) break;

                    const auto* payload = bytes.data() + cursor + 1;
                    switch (id) {
                    case 6: z += signed_be16(payload); break;
                    case 8: ++z; break;
                    case 12: z += signed_be32(payload); break;
                    case 14: ++x; break;
                    case 15: --x; break;
                    case 16: ++y; break;
                    case 17: --y; break;
                    case 18: ++z; break;
                    case 19: --z; break;
                    case 20: x += signed_be16(payload); break;
                    case 21: x += signed_be32(payload); break;
                    case 22: y += signed_be16(payload); break;
                    case 23: y += signed_be32(payload); break;
                    case 24: z += signed_be16(payload); break;
                    case 25: z += signed_be32(payload); break;
                    case 28: x += static_cast<std::int8_t>(payload[0]); break;
                    case 29: y += static_cast<std::int8_t>(payload[0]); break;
                    case 30: z += static_cast<std::int8_t>(payload[0]); break;
                    case 31: mPoolId = payload[0]; break;
                    default: break;
                    }
                    ++command_index;
                    if (profile) ++command_counts[id];
                    if (!metadata) update_bounds();
                    cursor += length;
                }
                if (cursor != 0) {
                    decoded.consume(cursor);
                    report_progress();
                    continue;
                }
            }

            if (!mBoundsOnly) {
                const auto bytes = decoded.refill_contiguous();
                std::size_t cursor = 0;
                std::size_t pairs = 0;
                while (bytes.size() - cursor >= 6 &&
                    bytes[cursor] == 5 && bytes[cursor + 5] == 18) {
                    cursor += 6;
                    ++pairs;
                }
                if (pairs >= 4) {
                    const bool sample_run = detail_profile &&
                        ((z_run_count & kDetailedProfileSampleMask) == 0);
                    const auto resolve_start = sample_run
                        ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};
                    z_run_runtimes.clear();
                    z_run_runtimes.reserve(pairs);
                    bool z_run_contains_air = false;
                    const auto run_start_command = command_index + 1;
                    const auto run_start_offset = decoded.offset();
                    for (std::size_t pair = 0; pair < pairs; ++pair) {
                        const auto* payload = bytes.data() + pair * 6 + 1;
                        const auto name = static_cast<std::uint16_t>(
                            static_cast<std::uint16_t>(payload[0]) << 8u | payload[1]);
                        const auto states = static_cast<std::uint16_t>(
                            static_cast<std::uint16_t>(payload[2]) << 8u | payload[3]);
                        try {
                            const auto runtime_id = constant_state_runtime(name, states);
                            z_run_runtimes.push_back(runtime_id);
                            if (runtime_id != air_runtime_id) {
                                ++mNonAirBlocks;
                            } else {
                                z_run_contains_air = true;
                            }
                        } catch (const std::exception& error) {
                            throw std::runtime_error(
                                "command #" + std::to_string(run_start_command + pair * 2) +
                                " decoded offset " + std::to_string(run_start_offset + pair * 6) +
                                ": " + error.what());
                        }
                    }
                    if (sample_run) {
                        sampled_z_resolve_duration +=
                            std::chrono::steady_clock::now() - resolve_start;
                    }

                    const BlockPos run_start{ x, y, z };
                    if (mMaterializeBlocks) {
                        for (std::size_t index = 0; index < z_run_runtimes.size(); ++index) {
                            const auto runtime_id = z_run_runtimes[index];
                            if (runtime_id != air_runtime_id) {
                                mBlocks.push_back({ x, y, z + static_cast<std::int32_t>(index), runtime_id });
                            }
                        }
                    }
                    if (mZRunConsumer) {
                        const auto consumer_start = sample_run
                            ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
                        mZRunConsumer(run_start, z_run_runtimes, z_run_contains_air);
                        if (sample_run) {
                            sampled_z_consumer_duration +=
                                std::chrono::steady_clock::now() - consumer_start;
                        }
                    } else if (mBlockConsumer) {
                        for (std::size_t index = 0; index < z_run_runtimes.size(); ++index) {
                            const auto runtime_id = z_run_runtimes[index];
                            if (runtime_id != air_runtime_id) {
                                mBlockConsumer(
                                    { x, y, z + static_cast<std::int32_t>(index) }, runtime_id);
                            }
                        }
                    }
                    if (sample_run) {
                        ++sampled_z_runs;
                        sampled_z_blocks += pairs;
                    }
                    update_bounds();
                    z += static_cast<std::int32_t>(pairs);
                    update_bounds();
                    command_index += pairs * 2;
                    state_placements += pairs;
                    z_run_blocks += pairs;
                    ++z_run_count;
                    if (profile) {
                        command_counts[5] += pairs;
                        command_counts[18] += pairs;
                    }
                    decoded.consume(cursor);
                    report_progress();
                    continue;
                }
            }

            const auto command_offset = decoded.offset();
            const auto command_id = decoded.try_u8();
            if (!command_id) break;
            ++command_index;
            if (profile) ++command_counts[*command_id];
            bool metadata = false;
            try {
                switch (*command_id) {
                case 1: {
                    if (constants.size() >=
                        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1) {
                        throw std::runtime_error("constant string 表超过 uint16 范围");
                    }
                    if (mBoundsOnly) decoded.skip_cstring("constant string");
                    else constants.push_back(decoded.cstring("constant string"));
                    metadata = true;
                    break;
                }
                case 5: {
                    if (mBoundsOnly) {
                        decoded.skip(4, "constant block state");
                    } else {
                        const auto name = decoded.u16("block constant id");
                        const auto states = decoded.u16("states constant id");
                        ++state_placements;
                        place(constant_state_runtime(name, states), nullptr);
                    }
                    break;
                }
                case 6: z += decoded.i16("int16 z"); break;
                case 7: {
                    if (mBoundsOnly) {
                        decoded.skip(4, "legacy block");
                    } else {
                        const auto name = decoded.u16("block constant id");
                        const auto data = decoded.u16("legacy block data");
                        ++legacy_placements;
                        place(legacy_runtime(name, data), nullptr);
                    }
                    break;
                }
                case 8: ++z; break;
                case 9: break;
                case 12: z += decoded.i32("int32 z"); break;
                case 13: {
                    if (mBoundsOnly) {
                        decoded.skip(2, "block constant id");
                        decoded.skip_cstring("deprecated block states");
                    } else {
                        const auto name = decoded.u16("block constant id");
                        const auto states = decoded.cstring("deprecated block states");
                        ++state_placements;
                        place(resolve_state_runtime(name, states), nullptr);
                    }
                    break;
                }
                case 14: ++x; break;
                case 15: --x; break;
                case 16: ++y; break;
                case 17: --y; break;
                case 18: ++z; break;
                case 19: --z; break;
                case 20: x += decoded.i16("int16 x"); break;
                case 21: x += decoded.i32("int32 x"); break;
                case 22: y += decoded.i16("int16 y"); break;
                case 23: y += decoded.i32("int32 y"); break;
                case 24: z += decoded.i16("int16 z"); break;
                case 25: z += decoded.i32("int32 z"); break;
                case 26:
                    if (mBoundsOnly) skip_command_block_data(decoded);
                    else store_entity(command_block_nbt(read_command_block_data(decoded)), 0);
                    break;
                case 27: {
                    if (mBoundsOnly) {
                        decoded.skip(4, "legacy command block");
                        skip_command_block_data(decoded);
                    } else {
                        const auto name = decoded.u16("block constant id");
                        const auto data = decoded.u16("legacy block data");
                        place(legacy_runtime(name, data), command_block_nbt(read_command_block_data(decoded)));
                    }
                    break;
                }
                case 28: x += decoded.i8("int8 x"); break;
                case 29: y += decoded.i8("int8 y"); break;
                case 30: z += decoded.i8("int8 z"); break;
                case 31: mPoolId = decoded.u8("runtime pool id"); metadata = true; break;
                case 32: {
                    if (mBoundsOnly) {
                        decoded.skip(2, "runtime pool index");
                    } else {
                        const auto index = decoded.u16("runtime pool index");
                        ++pool_placements;
                        place(pool_runtime(index), nullptr);
                    }
                    break;
                }
                case 33: (void)decoded.u32("runtime pool index"); break;
                case 34: {
                    if (mBoundsOnly) {
                        decoded.skip(2, "runtime pool index");
                        skip_command_block_data(decoded);
                    } else {
                        const auto index = decoded.u16("runtime pool index");
                        ++pool_placements;
                        place(pool_runtime(index), command_block_nbt(read_command_block_data(decoded)));
                    }
                    break;
                }
                case 35: {
                    if (mBoundsOnly) {
                        decoded.skip(4, "runtime pool index");
                        skip_command_block_data(decoded);
                    } else {
                        const auto index = decoded.u32("runtime pool index");
                        ++pool_placements;
                        place(pool_runtime(index), command_block_nbt(read_command_block_data(decoded)));
                    }
                    break;
                }
                case 36: {
                    if (mBoundsOnly) {
                        decoded.skip(2, "command block data");
                        skip_command_block_data(decoded);
                        break;
                    }
                    const auto data = decoded.u16("command block data");
                    auto command = read_command_block_data(decoded);
                    if (command.mode > 2) throw std::runtime_error("command block mode 越界");
                    static constexpr std::array<std::string_view, 3> names{
                        "minecraft:command_block", "minecraft:repeating_command_block", "minecraft:chain_command_block"
                    };
                    const auto runtime = mRegistry.legacy_runtime_id(names[command.mode], data)
                        .value_or(mRegistry.find(names[command.mode]).value_or(mRegistry.air_runtime_id()));
                    ++legacy_placements;
                    place(runtime, command_block_nbt(command));
                    break;
                }
                case 37: {
                    if (mBoundsOnly) {
                        decoded.skip(2, "runtime pool index");
                        skip_chest_nbt(decoded);
                    } else {
                        const auto index = decoded.u16("runtime pool index");
                        ++pool_placements;
                        place(pool_runtime(index), read_chest_nbt(decoded));
                    }
                    break;
                }
                case 38: {
                    if (mBoundsOnly) {
                        decoded.skip(4, "runtime pool index");
                        skip_chest_nbt(decoded);
                    } else {
                        const auto index = decoded.u32("runtime pool index");
                        ++pool_placements;
                        place(pool_runtime(index), read_chest_nbt(decoded));
                    }
                    break;
                }
                case 39: decoded.skip(decoded.u32("debug data length"), "debug data"); break;
                case 40: {
                    if (mBoundsOnly) {
                        decoded.skip(4, "legacy chest block");
                        skip_chest_nbt(decoded);
                    } else {
                        const auto name = decoded.u16("block constant id");
                        const auto data = decoded.u16("legacy block data");
                        ++legacy_placements;
                        place(legacy_runtime(name, data), read_chest_nbt(decoded));
                    }
                    break;
                }
                case 41: {
                    if (mBoundsOnly) {
                        decoded.skip(6, "NBT block header");
                        skip_nbt_compound(decoded);
                    } else {
                        const auto name = decoded.u16("block constant id");
                        const auto states = decoded.u16("states constant id");
                        (void)decoded.u16("NBT prefix");
                        ++state_placements;
                        place(constant_state_runtime(name, states), read_block_nbt(decoded));
                    }
                    break;
                }
                case 88: terminated = true; break;
                default: throw std::runtime_error("未知 command id=" + std::to_string(*command_id));
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    "command #" + std::to_string(command_index) + " decoded offset " +
                    std::to_string(command_offset) + ": " + error.what());
            }
            if (!metadata) update_bounds();
            report_progress();
            if (terminated) break;
        }
        if (!terminated) {
            throw std::runtime_error(
                "缺少 terminate command，最后 command #" + std::to_string(command_index) +
                " decoded offset " + std::to_string(decoded.offset()));
        }

        for (auto& block : mBlocks) {
            block.x -= min_x;
            block.y -= min_y;
            block.z -= min_z;
        }
        decltype(mBlockEntities) shifted_entities;
        shifted_entities.reserve(mBlockEntities.size());
        for (auto& [pos, payload] : mBlockEntities) {
            shifted_entities.emplace(
                BlockPos{ pos.x - min_x, pos.y - min_y, pos.z - min_z },
                std::move(payload));
        }
        mBlockEntities = std::move(shifted_entities);
        mMin = { min_x, min_y, min_z };
        mOriginalSize = { max_x - min_x + 1, max_y - min_y + 1, max_z - min_z + 1 };
        mBlocksLoaded = mMaterializeBlocks;
        set_offset({});
        if (std::getenv("WATER_STRUCTURE_PROFILE")) {
            const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - read_start).count();
            std::cerr << "BDX read: mode=" << (mBoundsOnly ? "bounds" : "full")
                      << " commands=" << command_index
                      << " decoded_bytes=" << decoded.offset()
                      << " read_ms=" << read_ms
                      << " z_run_blocks=" << z_run_blocks
                      << " z_runs=" << z_run_count;
            if (profile) {
                std::cerr << " command_counts=";
                bool first = true;
                for (std::size_t id = 0; id < command_counts.size(); ++id) {
                    if (command_counts[id] == 0) continue;
                    if (!first) std::cerr << ',';
                    first = false;
                    std::cerr << id << ':' << command_counts[id];
                }
            }
            std::cerr << '\n';
            if (detail_profile) {
                const auto to_microseconds = [](const auto duration) {
                    return std::chrono::duration_cast<std::chrono::microseconds>(
                        duration).count();
                };
                std::cerr << "BDX detail: mode=" << (mBoundsOnly ? "bounds" : "full")
                          << " brotli_input_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 brotli_profile.input_duration).count()
                          << " brotli_decode_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 brotli_profile.decode_duration).count()
                          << " brotli_input_calls=" << brotli_profile.input_calls
                          << " brotli_decode_calls=" << brotli_profile.decode_calls
                          << " sampled_z_runs=" << sampled_z_runs
                          << " sampled_z_blocks=" << sampled_z_blocks
                          << " sampled_z_resolve_us="
                          << to_microseconds(sampled_z_resolve_duration)
                          << " sampled_z_consumer_us="
                          << to_microseconds(sampled_z_consumer_duration)
                          << " sampled_z_count_us="
                          << to_microseconds(sampled_z_count_duration)
                          << '\n';
            }
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 BDX 失败: ") + error.what());
    }
}

Result<void> BdxStructure::ensure_blocks_loaded() const
{
    if (mBlocksLoaded) return Result<void>::success();
    if (mSourcePath.empty()) return Result<void>::failure("BDX 源文件路径为空");

    BdxStructure loaded(mRegistry);
    loaded.mMaterializeBlocks = true;
    loaded.mCaptureEntities = false;
    auto parsed = loaded.read(mSourcePath);
    if (!parsed) return parsed;
    mBlocks = std::move(loaded.mBlocks);
    mBlocksLoaded = true;
    mChunkIndex.clear();
    return Result<void>::success();
}

Result<ChunkMap> BdxStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    if (const auto loaded = ensure_blocks_loaded(); !loaded) {
        return Result<ChunkMap>::failure(loaded.error());
    }
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    if (!mChunkIndex.ensure(mBlocks, mOffset, [](const Block& block) {
        return BlockPos{ block.x, block.y, block.z };
    })) return Result<ChunkMap>::failure("BDX chunk index 超过 uint32 容量");
    for (auto& [chunk_pos, chunk] : result) {
        const auto* indexed = mChunkIndex.find(chunk_pos);
        if (!indexed) continue;
        for (const auto index : *indexed) {
            const auto& block = mBlocks[index];
            const auto x = block.x + mOffset.x;
            const auto y = block.y + mOffset.y;
            const auto z = block.z + mOffset.z;
            const auto sub_y = floor_div(y - 64, 16);
            auto [sub, inserted] = chunk.sub_chunks.try_emplace(sub_y);
            if (inserted) {
                sub->second.layer0.fill(mRegistry.air_runtime_id());
                sub->second.layer1.fill(mRegistry.air_runtime_id());
            }
            const auto local_x = x - chunk_pos.x * 16;
            const auto local_y = y - (sub_y * 16 + 64);
            const auto local_z = z - chunk_pos.z * 16;
            sub->second.layer0[static_cast<std::size_t>(
                (local_y * 16 + local_z) * 16 + local_x)] = block.runtime_id;
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> BdxStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    for (const auto& [source, payload] : mBlockEntities) {
        const BlockPos position{ source.x + mOffset.x, source.y + mOffset.y, source.z + mOffset.z };
        if (position.y < 0 || position.y >= mSize.height) continue;
        const auto chunk = block_to_chunk(position);
        const auto found = result.find(chunk);
        if (found == result.end()) continue;
        found->second.push_back({
            {
                floor_mod(position.x, 16),
                structure_y_to_chunk_local(position.y),
                floor_mod(position.z, 16)
            },
            payload
        });
    }
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> BdxStructure::write_to_world(
    WorldTarget& world,
    SubChunkPos start,
    ConversionCallbacks callbacks) const
{
    const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr;
    const auto stream_start = std::chrono::steady_clock::now();
    auto* adapter = dynamic_cast<BedrockWorldAdapter*>(&world);
    if (adapter == nullptr || mSourcePath.empty()) {
        return convert_to_world(*this, world, start, std::move(callbacks));
    }

    // Keep chunk ownership/LRU cheap, but track persisted data per subchunk.
    // A revisited layer reloads only its 16x16x16 payload rather than every Y
    // layer in the chunk. This preserves bounded memory for arbitrary order.
    const auto chunk_cache_limit = [] {
        constexpr std::size_t default_limit = 64;
        const auto* value = std::getenv("WATER_STRUCTURE_BDX_CHUNK_CACHE");
        if (value == nullptr || *value == '\0') return default_limit;
        std::size_t parsed = 0;
        const auto text = std::string_view(value);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            return default_limit;
        }
        return std::clamp<std::size_t>(parsed, 64, 512);
    }();
    const auto cold_cache_limit = [] {
        constexpr std::size_t default_mebibytes = 64;
        const auto* value = std::getenv("WATER_STRUCTURE_BDX_COLD_CACHE_MB");
        if (value == nullptr || *value == '\0') {
            return default_mebibytes * 1024 * 1024;
        }
        std::size_t parsed = 0;
        const auto text = std::string_view(value);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            return default_mebibytes * 1024 * 1024;
        }
        return std::min<std::size_t>(parsed, 1024) * 1024 * 1024;
    }();
    constexpr std::int32_t kMinCachedSubY = kOverworldMinY / 16;
    constexpr std::size_t kCachedSubYCount = (320 - kOverworldMinY) / 16;
    struct CachedChunk {
        ChunkData data;
        std::array<SubChunkData*, kCachedSubYCount> layers{};
        std::uint64_t last_touch = 0;
    };
    struct ChunkLookasideEntry {
        ChunkPos position{};
        CachedChunk* chunk = nullptr;
    };
    struct SubChunkLookasideEntry {
        SubChunkPos position{};
        CachedChunk* owner = nullptr;
        SubChunkData* subchunk = nullptr;
    };
    struct ColdPayload {
        std::vector<std::uint8_t> payload;
        std::size_t charge = 0;
        std::list<SubChunkPos>::iterator lru;
    };

    std::unordered_map<ChunkPos, CachedChunk, ChunkPosHash> cache;
    cache.reserve(chunk_cache_limit);
    std::unordered_set<SubChunkPos, SubChunkPosHash> flushed;
    std::uint64_t touch_clock = 0;
    std::array<ChunkLookasideEntry, 256> chunk_lookaside{};
    std::array<SubChunkLookasideEntry, 256> subchunk_lookaside{};
    std::unordered_map<SubChunkPos, ColdPayload, SubChunkPosHash> cold_cache;
    std::list<SubChunkPos> cold_lru;
    std::size_t cold_cache_bytes = 0;
    std::size_t cold_cache_peak_bytes = 0;
    std::size_t cold_cache_hits = 0;
    std::size_t cold_cache_misses = 0;
    std::size_t cold_cache_evictions = 0;

    const auto erase_cold = [&](auto found) {
        cold_cache_bytes -= found->second.charge;
        cold_lru.erase(found->second.lru);
        cold_cache.erase(found);
    };
    const auto insert_cold = [&](
        EncodedSubChunkData encoded,
        std::vector<EncodedSubChunkData>& evicted) {
        if (encoded.payload.empty()) return;
        if (cold_cache_limit == 0) {
            evicted.push_back(std::move(encoded));
            return;
        }
        if (auto found = cold_cache.find(encoded.pos); found != cold_cache.end()) {
            erase_cold(found);
        }
        constexpr std::size_t entry_overhead = 128;
        const auto charge = encoded.payload.capacity() + entry_overhead;
        if (charge > cold_cache_limit) {
            evicted.push_back(std::move(encoded));
            return;
        }
        cold_lru.push_front(encoded.pos);
        cold_cache_bytes += charge;
        cold_cache.emplace(encoded.pos, ColdPayload{
            std::move(encoded.payload), charge, cold_lru.begin() });
        while (cold_cache_bytes > cold_cache_limit && !cold_lru.empty()) {
            const auto oldest = cold_lru.back();
            const auto found = cold_cache.find(oldest);
            if (found == cold_cache.end()) {
                cold_lru.pop_back();
                continue;
            }
            evicted.push_back({ oldest, std::move(found->second.payload) });
            erase_cold(found);
            ++cold_cache_evictions;
        }
        cold_cache_peak_bytes = std::max(cold_cache_peak_bytes, cold_cache_bytes);
    };

    constexpr std::size_t kFlushBatchSize = 16;
    std::size_t save_batches = 0;
    std::size_t reload_count = 0;
    std::chrono::steady_clock::duration save_duration{};
    std::chrono::steady_clock::duration encode_duration{};
    std::chrono::steady_clock::duration database_save_duration{};
    std::chrono::steady_clock::duration save_wait_duration{};
    std::chrono::steady_clock::duration reload_duration{};
    std::chrono::steady_clock::duration database_load_duration{};
    std::chrono::steady_clock::duration decode_duration{};
    const bool detail_profile =
        std::getenv("WATER_STRUCTURE_PROFILE_DETAIL") != nullptr;
    if (detail_profile) {
        BedrockWorldOperator::resetSubChunkDecodeProfile();
        BedrockWorldOperator::resetSubChunkEncodeProfile();
    }
    struct CacheDetailProfile {
        std::uint64_t acquire_calls = 0;
        std::uint64_t chunk_hits = 0;
        std::uint64_t chunk_misses = 0;
        std::uint64_t chunk_lookaside_hits = 0;
        std::uint64_t chunk_map_hits = 0;
        std::uint64_t lru_already_front = 0;
        std::uint64_t lru_splices = 0;
        std::uint64_t pending_waits = 0;
        std::uint64_t capacity_flushes = 0;
        std::uint64_t subchunk_hits = 0;
        std::uint64_t subchunk_lookaside_hits = 0;
        std::uint64_t subchunk_misses = 0;
        std::uint64_t fresh_subchunks = 0;
        std::uint64_t reload_subchunks = 0;
        std::uint64_t sampled_hot_calls = 0;
        std::uint64_t sampled_reload_calls = 0;
        std::uint64_t sampled_fresh_calls = 0;
        std::chrono::steady_clock::duration chunk_lookup{};
        std::chrono::steady_clock::duration chunk_lru{};
        std::chrono::steady_clock::duration subchunk_lookup{};
        std::chrono::steady_clock::duration subchunk_init{};
        std::chrono::steady_clock::duration flushed_lookup{};
        std::chrono::steady_clock::duration cold_extract{};
        std::chrono::steady_clock::duration decoded_assignment{};
        std::chrono::steady_clock::duration fresh_emplace{};
        std::chrono::steady_clock::duration reload_emplace{};
    } cache_detail;
    std::uint64_t sampled_world_z_runs = 0;
    std::uint64_t sampled_world_z_blocks = 0;
    std::uint64_t sampled_world_z_segments = 0;
    std::chrono::steady_clock::duration sampled_world_route_duration{};
    std::chrono::steady_clock::duration sampled_world_acquire_duration{};
    std::chrono::steady_clock::duration sampled_world_write_duration{};
    constexpr std::uint64_t kDetailedWorldSampleMask = 4095;
    struct SaveOutcome {
        Result<void> result;
        std::vector<EncodedSubChunkData> payloads;
        std::chrono::steady_clock::duration encode_duration{};
        std::chrono::steady_clock::duration database_duration{};
    };
    struct PendingSave {
        std::future<SaveOutcome> future;
        std::unordered_set<ChunkPos, ChunkPosHash> chunks;
        std::vector<SubChunkPos> layers;
    };
    constexpr std::size_t kEncodingWorkers = 2;
    detail::BoundedThreadPool save_pool(kEncodingWorkers, kEncodingWorkers);
    std::deque<PendingSave> pending_saves;
    const auto chunk_lookaside_slot = [](ChunkPos position) {
        auto mixed = static_cast<std::uint32_t>(position.x) * 0x9e3779b1u ^
            static_cast<std::uint32_t>(position.z) * 0x85ebca6bu;
        mixed ^= mixed >> 16u;
        return static_cast<std::size_t>(mixed & 255u);
    };
    const auto subchunk_lookaside_slot = [](SubChunkPos position) {
        auto mixed = static_cast<std::uint32_t>(position.x) * 0x9e3779b1u ^
            static_cast<std::uint32_t>(position.y) * 0x85ebca6bu ^
            static_cast<std::uint32_t>(position.z) * 0xc2b2ae35u;
        mixed ^= mixed >> 16u;
        return static_cast<std::size_t>(mixed & 255u);
    };
    const auto invalidate_chunk_lookaside = [&](ChunkPos position, CachedChunk* chunk) {
        auto& entry = chunk_lookaside[chunk_lookaside_slot(position)];
        if (entry.chunk == chunk) entry.chunk = nullptr;
        for (const auto& [sub_y, unused] : chunk->data.sub_chunks) {
            auto& sub_entry = subchunk_lookaside[subchunk_lookaside_slot(
                { position.x, sub_y, position.z })];
            if (sub_entry.owner == chunk) {
                sub_entry.owner = nullptr;
                sub_entry.subchunk = nullptr;
            }
        }
    };
    const auto complete_next_pending_save = [&]() -> Result<void> {
        if (pending_saves.empty()) return Result<void>::success();
        const auto wait_start = std::chrono::steady_clock::now();
        auto completed = pending_saves.front().future.get();
        save_wait_duration += std::chrono::steady_clock::now() - wait_start;
        encode_duration += completed.encode_duration;
        database_save_duration += completed.database_duration;
        save_duration += completed.encode_duration + completed.database_duration;
        if (!completed.result) {
            pending_saves.pop_front();
            return Result<void>::failure(
                "BDX 流式异步保存 chunk 失败: " + completed.result.error());
        }
        for (auto& payload : completed.payloads) {
            flushed.insert(payload.pos);
        }
        std::vector<EncodedSubChunkData> evicted;
        for (auto& payload : completed.payloads) {
            insert_cold(std::move(payload), evicted);
        }
        if (!evicted.empty()) {
            const auto database_start = std::chrono::steady_clock::now();
            auto saved = adapter->save_subchunk_payloads(std::move(evicted));
            const auto saved_for = std::chrono::steady_clock::now() - database_start;
            database_save_duration += saved_for;
            save_duration += saved_for;
            if (!saved) {
                pending_saves.pop_front();
                return Result<void>::failure(
                    "BDX 流式保存冷缓存 payload 失败: " + saved.error());
            }
        }
        pending_saves.pop_front();
        return Result<void>::success();
    };
    const auto flush_chunks = [&](std::span<const ChunkPos> positions) -> Result<void> {
        if (positions.empty()) return Result<void>::success();
        if (pending_saves.size() >= kEncodingWorkers) {
            auto previous = complete_next_pending_save();
            if (!previous) return previous;
        }

        struct OwnedChunkWrite {
            ChunkPos position{};
            ChunkData data;
        };
        std::vector<OwnedChunkWrite> owned;
        owned.reserve(positions.size());
        std::unordered_set<ChunkPos, ChunkPosHash> pending_chunks;
        pending_chunks.reserve(positions.size());
        std::vector<SubChunkPos> pending_layers;
            for (const auto position : positions) {
            const auto found = cache.find(position);
            if (found == cache.end()) continue;
            pending_chunks.insert(position);
            pending_layers.reserve(pending_layers.size() + found->second.data.sub_chunks.size());
            for (const auto& [sub_y, unused] : found->second.data.sub_chunks) {
                pending_layers.push_back({ position.x, sub_y, position.z });
            }
            invalidate_chunk_lookaside(position, &found->second);
            owned.push_back({ position, std::move(found->second.data) });
            cache.erase(found);
        }
        if (owned.empty()) return Result<void>::success();

        auto future = save_pool.submit(
            [adapter, owned = std::move(owned)]() mutable -> SaveOutcome {
                std::vector<ChunkWrite> writes;
                writes.reserve(owned.size());
                for (const auto& write : owned) {
                    writes.push_back({ write.position, &write.data });
                }
                const auto encode_start = std::chrono::steady_clock::now();
                auto encoded = adapter->encode_chunks(writes);
                const auto encoded_for = std::chrono::steady_clock::now() - encode_start;
                if (!encoded) {
                    return {
                        Result<void>::failure(encoded.error()), {}, encoded_for, {} };
                }
                auto payloads = std::move(encoded).value();
                return {
                    Result<void>::success(),
                    std::move(payloads),
                    encoded_for,
                    {}
                };
            });
        ++save_batches;
        pending_saves.push_back(PendingSave{
            std::move(future), std::move(pending_chunks), std::move(pending_layers) });
        return Result<void>::success();
    };

    const auto flush_oldest = [&](std::size_t count) -> Result<void> {
        std::vector<ChunkPos> positions;
        positions.reserve(std::min(count, cache.size()));
        if (count == 0 || cache.empty()) return Result<void>::success();
        std::vector<std::pair<std::uint64_t, ChunkPos>> oldest;
        oldest.reserve(cache.size());
        for (const auto& [position, cached] : cache) {
            oldest.push_back({ cached.last_touch, position });
        }
        const auto take = std::min(count, oldest.size());
        if (take < oldest.size()) {
            std::nth_element(
                oldest.begin(), oldest.begin() + static_cast<std::ptrdiff_t>(take), oldest.end(),
                [](const auto& left, const auto& right) { return left.first < right.first; });
        }
        for (std::size_t index = 0; index < take; ++index) {
            positions.push_back(oldest[index].second);
        }
        return flush_chunks(positions);
    };

    const auto acquire_chunk = [&](ChunkPos position, bool sample_hot) -> CachedChunk& {
        if (detail_profile) ++cache_detail.acquire_calls;
        const auto lookup_start = sample_hot
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        auto& lookaside = chunk_lookaside[chunk_lookaside_slot(position)];
        if (lookaside.chunk != nullptr && lookaside.position == position) {
            if (sample_hot) {
                cache_detail.chunk_lookup +=
                    std::chrono::steady_clock::now() - lookup_start;
            }
            if (detail_profile) {
                ++cache_detail.chunk_hits;
                ++cache_detail.chunk_lookaside_hits;
                ++cache_detail.lru_splices;
            }
            lookaside.chunk->last_touch = ++touch_clock;
            return *lookaside.chunk;
        }
        if (auto found = cache.find(position); found != cache.end()) {
            if (sample_hot) {
                cache_detail.chunk_lookup +=
                    std::chrono::steady_clock::now() - lookup_start;
            }
            if (detail_profile) {
                ++cache_detail.chunk_hits;
                ++cache_detail.chunk_map_hits;
            }
            if (detail_profile) ++cache_detail.lru_splices;
            found->second.last_touch = ++touch_clock;
            lookaside = { position, &found->second };
            return found->second;
        }
        if (sample_hot) {
            cache_detail.chunk_lookup +=
                std::chrono::steady_clock::now() - lookup_start;
        }
        if (detail_profile) ++cache_detail.chunk_misses;
        const auto pending = std::find_if(
            pending_saves.begin(), pending_saves.end(),
            [position](const auto& save) { return save.chunks.contains(position); });
        if (pending != pending_saves.end()) {
            if (detail_profile) ++cache_detail.pending_waits;
            const auto count = static_cast<std::size_t>(
                std::distance(pending_saves.begin(), pending)) + 1;
            for (std::size_t index = 0; index < count; ++index) {
                const auto completed = complete_next_pending_save();
                if (!completed) throw std::runtime_error(completed.error());
            }
        }
        if (cache.size() >= chunk_cache_limit) {
            if (detail_profile) ++cache_detail.capacity_flushes;
            const auto saved = flush_oldest(kFlushBatchSize);
            if (!saved) throw std::runtime_error(saved.error());
        }

        ChunkData data;
        data.layout = BlockLayerLayout::Native;
        auto& inserted = cache.emplace(
            position, CachedChunk{ std::move(data), {}, ++touch_clock })
            .first->second;
        lookaside = { position, &inserted };
        return inserted;
    };

    const auto acquire_subchunk = [&](SubChunkPos position, bool sample_hot) -> SubChunkData& {
        if (sample_hot) ++cache_detail.sampled_hot_calls;
        const auto subchunk_slot = subchunk_lookaside_slot(position);
        auto& sub_lookaside = subchunk_lookaside[subchunk_slot];
        if (sub_lookaside.subchunk != nullptr &&
            sub_lookaside.owner != nullptr &&
            sub_lookaside.position == position) {
            sub_lookaside.owner->last_touch = ++touch_clock;
            if (detail_profile) {
                ++cache_detail.subchunk_hits;
                ++cache_detail.subchunk_lookaside_hits;
            }
            return *sub_lookaside.subchunk;
        }
        auto& cached_chunk = acquire_chunk({ position.x, position.z }, sample_hot);
        auto& chunk = cached_chunk.data;
        const auto layer_index = static_cast<std::size_t>(position.y - kMinCachedSubY);
        if (cached_chunk.layers[layer_index] != nullptr) {
            if (detail_profile) ++cache_detail.subchunk_hits;
            sub_lookaside = { position, &cached_chunk, cached_chunk.layers[layer_index] };
            return *cached_chunk.layers[layer_index];
        }
        const auto subchunk_lookup_start = sample_hot
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if (auto found = chunk.sub_chunks.find(position.y); found != chunk.sub_chunks.end()) {
            if (sample_hot) {
                cache_detail.subchunk_lookup +=
                    std::chrono::steady_clock::now() - subchunk_lookup_start;
            }
            if (detail_profile) ++cache_detail.subchunk_hits;
            cached_chunk.layers[layer_index] = &found->second;
            sub_lookaside = { position, &cached_chunk, &found->second };
            return found->second;
        }
        if (sample_hot) {
            cache_detail.subchunk_lookup +=
                std::chrono::steady_clock::now() - subchunk_lookup_start;
        }
        if (detail_profile) ++cache_detail.subchunk_misses;
        const bool sample_miss = detail_profile &&
            ((cache_detail.subchunk_misses & 63u) == 1);
        const auto init_start = (sample_hot || sample_miss)
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        SubChunkData data;
        data.layer0.fill(mRegistry.air_runtime_id());
        data.layer1.fill(mRegistry.air_runtime_id());
        if (sample_hot || sample_miss) {
            cache_detail.subchunk_init +=
                std::chrono::steady_clock::now() - init_start;
        }
        const auto flushed_start = (sample_hot || sample_miss)
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const bool was_flushed = flushed.contains(position);
        if (sample_hot || sample_miss) {
            cache_detail.flushed_lookup +=
                std::chrono::steady_clock::now() - flushed_start;
        }
        if (was_flushed) {
            if (detail_profile) ++cache_detail.reload_subchunks;
            const bool sample_cold = sample_miss;
            if (sample_cold) {
                ++cache_detail.sampled_reload_calls;
            }
            ++reload_count;
            const auto reload_start = std::chrono::steady_clock::now();
            std::optional<std::vector<std::uint8_t>> payload;
            const auto cold_start = sample_cold
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            if (auto cold = cold_cache.find(position); cold != cold_cache.end()) {
                ++cold_cache_hits;
                payload = std::move(cold->second.payload);
                erase_cold(cold);
            } else {
                ++cold_cache_misses;
                const auto database_start = std::chrono::steady_clock::now();
                auto loaded = adapter->load_subchunk_payload(position);
                database_load_duration += std::chrono::steady_clock::now() - database_start;
                if (!loaded) {
                    throw std::runtime_error(
                        "BDX 流式重载 subchunk (" + std::to_string(position.x) + "," +
                        std::to_string(position.z) + ", subY=" + std::to_string(position.y) +
                        ") 失败: " + loaded.error());
                }
                payload = std::move(loaded).value();
            }
            if (sample_cold) {
                cache_detail.cold_extract +=
                    std::chrono::steady_clock::now() - cold_start;
            }
            if (payload && !payload->empty()) {
                const auto decode_start = std::chrono::steady_clock::now();
                auto decoded = adapter->decode_subchunk_payload(*payload);
                decode_duration += std::chrono::steady_clock::now() - decode_start;
                if (!decoded) {
                    throw std::runtime_error(
                        "BDX 流式解码 subchunk (" + std::to_string(position.x) + "," +
                        std::to_string(position.z) + ", subY=" + std::to_string(position.y) +
                        ") 失败: " + decoded.error());
                }
                const auto assignment_start = sample_cold
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                data = std::move(decoded).value();
                if (sample_cold) {
                    cache_detail.decoded_assignment +=
                        std::chrono::steady_clock::now() - assignment_start;
                }
            }
            reload_duration += std::chrono::steady_clock::now() - reload_start;
        } else if (detail_profile) {
            ++cache_detail.fresh_subchunks;
            if (sample_miss) ++cache_detail.sampled_fresh_calls;
        }
        const bool sample_emplace = sample_miss;
        const auto emplace_start = sample_emplace
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        auto& result = chunk.sub_chunks.emplace(position.y, std::move(data)).first->second;
        if (sample_emplace) {
            const auto elapsed = std::chrono::steady_clock::now() - emplace_start;
            if (was_flushed) cache_detail.reload_emplace += elapsed;
            else cache_detail.fresh_emplace += elapsed;
        }
        cached_chunk.layers[layer_index] = &result;
        sub_lookaside = { position, &cached_chunk, &result };
        return result;
    };

    SubChunkPos cached_subchunk_position{};
    SubChunkData* cached_sub = nullptr;
    const auto world_air_runtime_id = mRegistry.air_runtime_id();

    BdxStructure streamed(mRegistry);
    streamed.mBoundsOnly = false;
    streamed.mCaptureEntities = true;
    streamed.mBlockConsumer = [&](BlockPos raw, std::uint32_t runtime_id) {
        const auto local_x = static_cast<std::int64_t>(raw.x) - mMin.x + mOffset.x;
        const auto local_y = static_cast<std::int64_t>(raw.y) - mMin.y + mOffset.y;
        const auto local_z = static_cast<std::int64_t>(raw.z) - mMin.z + mOffset.z;
        const auto target_x = local_x + static_cast<std::int64_t>(start.x) * 16;
        const auto target_y = local_y + static_cast<std::int64_t>(start.y) * 16;
        const auto target_z = local_z + static_cast<std::int64_t>(start.z) * 16;
        if (target_x < std::numeric_limits<std::int32_t>::min() ||
            target_x > std::numeric_limits<std::int32_t>::max() ||
            target_z < std::numeric_limits<std::int32_t>::min() ||
            target_z > std::numeric_limits<std::int32_t>::max() ||
            target_y < kOverworldMinY || target_y > 319) {
            throw std::runtime_error(
                "BDX 流式目标坐标超出 Overworld 范围: (" +
                std::to_string(target_x) + "," + std::to_string(target_y) + "," +
                std::to_string(target_z) + ")");
        }

        const auto x = static_cast<std::int32_t>(target_x);
        const auto y = static_cast<std::int32_t>(target_y);
        const auto z = static_cast<std::int32_t>(target_z);
        const ChunkPos chunk_position{ floor_div(x, 16), floor_div(z, 16) };
        const auto sub_y = floor_div(y, 16);
        const SubChunkPos subchunk_position{ chunk_position.x, sub_y, chunk_position.z };
        if (cached_sub == nullptr || cached_subchunk_position != subchunk_position) {
            cached_sub = &acquire_subchunk(subchunk_position, false);
            cached_subchunk_position = subchunk_position;
        }
        const auto index = static_cast<std::size_t>(
            floor_mod(x, 16) * 256 + floor_mod(y, 16) * 16 + floor_mod(z, 16));
        cached_sub->layer0[index] = runtime_id;
    };
    struct ZRouteState {
        bool valid = false;
        std::int32_t raw_x = 0;
        std::int32_t raw_y = 0;
        std::int64_t next_raw_z = 0;
        std::int32_t target_x = 0;
        std::int32_t target_y = 0;
        std::int64_t next_target_z = 0;
        std::int32_t chunk_x = 0;
        std::int32_t chunk_z = 0;
        std::int32_t sub_y = 0;
        std::int32_t local_x = 0;
        std::int32_t local_y = 0;
        std::int32_t local_z = 0;
    };
    ZRouteState z_route;

    auto consume_z_run = [&](
        BlockPos raw,
        std::span<const std::uint32_t> runtime_ids,
        bool contains_air) {
        if (runtime_ids.empty()) return;
        const bool sample_run = detail_profile &&
            ((sampled_world_z_runs & kDetailedWorldSampleMask) == 0);
        const auto route_start = sample_run
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const auto continuous = z_route.valid &&
            raw.x == z_route.raw_x &&
            raw.y == z_route.raw_y &&
            static_cast<std::int64_t>(raw.z) == z_route.next_raw_z;
        const auto target_x64 = continuous
            ? static_cast<std::int64_t>(z_route.target_x)
            : static_cast<std::int64_t>(raw.x) - mMin.x + mOffset.x +
                static_cast<std::int64_t>(start.x) * 16;
        const auto target_y64 = continuous
            ? static_cast<std::int64_t>(z_route.target_y)
            : static_cast<std::int64_t>(raw.y) - mMin.y + mOffset.y +
                static_cast<std::int64_t>(start.y) * 16;
        const auto target_z64 = continuous
            ? z_route.next_target_z
            : static_cast<std::int64_t>(raw.z) - mMin.z + mOffset.z +
                static_cast<std::int64_t>(start.z) * 16;
        const auto target_last_z64 = target_z64 + static_cast<std::int64_t>(runtime_ids.size()) - 1;
        if (target_x64 < std::numeric_limits<std::int32_t>::min() ||
            target_x64 > std::numeric_limits<std::int32_t>::max() ||
            target_z64 < std::numeric_limits<std::int32_t>::min() ||
            target_last_z64 > std::numeric_limits<std::int32_t>::max() ||
            target_y64 < kOverworldMinY || target_y64 > 319) {
            throw std::runtime_error(
                "BDX 流式 Z run 目标坐标超出 Overworld 范围: (" +
                std::to_string(target_x64) + "," + std::to_string(target_y64) + "," +
                std::to_string(target_z64) + ")");
        }

        const auto target_x = static_cast<std::int32_t>(target_x64);
        const auto target_y = static_cast<std::int32_t>(target_y64);
        auto target_z = target_z64;
        const auto chunk_x = continuous ? z_route.chunk_x : floor_div(target_x, 16);
        auto chunk_z = continuous
            ? z_route.chunk_z
            : floor_div(static_cast<std::int32_t>(target_z), 16);
        const auto sub_y = continuous ? z_route.sub_y : floor_div(target_y, 16);
        const auto local_x = continuous ? z_route.local_x : floor_mod(target_x, 16);
        const auto local_y = continuous ? z_route.local_y : floor_mod(target_y, 16);
        auto local_z = continuous
            ? z_route.local_z
            : floor_mod(static_cast<std::int32_t>(target_z), 16);
        if (sample_run) {
            sampled_world_route_duration +=
                std::chrono::steady_clock::now() - route_start;
        }
        std::size_t consumed = 0;
        while (consumed < runtime_ids.size()) {
            const auto segment_route_start = sample_run
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const auto count = std::min<std::size_t>(
                runtime_ids.size() - consumed,
                static_cast<std::size_t>(16 - local_z));
            const SubChunkPos subchunk_position{ chunk_x, sub_y, chunk_z };
            if (sample_run) {
                sampled_world_route_duration +=
                    std::chrono::steady_clock::now() - segment_route_start;
            }
            if (cached_sub == nullptr || cached_subchunk_position != subchunk_position) {
                const auto acquire_start = sample_run
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                cached_sub = &acquire_subchunk(subchunk_position, sample_run);
                cached_subchunk_position = subchunk_position;
                if (sample_run) {
                    sampled_world_acquire_duration +=
                        std::chrono::steady_clock::now() - acquire_start;
                }
            }
            auto destination = static_cast<std::size_t>(
                local_x * 256 + local_y * 16 + local_z);
            const auto write_start = sample_run
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            if (!contains_air) {
                std::copy_n(
                    runtime_ids.data() + consumed,
                    count,
                    cached_sub->layer0.data() + destination);
            } else {
                for (std::size_t index = 0; index < count; ++index) {
                    const auto runtime_id = runtime_ids[consumed + index];
                    if (runtime_id != world_air_runtime_id) {
                        cached_sub->layer0[destination + index] = runtime_id;
                    }
                }
            }
            if (sample_run) {
                sampled_world_write_duration +=
                    std::chrono::steady_clock::now() - write_start;
                ++sampled_world_z_segments;
            }
            consumed += count;
            target_z += static_cast<std::int64_t>(count);
            local_z += static_cast<std::int32_t>(count);
            if (local_z == 16) {
                local_z = 0;
                ++chunk_z;
            }
        }
        z_route = {
            true,
            raw.x,
            raw.y,
            static_cast<std::int64_t>(raw.z) +
                static_cast<std::int64_t>(runtime_ids.size()),
            target_x,
            target_y,
            target_z,
            chunk_x,
            chunk_z,
            sub_y,
            local_x,
            local_y,
            local_z
        };
        if (sample_run) sampled_world_z_blocks += runtime_ids.size();
        if (detail_profile) ++sampled_world_z_runs;
    };
    streamed.mZRunConsumer = {
        &consume_z_run,
        [](void* context,
            BlockPos raw,
            std::span<const std::uint32_t> runtime_ids,
            bool contains_air) {
            (*static_cast<decltype(consume_z_run)*>(context))(
                raw, runtime_ids, contains_air);
        }
    };
    const auto parsed = streamed.read(mSourcePath);
    if (!parsed) return Result<void>::failure(parsed.error());
    if (profile) {
        const auto parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stream_start).count();
        std::cerr << "BDX stream parse: blocks=" << streamed.mNonAirBlocks
                  << " parse_and_write_ms=" << parse_ms
                  << " save_batches=" << save_batches
                  << " reloads=" << reload_count
                  << " save_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(save_duration).count()
                  << " save_wait_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(save_wait_duration).count()
                  << " reload_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(reload_duration).count()
                  << " db_load_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(database_load_duration).count()
                  << " decode_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(decode_duration).count()
                  << " cold_hits=" << cold_cache_hits
                  << " cold_misses=" << cold_cache_misses
                  << " cold_evictions=" << cold_cache_evictions
                  << " cold_peak_mb=" << (cold_cache_peak_bytes / (1024 * 1024))
                  << " cache_chunks=" << chunk_cache_limit
                  << '\n';
        if (detail_profile) {
            const auto to_microseconds = [](const auto duration) {
                return std::chrono::duration_cast<std::chrono::microseconds>(
                    duration).count();
            };
            const auto sampled_runs = sampled_world_z_runs == 0
                ? 0
                : (sampled_world_z_runs + kDetailedWorldSampleMask) /
                    (kDetailedWorldSampleMask + 1);
            std::cerr << "BDX world detail: z_runs=" << sampled_world_z_runs
                      << " sampled_runs=" << sampled_runs
                      << " sampled_blocks=" << sampled_world_z_blocks
                      << " sampled_segments=" << sampled_world_z_segments
                      << " route_us=" << to_microseconds(sampled_world_route_duration)
                      << " acquire_us=" << to_microseconds(sampled_world_acquire_duration)
                      << " write_us=" << to_microseconds(sampled_world_write_duration)
                      << '\n';
            std::cerr << "BDX cache detail counts: acquire_calls="
                      << cache_detail.acquire_calls
                      << " chunk_hits=" << cache_detail.chunk_hits
                      << " chunk_misses=" << cache_detail.chunk_misses
                      << " chunk_lookaside_hits="
                      << cache_detail.chunk_lookaside_hits
                      << " chunk_map_hits=" << cache_detail.chunk_map_hits
                      << " lru_already_front=" << cache_detail.lru_already_front
                      << " lru_splices=" << cache_detail.lru_splices
                      << " pending_waits=" << cache_detail.pending_waits
                      << " capacity_flushes=" << cache_detail.capacity_flushes
                      << " subchunk_hits=" << cache_detail.subchunk_hits
                      << " subchunk_lookaside_hits="
                      << cache_detail.subchunk_lookaside_hits
                      << " subchunk_misses=" << cache_detail.subchunk_misses
                      << " fresh_subchunks=" << cache_detail.fresh_subchunks
                      << " reload_subchunks=" << cache_detail.reload_subchunks
                      << '\n';
            std::cerr << "BDX cache detail samples: hot_calls="
                      << cache_detail.sampled_hot_calls
                      << " miss_calls="
                      << (cache_detail.sampled_fresh_calls +
                          cache_detail.sampled_reload_calls)
                      << " fresh_calls=" << cache_detail.sampled_fresh_calls
                      << " reload_calls=" << cache_detail.sampled_reload_calls
                      << " chunk_lookup_us="
                      << to_microseconds(cache_detail.chunk_lookup)
                      << " chunk_lru_us="
                      << to_microseconds(cache_detail.chunk_lru)
                      << " subchunk_lookup_us="
                      << to_microseconds(cache_detail.subchunk_lookup)
                      << " subchunk_init_us="
                      << to_microseconds(cache_detail.subchunk_init)
                      << " flushed_lookup_us="
                      << to_microseconds(cache_detail.flushed_lookup)
                      << " cold_extract_us="
                      << to_microseconds(cache_detail.cold_extract)
                      << " decoded_assignment_us="
                      << to_microseconds(cache_detail.decoded_assignment)
                      << " fresh_emplace_us="
                      << to_microseconds(cache_detail.fresh_emplace)
                      << " reload_emplace_us="
                      << to_microseconds(cache_detail.reload_emplace)
                      << '\n';
            const auto decode_profile =
                BedrockWorldOperator::subChunkDecodeProfile();
            std::cerr << "BDX decode detail: calls=" << decode_profile.calls
                      << " sampled_calls=" << decode_profile.sampledCalls
                      << " sampled_layers=" << decode_profile.sampledLayers
                      << " sampled_palette_entries="
                      << decode_profile.sampledPaletteEntries
                      << " payload_copy_us=" << (decode_profile.payloadCopyNs / 1000)
                      << " native_init_us=" << (decode_profile.nativeInitNs / 1000)
                      << " packed_read_us=" << (decode_profile.packedReadNs / 1000)
                      << " palette_resolve_us="
                      << (decode_profile.paletteResolveNs / 1000)
                      << " block_expand_us=" << (decode_profile.blockExpandNs / 1000)
                      << " set_blocks_us=" << (decode_profile.setBlocksNs / 1000)
                      << " wrapper_us=" << (decode_profile.wrapperNs / 1000)
                      << '\n';
            const auto encode_profile =
                BedrockWorldOperator::subChunkEncodeProfile();
            std::cerr << "BDX encode detail: calls=" << encode_profile.calls
                      << " sampled_calls=" << encode_profile.sampledCalls
                      << " sampled_layers=" << encode_profile.sampledLayers
                      << " sampled_palette_entries="
                      << encode_profile.sampledPaletteEntries
                      << " palette_build_us="
                      << (encode_profile.paletteBuildNs / 1000)
                      << " index_pack_us=" << (encode_profile.indexPackNs / 1000)
                      << " packed_write_us=" << (encode_profile.packedWriteNs / 1000)
                      << " palette_write_us="
                      << (encode_profile.paletteWriteNs / 1000)
                      << '\n';
        }
    }

    const auto total_chunks = static_cast<std::size_t>(mOriginalSize.chunk_x_count()) *
        static_cast<std::size_t>(mOriginalSize.chunk_z_count());
    if (callbacks.start) callbacks.start(total_chunks);

    while (!cache.empty()) {
        const auto saved = flush_oldest(kFlushBatchSize);
        if (!saved) return saved;
    }
    while (!pending_saves.empty()) {
        if (const auto saved = complete_next_pending_save(); !saved) return saved;
    }
    while (!cold_lru.empty()) {
        constexpr std::size_t kPayloadWriteBatchSize = 1024;
        std::vector<EncodedSubChunkData> payloads;
        payloads.reserve(std::min(kPayloadWriteBatchSize, cold_cache.size()));
        while (!cold_lru.empty() && payloads.size() < kPayloadWriteBatchSize) {
            const auto position = cold_lru.back();
            const auto found = cold_cache.find(position);
            if (found == cold_cache.end()) {
                cold_lru.pop_back();
                continue;
            }
            payloads.push_back({ position, std::move(found->second.payload) });
            erase_cold(found);
        }
        if (payloads.empty()) continue;
        const auto database_start = std::chrono::steady_clock::now();
        auto saved = adapter->save_subchunk_payloads(std::move(payloads));
        const auto saved_for = std::chrono::steady_clock::now() - database_start;
        database_save_duration += saved_for;
        save_duration += saved_for;
        if (!saved) {
            return Result<void>::failure(
                "BDX 流式保存剩余冷缓存 payload 失败: " + saved.error());
        }
    }

    std::unordered_map<ChunkPos, std::vector<BlockEntity>, ChunkPosHash> entities;
    for (const auto& [position, payload] : streamed.mBlockEntities) {
        const auto target_x = static_cast<std::int64_t>(position.x + mOffset.x) +
            static_cast<std::int64_t>(start.x) * 16;
        const auto target_y = static_cast<std::int64_t>(position.y + mOffset.y) +
            static_cast<std::int64_t>(start.y) * 16;
        const auto target_z = static_cast<std::int64_t>(position.z + mOffset.z) +
            static_cast<std::int64_t>(start.z) * 16;
        if (target_x < std::numeric_limits<std::int32_t>::min() ||
            target_x > std::numeric_limits<std::int32_t>::max() ||
            target_z < std::numeric_limits<std::int32_t>::min() ||
            target_z > std::numeric_limits<std::int32_t>::max() ||
            target_y < kOverworldMinY || target_y > 319) continue;
        const BlockPos target{
            static_cast<std::int32_t>(target_x),
            static_cast<std::int32_t>(target_y),
            static_cast<std::int32_t>(target_z)
        };
        entities[block_to_chunk(target)].push_back({ target, payload });
    }
    for (auto& [position, values] : entities) {
        const auto saved = adapter->save_chunk_nbt(position, values);
        if (!saved) return saved;
    }

    if (callbacks.progress) {
        for (std::size_t completed = 0; completed < total_chunks; ++completed) {
            callbacks.progress();
        }
    }
    if (profile) {
        const auto save_ms = std::chrono::duration_cast<std::chrono::milliseconds>(save_duration).count();
        std::cerr << "BDX stream: blocks=" << streamed.mNonAirBlocks
                  << " save_batches=" << save_batches
                  << " reloads=" << reload_count
                  << " save_ms=" << save_ms
                  << " encode_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(encode_duration).count()
                  << " db_save_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(database_save_duration).count()
                  << " save_wait_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(save_wait_duration).count()
                  << " reload_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(reload_duration).count()
                  << " db_load_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(database_load_duration).count()
                  << " decode_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(decode_duration).count()
                  << " cold_hits=" << cold_cache_hits
                  << " cold_misses=" << cold_cache_misses
                  << " cold_evictions=" << cold_cache_evictions
                  << " cold_peak_mb=" << (cold_cache_peak_bytes / (1024 * 1024))
                  << " cache_chunks=" << chunk_cache_limit << '\n';
    }
    return Result<void>::success();
}

Result<void> BdxStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("BDX 导出尚未迁移");
}

} // namespace water_structure
