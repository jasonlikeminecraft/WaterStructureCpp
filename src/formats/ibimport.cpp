#include "ibimport.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <io/stream_writer.h>
#include <nlohmann/json.hpp>
#include <tag_compound.h>
#include <tag_primitive.h>
#include <tag_string.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <fstream>
#include <limits>
#include <optional>
#include <cctype>
#include <sstream>
#include <streambuf>

namespace water_structure {
namespace {

std::int32_t relative(std::string_view token)
{
    if (!token.empty() && token.front() == '~') token.remove_prefix(1);
    if (token.empty()) return 0;
    std::int32_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() ? value : 0;
}

std::optional<std::uint64_t> varint(std::istream& input)
{
    const auto first = input.get();
    if (first == std::char_traits<char>::eof()) return std::nullopt;
    std::uint64_t value = 0;
    auto byte = static_cast<std::uint8_t>(first);
    for (unsigned shift = 0; shift < 63; shift += 7) {
        value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) return value;
        const auto next = input.get();
        if (next == std::char_traits<char>::eof()) throw std::runtime_error("IBImport varint truncated");
        byte = static_cast<std::uint8_t>(next);
    }
    throw std::runtime_error("IBImport 变长整数过长");
}

class XorSegmentBuffer final : public std::streambuf {
public:
    XorSegmentBuffer(std::istream& input, std::uint64_t length, std::uint8_t key)
        : mInput(input), mRemaining(length), mKey(key)
    {
        setg(mBuffer.data(), mBuffer.data(), mBuffer.data());
    }

    void finish()
    {
        setg(mBuffer.data(), mBuffer.data(), mBuffer.data());
        while (mRemaining != 0) {
            const auto count = static_cast<std::streamsize>(
                std::min<std::uint64_t>(mRemaining, mBuffer.size()));
            mInput.read(mBuffer.data(), count);
            const auto received = mInput.gcount();
            if (received != count) {
                mTruncated = true;
                mRemaining = 0;
                break;
            }
            mRemaining -= static_cast<std::uint64_t>(received);
        }
        if (mTruncated) throw std::runtime_error("IBImport 段数据截断");
    }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        if (mRemaining == 0) return traits_type::eof();
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(mRemaining, mBuffer.size()));
        mInput.read(mBuffer.data(), count);
        const auto received = mInput.gcount();
        if (received <= 0) {
            mTruncated = true;
            mRemaining = 0;
            return traits_type::eof();
        }
        mRemaining -= static_cast<std::uint64_t>(received);
        if (received != count) mTruncated = true;
        for (std::streamsize index = 0; index < received; ++index) {
            mBuffer[static_cast<std::size_t>(index)] = static_cast<char>(
                static_cast<std::uint8_t>(mBuffer[static_cast<std::size_t>(index)]) ^ mKey);
        }
        setg(mBuffer.data(), mBuffer.data(), mBuffer.data() + received);
        return traits_type::to_int_type(*gptr());
    }

private:
    std::istream& mInput;
    std::uint64_t mRemaining = 0;
    std::uint8_t mKey = 0;
    bool mTruncated = false;
    std::array<char, 64 * 1024> mBuffer{};
};

class XorSegmentInput final {
public:
    XorSegmentInput(std::istream& input, std::uint64_t length, std::uint8_t key)
        : mBuffer(input, length, key), mStream(&mBuffer) {}

    std::istream& stream() noexcept { return mStream; }
    void finish() { mBuffer.finish(); }

private:
    XorSegmentBuffer mBuffer;
    std::istream mStream;
};

class CommandJsonSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    explicit CommandJsonSax(std::function<void(const nlohmann::json&)> consume)
        : mConsume(std::move(consume)) {}

    bool null() override { return value(nullptr); }
    bool boolean(bool input) override { return value(input); }
    bool number_integer(number_integer_t input) override { return value(input); }
    bool number_unsigned(number_unsigned_t input) override { return value(input); }
    bool number_float(number_float_t input, const string_t&) override { return value(input); }
    bool string(string_t& input) override { return value(input); }
    bool binary(binary_t& input) override { return value(input); }

    bool start_object(std::size_t) override
    {
        if (mDepth == 1) {
            mCurrent = nlohmann::json::object();
            mInCommand = true;
        }
        ++mDepth;
        return true;
    }
    bool key(string_t& input) override
    {
        if (mInCommand && mDepth == 2) mKey = input;
        return true;
    }
    bool end_object() override
    {
        if (mInCommand && mDepth == 2) {
            mConsume(mCurrent);
            mCurrent = nlohmann::json{};
            mInCommand = false;
            mKey.clear();
        }
        if (mDepth != 0) --mDepth;
        return true;
    }
    bool start_array(std::size_t) override { ++mDepth; return true; }
    bool end_array() override { if (mDepth != 0) --mDepth; return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& error) override
    {
        mError = error.what();
        return false;
    }
    const std::string& error() const noexcept { return mError; }

private:
    template<class Value>
    bool value(Value&& input)
    {
        if (mInCommand && mDepth == 2 && !mKey.empty()) {
            mCurrent[mKey] = std::forward<Value>(input);
            mKey.clear();
        }
        return true;
    }

    std::function<void(const nlohmann::json&)> mConsume;
    nlohmann::json mCurrent;
    std::string mKey;
    std::string mError;
    std::size_t mDepth = 0;
    bool mInCommand = false;
};

bool boolish(const nlohmann::json& value)
{
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_integer()) return value.get<std::int64_t>() != 0;
    if (value.is_string()) return value.get<std::string>() == "true" || value.get<std::string>() == "1";
    return false;
}

std::string trim_copy(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> decode_base64(std::string_view encoded)
{
    if (encoded.empty() || encoded.size() % 4 != 0) return std::nullopt;
    auto value = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    std::string output;
    output.reserve(encoded.size() / 4 * 3);
    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        const auto a = value(encoded[index]);
        const auto b = value(encoded[index + 1]);
        const auto c = encoded[index + 2] == '=' ? 0 : value(encoded[index + 2]);
        const auto d = encoded[index + 3] == '=' ? 0 : value(encoded[index + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (encoded[index + 2] == '=' && encoded[index + 3] != '=')) return std::nullopt;
        const auto bits = (static_cast<std::uint32_t>(a) << 18) |
            (static_cast<std::uint32_t>(b) << 12) |
            (static_cast<std::uint32_t>(c) << 6) | static_cast<std::uint32_t>(d);
        output.push_back(static_cast<char>(bits >> 16));
        if (encoded[index + 2] != '=') output.push_back(static_cast<char>(bits >> 8));
        if (encoded[index + 3] != '=') output.push_back(static_cast<char>(bits));
    }
    return output;
}

std::string unquote(std::string value)
{
    value = trim_copy(std::move(value));
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return value;
    try {
        const auto parsed = nlohmann::json::parse(value);
        return parsed.is_string() ? parsed.get<std::string>() : value;
    } catch (...) {
        return value.substr(1, value.size() - 2);
    }
}

std::vector<BlockStateProperty> parse_state_properties(std::string state)
{
    std::vector<BlockStateProperty> result;
    state = trim_copy(std::move(state));
    if (state.size() < 2 || state.front() != '[' || state.back() != ']') return result;
    state = state.substr(1, state.size() - 2);
    std::vector<std::string> parts;
    std::size_t begin = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0; index <= state.size(); ++index) {
        const auto character = index < state.size() ? state[index] : ',';
        if (quoted && escaped) {
            escaped = false;
        } else if (quoted && character == '\\') {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        } else if (character == ',' && !quoted) {
            parts.push_back(state.substr(begin, index - begin));
            begin = index + 1;
        }
    }
    for (auto part : parts) {
        part = trim_copy(std::move(part));
        const auto equal = part.find('=');
        if (equal == std::string::npos) continue;
        auto name = unquote(part.substr(0, equal));
        auto value = trim_copy(part.substr(equal + 1));
        if (name.empty()) continue;
        BlockStateProperty property;
        property.name = std::move(name);
        if (value == "true" || value == "false") {
            property.type = BlockStateValueType::Byte;
            property.value = value == "true" ? "1" : "0";
        } else if (!value.empty() && value.front() == '"') {
            property.type = BlockStateValueType::String;
            property.value = unquote(std::move(value));
        } else {
            std::int32_t parsed = 0;
            const auto parse_result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (parse_result.ec == std::errc{} && parse_result.ptr == value.data() + value.size()) {
                property.type = BlockStateValueType::Int;
                property.value = std::to_string(parsed);
            } else {
                property.type = BlockStateValueType::String;
                property.value = std::move(value);
            }
        }
        result.push_back(std::move(property));
    }
    return result;
}

NbtPayload command_nbt(const nlohmann::json& command)
{
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("CommandBlock");
    root["Command"] = nbt::tag_string(command.value("CommandMessage", std::string{}));
    root["CustomName"] = nbt::tag_string(command.value("Commandtitle", std::string{}));
    root["LastOutput"] = nbt::tag_string("");
    root.emplace<nbt::tag_byte>("ExecuteOnFirstTick", 0);
    root.emplace<nbt::tag_byte>("TrackOutput", 0);
    root.emplace<nbt::tag_byte>("conditionalMode", boolish(command.value("Conditional", nlohmann::json{})) ? 1 : 0);
    root.emplace<nbt::tag_byte>("auto", boolish(command.value("isRedstone", nlohmann::json{})) ? 0 : 1);
    root.emplace<nbt::tag_int>("TickDelay", command.value("isTime", 0));
    root.emplace<nbt::tag_int>("Version", 38);
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", root, output, endian::little);
    const auto bytes = output.str();
    return { bytes.begin(), bytes.end() };
}

} // namespace

void IbImportStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mCommandIndex.clear();
    mBroadCommands.clear();
    mCommandIndexReady = false;
    mSize = { mOriginalSize.width + std::abs(offset.x), mOriginalSize.height + std::abs(offset.y), mOriginalSize.length + std::abs(offset.z) };
}

Result<void> IbImportStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 IBImport 文件: " + path.string());
    try {
        std::array<char, 9> header{}; input.read(header.data(), header.size());
        if (input.gcount() != 9 || std::string_view(header.data(), header.size()) != "IBImport ") return Result<void>::failure("IBImport 文件头无效");
        std::vector<Command> commands;
        commands.reserve(4096);
        std::vector<NbtBlock> nbt_blocks;
        std::array<std::int32_t, 3> minimum{ std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max() };
        std::array<std::int32_t, 3> maximum{ std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min() };
        std::size_t segment_index = 0;
        while (true) {
            const auto length = varint(input);
            if (!length) break;
            const auto key = input.get();
            if (key == std::char_traits<char>::eof()) throw std::runtime_error("IBImport 段密钥截断");
            XorSegmentInput segment(input, *length, static_cast<std::uint8_t>(key));
            if (segment_index == 0) {
                std::string line;
                std::size_t line_number = 0;
                while (std::getline(segment.stream(), line)) {
                    ++line_number;
                    std::istringstream fields(line);
                    std::string op, sx1, sy1, sz1, sx2, sy2, sz2, name, state;
                    if (!(fields >> op >> sx1 >> sy1 >> sz1)) continue;
                    std::transform(op.begin(), op.end(), op.begin(), [](unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
                    const bool fill = op == "fill";
                    if (!fill && op != "setblock") continue;
                    if (fill) {
                        if (!(fields >> sx2 >> sy2 >> sz2 >> name)) continue;
                    } else {
                        if (!(fields >> name)) continue;
                        sx2 = sx1; sy2 = sy1; sz2 = sz1;
                    }
                    const auto x1 = relative(sx1), y1 = relative(sy1), z1 = relative(sz1);
                    const auto x2 = relative(sx2), y2 = relative(sy2), z2 = relative(sz2);
                    std::getline(fields, state);
                    state = trim_copy(std::move(state));
                    auto properties = parse_state_properties(state);
                    const auto runtime = mRegistry.find(name, properties).value_or(
                        mRegistry.register_state({ name, properties, 0 }));
                    const Command command{
                        std::min(x1, x2), std::max(x1, x2),
                        std::min(y1, y2), std::max(y1, y2),
                        std::min(z1, z2), std::max(z1, z2), runtime
                    };
                    commands.push_back(command);
                    minimum[0] = std::min(minimum[0], command.x1);
                    minimum[1] = std::min(minimum[1], command.y1);
                    minimum[2] = std::min(minimum[2], command.z1);
                    maximum[0] = std::max(maximum[0], command.x2);
                    maximum[1] = std::max(maximum[1], command.y2);
                    maximum[2] = std::max(maximum[2], command.z2);
                }
            } else if (segment_index == 1) {
                CommandJsonSax sax([&](const nlohmann::json& command) {
                    const BlockPos pos{
                        relative(command.value("posX", std::string{})),
                        relative(command.value("posY", std::string{})),
                        relative(command.value("posZ", std::string{}))
                    };
                    const auto exists = std::find_if(commands.rbegin(), commands.rend(), [&](const Command& block) {
                        return pos.x >= block.x1 && pos.x <= block.x2 &&
                            pos.y >= block.y1 && pos.y <= block.y2 &&
                            pos.z >= block.z1 && pos.z <= block.z2;
                    }) != commands.rend();
                    if (!exists) return;
                    auto decoded = command.value("CommandMessage", std::string{});
                    if (const auto plain = decode_base64(decoded)) decoded = *plain;
                    auto normalized = command;
                    normalized["CommandMessage"] = decoded;
                    nbt_blocks.push_back({ pos, command_nbt(normalized) });
                });
                if (!nlohmann::json::sax_parse(segment.stream(), &sax)) {
                    throw std::runtime_error("IBImport 命令方块 JSON 无效: " + sax.error());
                }
            }
            segment.finish();
            ++segment_index;
        }
        if (segment_index == 0) return Result<void>::failure("IBImport 没有数据段");
        if (commands.empty()) return Result<void>::failure("IBImport 脚本没有有效 setblock 或 fill");
        for (auto& command : commands) {
            command.x1 -= minimum[0]; command.x2 -= minimum[0];
            command.y1 -= minimum[1]; command.y2 -= minimum[1];
            command.z1 -= minimum[2]; command.z2 -= minimum[2];
        }
        for (auto& block : nbt_blocks) {
            block.pos.x -= minimum[0]; block.pos.y -= minimum[1]; block.pos.z -= minimum[2];
        }
        mCommands = std::move(commands);
        mNbtBlocks = std::move(nbt_blocks);
        mOriginalSize = { maximum[0] - minimum[0] + 1, maximum[1] - minimum[1] + 1, maximum[2] - minimum[2] + 1 }; set_offset({});
        mNonAirBlocks.reset();
        return Result<void>::success();
    } catch (const std::exception& error) { return Result<void>::failure(std::string("解析 IBImport 失败: ") + error.what()); }
}

Result<ChunkMap> IbImportStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    const auto air = mRegistry.air_runtime_id();
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    if (!mCommandIndexReady) {
        mCommandIndex.clear();
        mBroadCommands.clear();
        for (std::size_t index = 0; index < mCommands.size(); ++index) {
            if (index > std::numeric_limits<std::uint32_t>::max())
                return Result<ChunkMap>::failure("IBImport 命令数量超过 uint32 容量");
            const auto& command = mCommands[index];
            const auto chunk_x1 = floor_div(command.x1 + mOffset.x, 16);
            const auto chunk_x2 = floor_div(command.x2 + mOffset.x, 16);
            const auto chunk_z1 = floor_div(command.z1 + mOffset.z, 16);
            const auto chunk_z2 = floor_div(command.z2 + mOffset.z, 16);
            const auto covered = static_cast<std::uint64_t>(chunk_x2 - chunk_x1 + 1) *
                static_cast<std::uint64_t>(chunk_z2 - chunk_z1 + 1);
            if (covered > 4096) {
                mBroadCommands.push_back(static_cast<std::uint32_t>(index));
                continue;
            }
            for (auto z = chunk_z1; z <= chunk_z2; ++z)
                for (auto x = chunk_x1; x <= chunk_x2; ++x)
                    mCommandIndex[{ x, z }].push_back(static_cast<std::uint32_t>(index));
        }
        mCommandIndexReady = true;
    }
    for (auto& [chunk_pos, chunk] : result) {
        std::vector<std::uint32_t> candidate = mBroadCommands;
        if (const auto indexed = mCommandIndex.find(chunk_pos); indexed != mCommandIndex.end())
            candidate.insert(candidate.end(), indexed->second.begin(), indexed->second.end());
        std::sort(candidate.begin(), candidate.end());
        candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
        for (const auto command_index : candidate) {
            const auto& command = mCommands[command_index];
            const auto x1 = std::max(command.x1 + mOffset.x, chunk_pos.x * 16);
            const auto x2 = std::min(command.x2 + mOffset.x, chunk_pos.x * 16 + 15);
            const auto z1 = std::max(command.z1 + mOffset.z, chunk_pos.z * 16);
            const auto z2 = std::min(command.z2 + mOffset.z, chunk_pos.z * 16 + 15);
            if (x1 > x2 || z1 > z2) continue;
            for (auto y = command.y1 + mOffset.y; y <= command.y2 + mOffset.y; ++y) {
                const auto sy = floor_div(y - 64, 16);
                auto [sub, inserted] = chunk.sub_chunks.try_emplace(sy);
                if (inserted) {
                    sub->second.layer0.fill(air);
                    sub->second.layer1.fill(air);
                }
                const auto ly = y - (sy * 16 + 64);
                for (auto z = z1; z <= z2; ++z) for (auto x = x1; x <= x2; ++x) {
                    const auto lx = x - chunk_pos.x * 16;
                    const auto lz = z - chunk_pos.z * 16;
                    sub->second.layer0[static_cast<std::size_t>((ly * 16 + lz) * 16 + lx)] = command.runtime_id;
                }
            }
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<void> IbImportStructure::visit_chunks(
    std::span<const ChunkPos> positions,
    const ChunkVisitor& visitor) const
{
    if (!visitor) return Result<void>::failure("chunk visitor is empty");
    for (const auto position : positions) {
        const std::array<ChunkPos, 1> request{{ position }};
        auto chunks = get_chunks(request);
        if (!chunks) return Result<void>::failure(chunks.error());
        const auto found = chunks.value().find(position);
        if (found == chunks.value().end()) continue;
        auto visited = visitor(position, found->second);
        if (!visited) return visited;
    }
    return Result<void>::success();
}

Result<std::size_t> IbImportStructure::count_non_air_blocks() const
{
    if (mNonAirBlocks) return Result<std::size_t>::success(*mNonAirBlocks);
    std::size_t count = 0;
    for (std::int32_t z = 0; z < mSize.chunk_z_count(); ++z) {
        for (std::int32_t x = 0; x < mSize.chunk_x_count(); ++x) {
            const std::array<ChunkPos, 1> position{{ x, z }};
            auto chunks = get_chunks(position);
            if (!chunks) return Result<std::size_t>::failure(chunks.error());
            const auto& chunk = chunks.value().at({ x, z });
            for (const auto& [_, sub] : chunk.sub_chunks)
                for (const auto runtime : sub.layer0)
                    if (runtime != mRegistry.air_runtime_id()) ++count;
        }
    }
    mNonAirBlocks = count;
    return Result<std::size_t>::success(count);
}

Result<NbtChunkMap> IbImportStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result; for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    for (const auto& block : mNbtBlocks) if (!block.nbt.empty()) { const BlockPos pos{ block.pos.x + mOffset.x, block.pos.y + mOffset.y, block.pos.z + mOffset.z }; const auto chunk = block_to_chunk(pos); if (const auto it=result.find(chunk);it!=result.end()) it->second.push_back({{pos.x-chunk.x*16,structure_y_to_chunk_local(pos.y),pos.z-chunk.z*16},block.nbt}); }
    return Result<NbtChunkMap>::success(std::move(result));
}
Result<void> IbImportStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const { return convert_to_world(*this, world, start, std::move(callbacks)); }
Result<void> IbImportStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks) { return Result<void>::failure("IBImport 导出尚未迁移"); }

} // namespace water_structure
