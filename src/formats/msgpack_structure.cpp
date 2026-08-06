#include "msgpack_structure.hpp"

#include <WaterStructure/world.hpp>

#include <msgpack.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kMaxInputBytes = 2ull * 1024 * 1024 * 1024;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open MessagePack file: " + path.string());
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > kMaxInputBytes) {
        throw std::runtime_error("MessagePack input exceeds 2 GiB");
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty()) input.read(reinterpret_cast<char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    if (!input && !input.eof()) throw std::runtime_error("read MessagePack input failed");
    return data;
}

std::int64_t integer(const msgpack::object& value, std::string_view field)
{
    switch (value.type) {
    case msgpack::type::POSITIVE_INTEGER:
        if (value.via.u64 > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) break;
        return static_cast<std::int64_t>(value.via.u64);
    case msgpack::type::NEGATIVE_INTEGER:
        return value.via.i64;
    default: break;
    }
    throw std::runtime_error(std::string(field) + " is not an integer");
}

std::string string_value(const msgpack::object& value, std::string_view field)
{
    if (value.type != msgpack::type::STR) {
        throw std::runtime_error(std::string(field) + " is not a string");
    }
    return { value.via.str.ptr, value.via.str.size };
}

bool boolean(const msgpack::object& value, std::string_view field)
{
    if (value.type == msgpack::type::BOOLEAN) return value.via.boolean;
    throw std::runtime_error(std::string(field) + " is not a boolean");
}

std::vector<msgpack::object> array(const msgpack::object& value, std::string_view field)
{
    if (value.type != msgpack::type::ARRAY) {
        throw std::runtime_error(std::string(field) + " is not an array");
    }
    return { value.via.array.ptr, value.via.array.ptr + value.via.array.size };
}

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool equal_fold(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

std::uint32_t unknown_runtime_id(RuntimeRegistry& registry)
{
    if (const auto unknown = registry.find("minecraft:unknown")) return *unknown;
    return registry.register_state({ "minecraft:unknown", {}, 0 });
}

std::uint32_t base_runtime_id(RuntimeRegistry& registry, const std::string& name)
{
    if (const auto found = registry.find(name)) return *found;
    if (const auto found = registry.compatible_java_runtime_id(name)) return *found;
    return unknown_runtime_id(registry);
}

std::optional<std::int64_t> decimal_string(std::string_view text)
{
    if (text.empty()) return std::nullopt;
    if (text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return std::nullopt;
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return result;
}

bool valid_state_string(std::string_view text)
{
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') return false;
    text.remove_prefix(1);
    text.remove_suffix(1);
    if (text.empty()) return true;
    bool quoted = false;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index < text.size() && text[index] == '"') quoted = !quoted;
        if (index < text.size() && (text[index] != ',' || quoted)) continue;
        const auto part = text.substr(begin, index - begin);
        const auto equal = part.find('=');
        if (equal == std::string_view::npos || trim(std::string(part.substr(0, equal))).empty()) {
            return false;
        }
        begin = index + 1;
    }
    return !quoted;
}

std::uint32_t legacy_runtime_id(
    RuntimeRegistry& registry, const std::string& name, std::int64_t data)
{
    if (const auto found = registry.legacy_runtime_id(name, static_cast<std::uint16_t>(data))) {
        return *found;
    }
    return unknown_runtime_id(registry);
}

std::uint32_t bds_runtime_id(
    RuntimeRegistry& registry, const std::string& name, const msgpack::object& data)
{
    if (data.type == msgpack::type::POSITIVE_INTEGER ||
        data.type == msgpack::type::NEGATIVE_INTEGER) {
        return legacy_runtime_id(registry, name, integer(data, "block data"));
    }
    if (data.type != msgpack::type::STR) return base_runtime_id(registry, name);

    const auto encoded = trim(std::string(data.via.str.ptr, data.via.str.size));
    if (encoded.empty()) return base_runtime_id(registry, name);
    if (const auto numeric = decimal_string(encoded)) {
        return legacy_runtime_id(registry, name, *numeric);
    }
    if (!valid_state_string(encoded)) return base_runtime_id(registry, name);
    if (encoded == "[]") return base_runtime_id(registry, name);
    if (const auto found = registry.java_runtime_id(name + encoded)) return *found;
    if (const auto found = registry.compatible_java_runtime_id(name + encoded)) return *found;
    return unknown_runtime_id(registry);
}

std::uint32_t nexus_runtime_id(
    RuntimeRegistry& registry, const std::string& name, const msgpack::object& data)
{
    return legacy_runtime_id(registry, name, integer(data, "block data"));
}

std::int32_t coordinate(const msgpack::object& value, std::string_view field)
{
    const auto decoded = integer(value, field);
    if (decoded < std::numeric_limits<std::int32_t>::min() ||
        decoded > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(decoded);
}

} // namespace

std::string_view MsgpackStructure::name() const noexcept
{
    return mFormat == StructureId::BDS ? "BDS" : "NexusNP";
}

Result<void> MsgpackStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    try {
        const auto bytes = read_file(path);
        msgpack::object_handle handle = msgpack::unpack(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto root = handle.get();
        const auto top = array(root, "top-level array");
        if (top.size() < (mFormat == StructureId::BDS ? 1u : 2u)) {
            throw std::runtime_error("top-level array is too short");
        }
        const auto blocks = array(top[0], mFormat == StructureId::BDS ? "BDS blocks" : "NP block_data");
        if (blocks.empty()) throw std::runtime_error("block array is empty");

        struct Pending { BlockPos pos{}; std::uint32_t runtime = 0; };
        std::map<BlockPos, Pending, std::less<>> accumulated;
        BlockPos minimum{}, maximum{};
        bool populated = false;
        auto add_bounds = [&](BlockPos pos) {
            if (!populated) { populated = true; minimum = maximum = pos; return; }
            minimum.x = std::min(minimum.x, pos.x); minimum.y = std::min(minimum.y, pos.y);
            minimum.z = std::min(minimum.z, pos.z); maximum.x = std::max(maximum.x, pos.x);
            maximum.y = std::max(maximum.y, pos.y); maximum.z = std::max(maximum.z, pos.z);
        };

        for (std::size_t index = 0; index < blocks.size(); ++index) {
            try {
                const auto entry = array(blocks[index], "block entry");
                const auto minimum_fields = mFormat == StructureId::BDS ? 6u : 5u;
                if (entry.size() < minimum_fields) throw std::runtime_error("block entry is too short");
                const auto raw_block_name = string_value(entry[0], "block name");
                const auto block_name = trim(raw_block_name);
                const BlockPos pos{
                    coordinate(entry[1], "x"), coordinate(entry[2], "y"), coordinate(entry[3], "z") };
                const auto data_index = 4u;
                const auto runtime = mFormat == StructureId::BDS
                    ? bds_runtime_id(mRegistry, block_name, entry[data_index])
                    : nexus_runtime_id(mRegistry, block_name, entry[data_index]);
                bool air = false;
                if (mFormat == StructureId::BDS) air = boolean(entry[5], "air flag");
                // Go only trims to detect an empty name; EqualFold is performed on
                // the original string, so a space-padded air name still contributes
                // to bounds/count while resolving to the air runtime ID.
                if (air || block_name.empty() || equal_fold(raw_block_name, "minecraft:air")) continue;
                accumulated[pos] = { pos, runtime };
                add_bounds(pos);
                ++mNonAirBlocks;
                for (std::size_t extra = minimum_fields; extra < entry.size(); ++extra) {
                    if (entry[extra].type == msgpack::type::NIL) continue;
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(name()) + " block index " +
                    std::to_string(index) + ": " + error.what());
            }
        }
        if (!populated || accumulated.empty()) throw std::runtime_error("structure has no non-air blocks");
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (width <= 0 || height <= 0 || length <= 0 || width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() || length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("structure bounds are invalid");
        }
        mStore.set_size({ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) });
        for (const auto& [world, pending] : accumulated) {
            mStore.put({ world.x - minimum.x, world.y - minimum.y, world.z - minimum.z }, pending.runtime);
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse " + std::string(name()) + " failed: " + error.what());
    }
}

Result<void> MsgpackStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> MsgpackStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(std::string(name()) + " file writer is not implemented yet");
}

} // namespace water_structure
