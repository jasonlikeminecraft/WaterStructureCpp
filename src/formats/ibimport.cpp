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
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <optional>
#include <cctype>

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

std::uint32_t varint(std::istream& input)
{
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 7) {
        const auto next = input.get();
        if (next == std::char_traits<char>::eof()) throw std::runtime_error("IBImport varint truncated");
        const auto byte = static_cast<std::uint8_t>(next);
        value |= static_cast<std::uint32_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) return value;
    }
    throw std::runtime_error("IBImport 变长整数过长");
}

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
    mSize = { mOriginalSize.width + std::abs(offset.x), mOriginalSize.height + std::abs(offset.y), mOriginalSize.length + std::abs(offset.z) };
}

Result<void> IbImportStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 IBImport 文件: " + path.string());
    try {
        std::array<char, 9> header{}; input.read(header.data(), header.size());
        if (input.gcount() != 9 || std::string_view(header.data(), header.size()) != "IBImport ") return Result<void>::failure("IBImport 文件头无效");
        std::vector<std::vector<std::uint8_t>> segments;
        while (input.peek() != std::char_traits<char>::eof()) {
            const auto length = varint(input);
            if (length > 256u * 1024u * 1024u) throw std::runtime_error("IBImport 段长度超过 256 MiB");
            const auto key = input.get(); if (key == std::char_traits<char>::eof()) throw std::runtime_error("IBImport 段密钥截断");
            std::vector<std::uint8_t> data(length); input.read(reinterpret_cast<char*>(data.data()), length);
            if (input.gcount() != static_cast<std::streamsize>(length)) throw std::runtime_error("IBImport 段数据截断");
            for (auto& byte : data) byte ^= static_cast<std::uint8_t>(key);
            segments.push_back(std::move(data));
        }
        if (segments.empty()) return Result<void>::failure("IBImport 没有数据段");
        std::map<std::array<std::int32_t, 3>, Block> blocks;
        std::istringstream script(std::string(segments[0].begin(), segments[0].end()));
        std::string line;
        while (std::getline(script, line)) {
            std::istringstream fields(line); std::string op, sx, sy, sz, name, state;
            if (!(fields >> op >> sx >> sy >> sz >> name) || op != "setblock") continue;
            std::getline(fields, state);
            state = trim_copy(std::move(state));
            auto properties = parse_state_properties(state);
            const auto runtime = mRegistry.find(name, properties).value_or(mRegistry.register_state({ name, properties, 0 }));
            const std::array<std::int32_t, 3> key{ relative(sx), relative(sy), relative(sz) };
            blocks[key] = { { key[0], key[1], key[2] }, runtime, {} };
        }
        if (segments.size() >= 2) {
                const auto commands = nlohmann::json::parse(segments[1]);
                if (commands.is_array()) for (const auto& command : commands) {
                const std::array<std::int32_t, 3> key{ relative(command.value("posX", std::string{})), relative(command.value("posY", std::string{})), relative(command.value("posZ", std::string{})) };
                    if (const auto it = blocks.find(key); it != blocks.end()) {
                        auto decoded = command.value("CommandMessage", std::string{});
                        if (const auto plain = decode_base64(decoded)) decoded = *plain;
                        auto normalized = command;
                        normalized["CommandMessage"] = decoded;
                        it->second.nbt = command_nbt(normalized);
                    }
            }
        }
        if (blocks.empty()) return Result<void>::failure("IBImport 脚本没有有效 setblock");
        std::array<std::int32_t, 3> minimum{ std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max() };
        std::array<std::int32_t, 3> maximum{ std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min() };
        for (const auto& [key, _] : blocks) for (int axis = 0; axis < 3; ++axis) { minimum[axis] = std::min(minimum[axis], key[axis]); maximum[axis] = std::max(maximum[axis], key[axis]); }
        mBlocks.clear(); mBlocks.reserve(blocks.size()); mNonAirBlocks = 0;
        for (auto& [_, block] : blocks) { block.pos.x -= minimum[0]; block.pos.y -= minimum[1]; block.pos.z -= minimum[2]; if (block.runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks; mBlocks.push_back(std::move(block)); }
        mOriginalSize = { maximum[0] - minimum[0] + 1, maximum[1] - minimum[1] + 1, maximum[2] - minimum[2] + 1 }; set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) { return Result<void>::failure(std::string("解析 IBImport 失败: ") + error.what()); }
}

Result<ChunkMap> IbImportStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result; for (const auto pos : positions) result.emplace(pos, ChunkData{});
    for (const auto& block : mBlocks) { const auto x = block.pos.x + mOffset.x, y = block.pos.y + mOffset.y, z = block.pos.z + mOffset.z; const ChunkPos chunk{ floor_div(x,16), floor_div(z,16) }; const auto it = result.find(chunk); if (it == result.end()) continue; const auto sy = floor_div(y-64,16); auto [sub,inserted]=it->second.sub_chunks.try_emplace(sy); if(inserted){sub->second.layer0.fill(mRegistry.air_runtime_id());sub->second.layer1.fill(mRegistry.air_runtime_id());} const auto lx=x-chunk.x*16,ly=y-(sy*16+64),lz=z-chunk.z*16; sub->second.layer0[static_cast<std::size_t>((ly*16+lz)*16+lx)]=block.runtime_id; }
    return Result<ChunkMap>::success(std::move(result));
}
Result<NbtChunkMap> IbImportStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result; for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    for (const auto& block : mBlocks) if (!block.nbt.empty()) { const BlockPos pos{ block.pos.x + mOffset.x, block.pos.y + mOffset.y, block.pos.z + mOffset.z }; const auto chunk = block_to_chunk(pos); if (const auto it=result.find(chunk);it!=result.end()) it->second.push_back({{pos.x-chunk.x*16,structure_y_to_chunk_local(pos.y),pos.z-chunk.z*16},block.nbt}); }
    return Result<NbtChunkMap>::success(std::move(result));
}
Result<void> IbImportStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const { return convert_to_world(*this, world, start, std::move(callbacks)); }
Result<void> IbImportStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks) { return Result<void>::failure("IBImport 导出尚未迁移"); }

} // namespace water_structure
