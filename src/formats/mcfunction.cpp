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
#include <map>

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
        const auto comma = text.find(',', begin);
        const auto part = text.substr(begin, comma == std::string_view::npos ? text.size() - begin : comma - begin);
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
            if (property.value == "true" || property.value == "false") property.type = BlockStateValueType::Byte;
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
    mSize = { mOriginalSize.width + std::abs(offset.x), mOriginalSize.height + std::abs(offset.y),
        mOriginalSize.length + std::abs(offset.z) };
}

Result<void> McFunctionStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) return Result<void>::failure("无法打开 MCFunction 文件: " + path.string());
    std::map<std::array<std::int32_t, 3>, Block> blocks;
    std::int32_t min_x = std::numeric_limits<std::int32_t>::max(), min_y = min_x, min_z = min_x;
    std::int32_t max_x = std::numeric_limits<std::int32_t>::min(), max_y = max_x, max_z = max_x;
    std::string line;
    std::size_t line_number = 0;
    auto add = [&](std::int32_t x, std::int32_t y, std::int32_t z, std::string name, std::string state_text) -> Result<void> {
        auto parsed_states = states(state_text);
        if (!parsed_states) return Result<void>::failure("第 " + std::to_string(line_number) + " 行: 状态格式无效");
        const auto runtime = mRegistry.find(name, *parsed_states);
        const auto runtime_id = runtime.value_or(mRegistry.register_state(BlockState{ std::move(name), std::move(*parsed_states), 0 }));
        blocks[{ x, y, z }] = { x, y, z, runtime_id };
        min_x = std::min(min_x, x); min_y = std::min(min_y, y); min_z = std::min(min_z, z);
        max_x = std::max(max_x, x); max_y = std::max(max_y, y); max_z = std::max(max_z, z);
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
            const auto name_and_state = fields[name_index];
            const auto open = name_and_state.find('[');
            const auto block_name = open == std::string::npos ? name_and_state : name_and_state.substr(0, open);
            const auto state_text = open == std::string::npos ? std::string{} : name_and_state.substr(open);
            for (auto x = std::min(*x1, *x2); x <= std::max(*x1, *x2); ++x)
                for (auto y = std::min(*y1, *y2); y <= std::max(*y1, *y2); ++y)
                    for (auto z = std::min(*z1, *z2); z <= std::max(*z1, *z2); ++z) {
                        auto result = add(x, y, z, block_name, state_text); if (!result) return result;
                    }
        } else {
            const auto x = parse_coord(1), y = parse_coord(2), z = parse_coord(3);
            if (!x || !y || !z) return Result<void>::failure("第 " + std::to_string(line_number) + " 行: 坐标无效");
            const auto name_and_state = fields[name_index];
            const auto open = name_and_state.find('[');
            auto result = add(*x, *y, *z, open == std::string::npos ? name_and_state : name_and_state.substr(0, open), open == std::string::npos ? std::string{} : name_and_state.substr(open));
            if (!result) return result;
        }
    }
    if (blocks.empty()) return Result<void>::failure("MCFunction 不包含 setblock 或 fill 命令");
    mBlocks.clear(); mBlocks.reserve(blocks.size());
    for (const auto& [_, block] : blocks) mBlocks.push_back({ block.x - min_x, block.y - min_y, block.z - min_z, block.runtime_id });
    mOriginalSize = { max_x - min_x + 1, max_y - min_y + 1, max_z - min_z + 1 };
    mNonAirBlocks = 0; for (const auto& block : mBlocks) if (block.runtime_id != mRegistry.air_runtime_id()) ++mNonAirBlocks;
    set_offset({});
    return Result<void>::success();
}

Result<ChunkMap> McFunctionStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result; for (const auto pos : positions) result.emplace(pos, ChunkData{});
    for (const auto& block : mBlocks) {
        const auto x = block.x + mOffset.x, y = block.y + mOffset.y, z = block.z + mOffset.z;
        const ChunkPos chunk{ floor_div(x, 16), floor_div(z, 16) }; auto it = result.find(chunk); if (it == result.end()) continue;
        const auto sub_y = floor_div(y - 64, 16); auto [sub, inserted] = it->second.sub_chunks.try_emplace(sub_y);
        if (inserted) { sub->second.layer0.fill(mRegistry.air_runtime_id()); sub->second.layer1.fill(mRegistry.air_runtime_id()); }
        const auto lx = x - chunk.x * 16, ly = y - (sub_y * 16 + 64), lz = z - chunk.z * 16;
        sub->second.layer0[static_cast<std::size_t>((ly * 16 + lz) * 16 + lx)] = block.runtime_id;
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> McFunctionStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result; for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> McFunctionStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const { return convert_to_world(*this, world, start, std::move(callbacks)); }
Result<void> McFunctionStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks) { return Result<void>::failure("MCFunction 导出尚未迁移"); }

} // namespace water_structure
