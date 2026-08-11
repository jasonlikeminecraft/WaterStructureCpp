#include "bdx.hpp"

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
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>

namespace water_structure {
namespace {

void require_bytes(const std::vector<std::uint8_t>& data, std::size_t position, std::size_t count, const char* field)
{
    if (count > data.size() - std::min(position, data.size())) {
        throw std::runtime_error(std::string(field) + " truncated");
    }
}

std::uint8_t u8(const std::vector<std::uint8_t>& data, std::size_t& position, const char* field)
{
    require_bytes(data, position, 1, field);
    return data[position++];
}

std::int8_t i8(const std::vector<std::uint8_t>& data, std::size_t& position, const char* field)
{
    return static_cast<std::int8_t>(u8(data, position, field));
}

std::uint16_t u16(const std::vector<std::uint8_t>& data, std::size_t& position, const char* field)
{
    require_bytes(data, position, 2, field);
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[position]) << 8u | data[position + 1]);
    position += 2;
    return value;
}

std::int16_t i16(const std::vector<std::uint8_t>& data, std::size_t& position, const char* field)
{
    return static_cast<std::int16_t>(u16(data, position, field));
}

std::uint32_t u32(const std::vector<std::uint8_t>& data, std::size_t& position, const char* field)
{
    require_bytes(data, position, 4, field);
    const auto value = static_cast<std::uint32_t>(data[position]) << 24u |
        static_cast<std::uint32_t>(data[position + 1]) << 16u |
        static_cast<std::uint32_t>(data[position + 2]) << 8u |
        data[position + 3];
    position += 4;
    return value;
}

std::int32_t i32(const std::vector<std::uint8_t>& data, std::size_t& position, const char* field)
{
    return static_cast<std::int32_t>(u32(data, position, field));
}

std::string cstring(const std::vector<std::uint8_t>& data, std::size_t& position, const char* field)
{
    const auto begin = position;
    while (position < data.size() && data[position] != 0) ++position;
    if (position == data.size()) throw std::runtime_error(std::string(field) + " truncated");
    std::string value(reinterpret_cast<const char*>(data.data() + begin), position - begin);
    ++position;
    return value;
}

void skip_bytes(const std::vector<std::uint8_t>& data, std::size_t& position, std::size_t count, const char* field)
{
    require_bytes(data, position, count, field);
    position += count;
}

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> compressed)
{
    constexpr std::size_t chunk_size = 64u * 1024u;
    constexpr std::size_t minimum_limit = 64u * 1024u * 1024u;
    constexpr std::size_t maximum_limit = 1024u * 1024u * 1024u;
    constexpr std::size_t maximum_expansion_ratio = 4096u;

    const auto scaled_limit = compressed.size() > maximum_limit / maximum_expansion_ratio
        ? maximum_limit
        : compressed.size() * maximum_expansion_ratio;
    const auto output_limit = std::clamp(scaled_limit, minimum_limit, maximum_limit);

    using Decoder = std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>;
    Decoder decoder(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr),
        &BrotliDecoderDestroyInstance);
    if (!decoder) throw std::runtime_error("无法创建 BDX Brotli decoder");

    std::vector<std::uint8_t> output;
    output.reserve(std::min(output_limit, std::max(chunk_size, compressed.size() * 8)));
    std::array<std::uint8_t, chunk_size> chunk{};
    auto* next_input = compressed.data();
    auto available_input = compressed.size();

    while (true) {
        auto* next_output = chunk.data();
        auto available_output = chunk.size();
        const auto status = BrotliDecoderDecompressStream(
            decoder.get(),
            &available_input,
            &next_input,
            &available_output,
            &next_output,
            nullptr);
        const auto produced = chunk.size() - available_output;
        if (produced > output_limit - output.size()) {
            throw std::runtime_error(
                "BDX Brotli 解压输出超过限制 " + std::to_string(output_limit) + " bytes");
        }
        output.insert(output.end(), chunk.begin(), chunk.begin() + produced);

        if (status == BROTLI_DECODER_RESULT_SUCCESS) return output;
        if (status == BROTLI_DECODER_RESULT_ERROR) {
            const auto code = BrotliDecoderGetErrorCode(decoder.get());
            const auto* message = BrotliDecoderErrorString(code);
            throw std::runtime_error(std::string("BDX Brotli 解压失败: ") +
                (message ? message : "unknown error"));
        }
        if (status == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0) {
            throw std::runtime_error("BDX Brotli 数据截断");
        }
    }
}

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
    for (std::size_t index = 0; index <= contents.size(); ++index) {
        if (index < contents.size() && contents[index] == '"' &&
            (index == 0 || contents[index - 1] != '\\')) quoted = !quoted;
        if (index < contents.size() && (contents[index] != ',' || quoted)) continue;
        const auto part = contents.substr(start, index - start);
        std::size_t equals = std::string_view::npos;
        bool key_quoted = false;
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (part[i] == '"' && (i == 0 || part[i - 1] != '\\')) key_quoted = !key_quoted;
            if (part[i] == '=' && !key_quoted) {
                equals = i;
                break;
            }
        }
        if (equals == std::string_view::npos) {
            if (!trim(part).empty()) throw std::runtime_error("BDX block states 缺少 '='");
        } else {
            BlockStateProperty property;
            property.name = unquote(trim(part.substr(0, equals)));
            auto value = trim(part.substr(equals + 1));
            if (value == "true" || value == "false") {
                property.type = BlockStateValueType::Byte;
                property.value = value == "true" ? "1" : "0";
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

CommandBlockData read_command_block_data(const std::vector<std::uint8_t>& data, std::size_t& position)
{
    CommandBlockData result;
    result.mode = u32(data, position, "command block mode");
    result.command = cstring(data, position, "command block command");
    result.custom_name = cstring(data, position, "command block custom name");
    result.last_output = cstring(data, position, "command block last output");
    result.tick_delay = i32(data, position, "command block tick delay");
    result.execute_on_first_tick = u8(data, position, "command block execute flag") != 0;
    result.track_output = u8(data, position, "command block track flag") != 0;
    result.conditional = u8(data, position, "command block conditional flag") != 0;
    result.needs_redstone = u8(data, position, "command block redstone flag") != 0;
    return result;
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
    const std::vector<std::uint8_t>& data,
    std::size_t& position)
{
    const auto count = u8(data, position, "chest slot count");
    nbt::tag_list items(nbt::tag_type::Compound);
    for (std::uint16_t i = 0; i < count; ++i) {
        nbt::tag_compound item;
        item["Name"] = nbt::tag_string(cstring(data, position, "chest item name"));
        item["Count"] = nbt::tag_byte(u8(data, position, "chest item count"));
        item["Damage"] = nbt::tag_short(static_cast<std::int16_t>(u16(data, position, "chest item damage")));
        item["Slot"] = nbt::tag_byte(u8(data, position, "chest item slot"));
        items.push_back(std::move(item));
    }
    auto result = std::make_unique<nbt::tag_compound>();
    result->operator[]("Items") = std::move(items);
    return result;
}

std::unique_ptr<nbt::tag_compound> read_block_nbt(
    const std::vector<std::uint8_t>& data,
    std::size_t& position)
{
    require_bytes(data, position, 1, "block NBT");
    const std::string remaining(reinterpret_cast<const char*>(data.data() + position), data.size() - position);
    std::istringstream input(remaining, std::ios::binary);
    auto [_, compound] = nbt::io::read_compound(input, endian::little);
    const auto available = input.rdbuf()->in_avail();
    if (available < 0 || static_cast<std::size_t>(available) > remaining.size()) {
        throw std::runtime_error("无法确定 block NBT 长度");
    }
    position += remaining.size() - static_cast<std::size_t>(available);
    return std::move(compound);
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
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 BDX 文件: " + path.string());
    try {
        const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(input)), {});
        if (file.size() < 4 || file[0] != 'B' || file[1] != 'D' || file[2] != '@') {
            return Result<void>::failure("BDX 文件头不是 BD@");
        }
        auto decoded = decompress(std::span<const std::uint8_t>(file).subspan(3));
        std::size_t position = 0;
        if (u8(decoded, position, "metadata magic") != 'B' ||
            u8(decoded, position, "metadata magic") != 'D' ||
            u8(decoded, position, "metadata magic") != 'X') {
            return Result<void>::failure("BDX metadata 头不是 BDX");
        }
        mAuthor = cstring(decoded, position, "BDX author");
        (void)u8(decoded, position, "BDX version");

        std::map<std::uint16_t, std::string> constants;
        std::int32_t x = 0, y = 0, z = 0;
        std::int32_t min_x = 0, min_y = 0, min_z = 0;
        std::int32_t max_x = 0, max_y = 0, max_z = 0;
        std::uint64_t command_index = 0;
        bool terminated = false;
        mPoolId = 0;
        mBlocks.clear();
        mBlockEntities.clear();
        mNonAirBlocks = 0;

        auto constant = [&](std::uint16_t id) -> const std::string* {
            const auto found = constants.find(id);
            return found == constants.end() ? nullptr : &found->second;
        };
        auto legacy_runtime = [&](std::uint16_t name_id, std::uint16_t block_data) {
            const auto* name = constant(name_id);
            if (!name) return mRegistry.air_runtime_id();
            return mRegistry.legacy_runtime_id(*name, block_data)
                .value_or(mRegistry.find(*name).value_or(mRegistry.air_runtime_id()));
        };
        auto state_runtime = [&](std::uint16_t name_id, std::string_view states) {
            const auto* name = constant(name_id);
            if (!name) return mRegistry.air_runtime_id();
            BlockState state;
            state.name = *name;
            state.states = parse_block_states(states);
            if (const auto runtime = mRegistry.legacy_state_runtime_id(state.name, state.states)) return *runtime;
            if (const auto runtime = mRegistry.find_compatible(std::move(state))) return *runtime;
            return mRegistry.legacy_runtime_id(*name, 0)
                .value_or(mRegistry.find(*name).value_or(mRegistry.air_runtime_id()));
        };
        auto pool_runtime = [&](std::uint32_t index) {
            const auto runtime = mRegistry.legacy_runtime_id(mPoolId, index);
            if (!runtime) {
                throw std::runtime_error(
                    "runtime pool " + std::to_string(mPoolId) + " index " + std::to_string(index) + " 越界");
            }
            return *runtime;
        };
        auto store_entity = [&](std::unique_ptr<nbt::tag_compound> entity, std::uint32_t runtime_id) {
            if (!entity) return;
            if (const auto state = mRegistry.state(runtime_id)) {
                if (const auto id = block_entity_id(state->name); !id.empty()) {
                    entity->operator[]("id") = nbt::tag_string(id);
                }
            }
            mBlockEntities[{ x, y, z }] = serialize_compound(*entity);
        };
        auto place = [&](std::uint32_t runtime_id, std::unique_ptr<nbt::tag_compound> entity) {
            store_entity(std::move(entity), runtime_id);
            if (runtime_id == mRegistry.air_runtime_id()) return;
            mBlocks.push_back({ x, y, z, runtime_id });
            ++mNonAirBlocks;
        };
        auto update_bounds = [&] {
            min_x = std::min(min_x, x); min_y = std::min(min_y, y); min_z = std::min(min_z, z);
            max_x = std::max(max_x, x); max_y = std::max(max_y, y); max_z = std::max(max_z, z);
        };

        while (position < decoded.size()) {
            const auto command_offset = position;
            ++command_index;
            bool metadata = false;
            try {
                const auto id = u8(decoded, position, "command id");
                switch (id) {
                case 1: {
                    if (constants.size() > std::numeric_limits<std::uint16_t>::max()) {
                        throw std::runtime_error("constant string 表超过 uint16 范围");
                    }
                    constants.emplace(static_cast<std::uint16_t>(constants.size()),
                        cstring(decoded, position, "constant string"));
                    metadata = true;
                    break;
                }
                case 5: {
                    const auto name = u16(decoded, position, "block constant id");
                    const auto states = u16(decoded, position, "states constant id");
                    const auto* encoded = constant(states);
                    place(encoded ? state_runtime(name, *encoded) : mRegistry.air_runtime_id(), nullptr);
                    break;
                }
                case 6: z += i16(decoded, position, "int16 z"); break;
                case 7: {
                    const auto name = u16(decoded, position, "block constant id");
                    const auto data = u16(decoded, position, "legacy block data");
                    place(legacy_runtime(name, data), nullptr);
                    break;
                }
                case 8: ++z; break;
                case 9: break;
                case 12: z += i32(decoded, position, "int32 z"); break;
                case 13: {
                    const auto name = u16(decoded, position, "block constant id");
                    const auto states = cstring(decoded, position, "deprecated block states");
                    place(state_runtime(name, states), nullptr);
                    break;
                }
                case 14: ++x; break;
                case 15: --x; break;
                case 16: ++y; break;
                case 17: --y; break;
                case 18: ++z; break;
                case 19: --z; break;
                case 20: x += i16(decoded, position, "int16 x"); break;
                case 21: x += i32(decoded, position, "int32 x"); break;
                case 22: y += i16(decoded, position, "int16 y"); break;
                case 23: y += i32(decoded, position, "int32 y"); break;
                case 24: z += i16(decoded, position, "int16 z"); break;
                case 25: z += i32(decoded, position, "int32 z"); break;
                case 26: store_entity(command_block_nbt(read_command_block_data(decoded, position)), 0); break;
                case 27: {
                    const auto name = u16(decoded, position, "block constant id");
                    const auto data = u16(decoded, position, "legacy block data");
                    place(legacy_runtime(name, data), command_block_nbt(read_command_block_data(decoded, position)));
                    break;
                }
                case 28: x += i8(decoded, position, "int8 x"); break;
                case 29: y += i8(decoded, position, "int8 y"); break;
                case 30: z += i8(decoded, position, "int8 z"); break;
                case 31: mPoolId = u8(decoded, position, "runtime pool id"); metadata = true; break;
                case 32: place(pool_runtime(u16(decoded, position, "runtime pool index")), nullptr); break;
                case 33: (void)u32(decoded, position, "runtime pool index"); break;
                case 34: {
                    const auto runtime = pool_runtime(u16(decoded, position, "runtime pool index"));
                    place(runtime, command_block_nbt(read_command_block_data(decoded, position)));
                    break;
                }
                case 35: {
                    const auto runtime = pool_runtime(u32(decoded, position, "runtime pool index"));
                    place(runtime, command_block_nbt(read_command_block_data(decoded, position)));
                    break;
                }
                case 36: {
                    const auto data = u16(decoded, position, "command block data");
                    auto command = read_command_block_data(decoded, position);
                    if (command.mode > 2) throw std::runtime_error("command block mode 越界");
                    static constexpr std::array<std::string_view, 3> names{
                        "minecraft:command_block", "minecraft:repeating_command_block", "minecraft:chain_command_block"
                    };
                    const auto runtime = mRegistry.legacy_runtime_id(names[command.mode], data)
                        .value_or(mRegistry.find(names[command.mode]).value_or(mRegistry.air_runtime_id()));
                    place(runtime, command_block_nbt(command));
                    break;
                }
                case 37: {
                    const auto runtime = pool_runtime(u16(decoded, position, "runtime pool index"));
                    place(runtime, read_chest_nbt(decoded, position));
                    break;
                }
                case 38: {
                    const auto runtime = pool_runtime(u32(decoded, position, "runtime pool index"));
                    place(runtime, read_chest_nbt(decoded, position));
                    break;
                }
                case 39: skip_bytes(decoded, position, u32(decoded, position, "debug data length"), "debug data"); break;
                case 40: {
                    const auto name = u16(decoded, position, "block constant id");
                    const auto data = u16(decoded, position, "legacy block data");
                    place(legacy_runtime(name, data), read_chest_nbt(decoded, position));
                    break;
                }
                case 41: {
                    const auto name = u16(decoded, position, "block constant id");
                    const auto states = u16(decoded, position, "states constant id");
                    (void)u16(decoded, position, "NBT prefix");
                    const auto* encoded = constant(states);
                    place(encoded ? state_runtime(name, *encoded) : mRegistry.air_runtime_id(),
                        read_block_nbt(decoded, position));
                    break;
                }
                case 88: terminated = true; break;
                default: throw std::runtime_error("未知 command id=" + std::to_string(id));
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    "command #" + std::to_string(command_index) + " decoded offset " +
                    std::to_string(command_offset) + ": " + error.what());
            }
            if (!metadata) update_bounds();
            if (terminated) break;
        }
        if (!terminated) {
            throw std::runtime_error(
                "缺少 terminate command，最后 command #" + std::to_string(command_index) +
                " decoded offset " + std::to_string(position));
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
        mOriginalSize = { max_x - min_x + 1, max_y - min_y + 1, max_z - min_z + 1 };
        set_offset({});
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(std::string("解析 BDX 失败: ") + error.what());
    }
}

Result<ChunkMap> BdxStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    mChunkIndex.ensure(mBlocks, mOffset, [](const Block& block) {
        return BlockPos{ block.x, block.y, block.z };
    });
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
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> BdxStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("BDX 导出尚未迁移");
}

} // namespace water_structure
