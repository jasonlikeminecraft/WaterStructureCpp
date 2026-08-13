#include "mcfunction.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

namespace water_structure {
namespace {

std::vector<std::string> split_fields(std::string_view line)
{
    std::vector<std::string> result;
    std::istringstream input{ std::string(line) };
    std::string field;
    while (input >> field) result.push_back(std::move(field));
    return result;
}

std::optional<std::int32_t> coordinate(std::string_view token)
{
    if (token.empty()) return std::nullopt;
    if (token.front() == '~') token.remove_prefix(1);
    if (token.empty()) return 0;
    std::int32_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return std::nullopt;
    return value;
}

std::optional<std::vector<BlockStateProperty>> states(std::string_view text)
{
    if (text.empty()) return std::vector<BlockStateProperty>{};
    if (text.front() != '[' || text.back() != ']') return std::nullopt;
    text.remove_prefix(1); text.remove_suffix(1);
    std::vector<BlockStateProperty> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        auto comma = std::string_view::npos;
        bool quoted = false;
        for (auto index = begin; index < text.size(); ++index) {
            if (text[index] == '"') quoted = !quoted;
            if (text[index] == ',' && !quoted) {
                comma = index;
                break;
            }
        }
        const auto part = text.substr(
            begin,
            comma == std::string_view::npos ? text.size() - begin : comma - begin);
        if (!part.empty()) {
            const auto equal = part.find('=');
            if (equal == std::string_view::npos || equal == 0) return std::nullopt;
            auto name = std::string(part.substr(0, equal));
            auto value = std::string(part.substr(equal + 1));
            auto trim = [](std::string& s) {
                const auto first = s.find_first_not_of(" \t\r\n\"");
                const auto last = s.find_last_not_of(" \t\r\n\"");
                s = first == std::string::npos ? std::string{} : s.substr(first, last - first + 1);
            };
            trim(name); trim(value);
            BlockStateProperty property;
            property.name = std::move(name);
            property.value = std::move(value);
            if (property.value == "true" || property.value == "false") {
                property.type = BlockStateValueType::Byte;
                property.value = property.value == "true" ? "1" : "0";
            }
            else {
                std::int32_t numeric = 0;
                const auto parsed = std::from_chars(property.value.data(), property.value.data() + property.value.size(), numeric);
                property.type = parsed.ec == std::errc{} && parsed.ptr == property.value.data() + property.value.size()
                    ? BlockStateValueType::Int : BlockStateValueType::String;
            }
            result.push_back(std::move(property));
        }
        if (comma == std::string_view::npos) break;
        begin = comma + 1;
    }
    return result;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
}

} // namespace

void McFunctionStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mCommandIndex.clear();
    mCommandIndexReady = false;
    mSize = { mOriginalSize.width + std::abs(offset.x), mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z) };
}

Result<void> McFunctionStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 MCFunction 文件: " + path.string());
    std::vector<Command> commands;
    commands.reserve(4096);
    std::int32_t min_x = std::numeric_limits<std::int32_t>::max(), min_y = min_x, min_z = min_x;
    std::int32_t max_x = std::numeric_limits<std::int32_t>::min(), max_y = max_x, max_z = max_x;
    std::string line;
    std::size_t line_number = 0;
    auto resolve = [&](std::string_view name, std::string_view state_text) -> Result<std::uint32_t> {
        auto parsed_states = states(state_text);
        if (!parsed_states) return Result<std::uint32_t>::failure("第 " + std::to_string(line_number) + " 行: 状态格式无效");
        std::string encoded(name);
        if (!state_text.empty()) encoded += state_text;
        // MCFunction uses Java block-state semantics. Some same-named Bedrock
        // properties encode different values (for example candle counts), so
        // the Java compatibility mapping must be attempted first.
        auto runtime = mRegistry.compatible_java_runtime_id(encoded);
        if (!runtime && !parsed_states->empty()) {
            runtime = mRegistry.legacy_state_runtime_id(name, *parsed_states);
        } else if (!runtime) {
            runtime = mRegistry.legacy_state_runtime_id(name, {});
            if (!runtime) runtime = mRegistry.legacy_runtime_id(name, 0);
        }
        return Result<std::uint32_t>::success(runtime.value_or(
            mRegistry.find("minecraft:unknown").value_or(mRegistry.air_runtime_id())));
    };
    auto add = [&](std::int32_t x1, std::int32_t y1, std::int32_t z1,
                   std::int32_t x2, std::int32_t y2, std::int32_t z2,
                   std::string_view name, std::string_view state_text) -> Result<void> {
        auto runtime = resolve(name, state_text);
        if (!runtime) return Result<void>::failure(runtime.error());
        const auto command = Command{
            std::min(x1, x2), std::max(x1, x2),
            std::min(y1, y2), std::max(y1, y2),
            std::min(z1, z2), std::max(z1, z2), runtime.value()
        };
        commands.push_back(command);
        min_x = std::min(min_x, command.x1); min_y = std::min(min_y, command.y1); min_z = std::min(min_z, command.z1);
        max_x = std::max(max_x, command.x2); max_y = std::max(max_y, command.y2); max_z = std::max(max_z, command.z2);
        return Result<void>::success();
    };
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto lower_end = line.find(' ');
        auto command = line.substr(0, lower_end);
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (command != "setblock" && command != "fill") continue;

        std::string state_text;
        if (const auto open = line.find('['); open != std::string::npos) {
            if (const auto close = line.find_last_of(']');
                close != std::string::npos && close > open) {
                state_text = line.substr(open, close - open + 1);
                line = trim(line.substr(0, open) + line.substr(close + 1));
            }
        }

        const auto fields = split_fields(line);
        const bool fill = command == "fill";
        const std::size_t name_index = fill ? 7 : 4;
        if (fields.size() <= name_index) continue;
        auto parse_coord = [&](std::size_t i) -> std::optional<std::int32_t> {
            return i < fields.size() ? coordinate(fields[i]) : std::nullopt;
        };
        if (fill) {
            if (fields.size() < 8) continue;
            const auto x1 = parse_coord(1), y1 = parse_coord(2), z1 = parse_coord(3), x2 = parse_coord(4), y2 = parse_coord(5), z2 = parse_coord(6);
            if (!x1 || !y1 || !z1 || !x2 || !y2 || !z2) return Result<void>::failure("第 " + std::to_string(line_number) + " 行: 坐标无效");
            auto result = add(*x1, *y1, *z1, *x2, *y2, *z2, fields[name_index], state_text);
            if (!result) return result;
        } else {
            const auto x = parse_coord(1), y = parse_coord(2), z = parse_coord(3);
            if (!x || !y || !z) return Result<void>::failure("第 " + std::to_string(line_number) + " 行: 坐标无效");
            auto result = add(*x, *y, *z, *x, *y, *z, fields[name_index], state_text);
            if (!result) return result;
        }
    }
    if (commands.empty()) return Result<void>::failure("MCFunction 不包含 setblock 或 fill 命令");
    mCommands = std::move(commands);
    mCommandIndex.clear();
    mCommandIndexReady = false;
    mOriginalSize = { max_x - min_x + 1, max_y - min_y + 1, max_z - min_z + 1 };
    mNonAirBlocks.reset();
    // Normalize to the same local origin used by all structure readers.
    for (auto& command : mCommands) {
        command.x1 -= min_x; command.x2 -= min_x;
        command.y1 -= min_y; command.y2 -= min_y;
        command.z1 -= min_z; command.z2 -= min_z;
    }
    set_offset({});
    return Result<void>::success();
}

Result<ChunkMap> McFunctionStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    const auto air = mRegistry.air_runtime_id();
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    if (!mCommandIndexReady) {
        mCommandIndex.clear();
        mBroadCommands.clear();
        for (std::size_t index = 0; index < mCommands.size(); ++index) {
            const auto& command = mCommands[index];
            const auto x1 = command.x1 + mOffset.x, x2 = command.x2 + mOffset.x;
            const auto z1 = command.z1 + mOffset.z, z2 = command.z2 + mOffset.z;
            const auto chunk_x1 = floor_div(x1, 16), chunk_x2 = floor_div(x2, 16);
            const auto chunk_z1 = floor_div(z1, 16), chunk_z2 = floor_div(z2, 16);
            const auto covered = static_cast<std::uint64_t>(chunk_x2 - chunk_x1 + 1) *
                static_cast<std::uint64_t>(chunk_z2 - chunk_z1 + 1);
            // A command covering an enormous area must not turn the index
            // itself into an unbounded allocation. Such commands are rare and
            // cheap to test during a requested chunk replay.
            if (covered > 4096) {
                mBroadCommands.push_back(static_cast<std::uint32_t>(index));
                continue;
            }
            for (auto z = chunk_z1; z <= chunk_z2; ++z)
                for (auto x = chunk_x1; x <= chunk_x2; ++x)
                    mCommandIndex[{x, z}].push_back(static_cast<std::uint32_t>(index));
        }
        mCommandIndexReady = true;
    }
    for (auto& [chunk_pos, chunk] : result) {
        const auto indexed = mCommandIndex.find(chunk_pos);
        const auto process = [&](std::uint32_t command_index) {
            const auto& command = mCommands[command_index];
            const auto cx1 = floor_div(command.x1 + mOffset.x, 16);
            const auto cx2 = floor_div(command.x2 + mOffset.x, 16);
            const auto cz1 = floor_div(command.z1 + mOffset.z, 16);
            const auto cz2 = floor_div(command.z2 + mOffset.z, 16);
            if (chunk_pos.x < cx1 || chunk_pos.x > cx2 || chunk_pos.z < cz1 || chunk_pos.z > cz2) return;
            const auto x1 = std::max(command.x1 + mOffset.x, chunk_pos.x * 16);
            const auto x2 = std::min(command.x2 + mOffset.x, chunk_pos.x * 16 + 15);
            const auto z1 = std::max(command.z1 + mOffset.z, chunk_pos.z * 16);
            const auto z2 = std::min(command.z2 + mOffset.z, chunk_pos.z * 16 + 15);
            for (auto y = command.y1 + mOffset.y; y <= command.y2 + mOffset.y; ++y) {
                const auto sub_y = floor_div(y - 64, 16);
                auto [sub, inserted] = chunk.sub_chunks.try_emplace(sub_y);
                if (inserted) { sub->second.layer0.fill(air); sub->second.layer1.fill(air); }
                const auto ly = y - (sub_y * 16 + 64);
                for (auto z = z1; z <= z2; ++z) for (auto x = x1; x <= x2; ++x) {
                    const auto lx = x - chunk_pos.x * 16, lz = z - chunk_pos.z * 16;
                    sub->second.layer0[static_cast<std::size_t>((ly * 16 + lz) * 16 + lx)] = command.runtime_id;
                }
            }
        };
        if (indexed != mCommandIndex.end()) {
            for (const auto command_index : indexed->second) process(command_index);
        }
        for (const auto command_index : mBroadCommands) process(command_index);
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<void> McFunctionStructure::visit_chunks(
    std::span<const ChunkPos> positions,
    const ChunkVisitor& visitor) const
{
    if (!visitor) return Result<void>::failure("chunk visitor is empty");
    // Keep only one materialized chunk alive.  This is important for callers
    // that stream directly into a world or an encoder.
    for (const auto position : positions) {
        const std::array<ChunkPos, 1> request{{position}};
        auto chunks = get_chunks(request);
        if (!chunks) return Result<void>::failure(chunks.error());
        const auto found = chunks.value().find(position);
        if (found == chunks.value().end()) continue;
        auto visited = visitor(position, found->second);
        if (!visited) return visited;
    }
    return Result<void>::success();
}

Result<std::size_t> McFunctionStructure::count_non_air_blocks() const
{
    if (mNonAirBlocks) return Result<std::size_t>::success(*mNonAirBlocks);
    std::size_t count = 0;
    for (std::int32_t z = 0; z < mSize.chunk_z_count(); ++z) {
        for (std::int32_t x = 0; x < mSize.chunk_x_count(); ++x) {
            const std::array<ChunkPos, 1> position{{x, z}};
            auto chunks = get_chunks(position);
            if (!chunks) return Result<std::size_t>::failure(chunks.error());
            const auto& chunk = chunks.value().at({x, z});
            for (const auto& [_, sub] : chunk.sub_chunks)
                for (const auto runtime : sub.layer0) if (runtime != mRegistry.air_runtime_id()) ++count;
        }
    }
    mNonAirBlocks = count;
    return Result<std::size_t>::success(count);
}

Result<NbtChunkMap> McFunctionStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result; for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> McFunctionStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const { return convert_to_world(*this, world, start, std::move(callbacks)); }
Result<void> McFunctionStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks) { return Result<void>::failure("MCFunction 导出尚未迁移"); }

} // namespace water_structure
