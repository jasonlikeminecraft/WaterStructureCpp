#include "bcf.hpp"

#include <WaterStructure/world.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kMaxInputBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaxTableEntries = 1ull << 24;
constexpr std::uint64_t kMaxExpandedBlocks = 1ull << 28;

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : mData(data) {}
    std::size_t position() const noexcept { return mPosition; }
    void seek(std::uint64_t offset, std::string_view field) {
        if (offset > mData.size()) fail(field);
        mPosition = static_cast<std::size_t>(offset);
    }
    std::uint8_t u8(std::string_view field) { require(1, field); return mData[mPosition++]; }
    std::uint16_t u16(std::string_view field) {
        require(2, field); const auto value = static_cast<std::uint16_t>(mData[mPosition]) |
            static_cast<std::uint16_t>(mData[mPosition + 1] << 8); mPosition += 2; return value;
    }
    std::int16_t i16(std::string_view field) { return static_cast<std::int16_t>(u16(field)); }
    std::uint32_t u32(std::string_view field) {
        require(4, field); std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(mData[mPosition++]) << shift;
        return value;
    }
    std::uint64_t u64(std::string_view field) {
        require(8, field); std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(mData[mPosition++]) << shift;
        return value;
    }
    std::string string16(std::string_view field) {
        const auto length = u16(field); require(length, field);
        std::string value(reinterpret_cast<const char*>(mData.data() + mPosition), length);
        mPosition += length; return value;
    }
    std::string bytes(std::size_t length, std::string_view field) {
        require(length, field); std::string value(reinterpret_cast<const char*>(mData.data() + mPosition), length);
        mPosition += length; return value;
    }
private:
    [[noreturn]] void fail(std::string_view field) const {
        throw std::runtime_error("BCF " + std::string(field) + " truncated at offset " + std::to_string(mPosition));
    }
    void require(std::size_t length, std::string_view field) const {
        if (length > mData.size() - mPosition) fail(field);
    }
    std::span<const std::uint8_t> mData;
    std::size_t mPosition = 0;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open BCF file: " + path.string());
    input.seekg(0, std::ios::end); const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > kMaxInputBytes) throw std::runtime_error("BCF input exceeds 2 GiB");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) throw std::runtime_error("read BCF input failed");
    return bytes;
}

std::optional<std::uint32_t> resolve_palette(
    RuntimeRegistry& registry, const std::string& name,
    const std::map<std::string, std::string>& encoded_states)
{
    if (encoded_states.empty()) return registry.find(name);
    if (const auto base = registry.find(name)) {
        if (auto state = registry.state(*base)) {
            auto merged = state->states;
            for (auto& property : merged) {
                const auto found = encoded_states.find(property.name);
                if (found == encoded_states.end()) continue;
                auto value = found->second;
                if (value == "true") value = "1";
                else if (value == "false") value = "0";
                property.value = std::move(value);
            }
            if (const auto runtime = registry.find(name, merged)) return runtime;
        }
    }
    std::string java = name + '[';
    std::size_t index = 0;
    for (const auto& [key, value] : encoded_states) {
        if (index++ != 0) java.push_back(',');
        java += key + '=' + value;
    }
    java.push_back(']');
    return registry.java_runtime_id(java);
}

struct Region {
    std::uint32_t runtime = 0;
    BlockPos minimum{};
    BlockPos maximum{};
};

} // namespace

Result<void> BcfStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    try {
        const auto bytes = read_file(path);
        Reader reader(bytes);
        if (reader.bytes(3, "magic") != "BCF") throw std::runtime_error("invalid BCF magic");
        (void)reader.u8("version");
        (void)reader.u16("width"); (void)reader.u16("length"); (void)reader.u16("height");
        (void)reader.u8("subchunk base size"); (void)reader.u64("subchunk count");
        const auto offsets_table = reader.u64("offset table pointer");
        const auto palette_offset = reader.u64("palette pointer");
        const auto type_map_offset = reader.u64("type map pointer");
        const auto state_name_offset = reader.u64("state name map pointer");
        const auto state_value_offset = reader.u64("state value map pointer");
        if (offsets_table == 0 || offsets_table >= bytes.size()) throw std::runtime_error("BCF offset table pointer is invalid");

        reader.seek(offsets_table, "offset table");
        const auto offset_count = reader.u64("offset count");
        if (offset_count > kMaxTableEntries) throw std::runtime_error("BCF offset count exceeds limit");
        std::vector<std::uint64_t> section_offsets;
        section_offsets.reserve(static_cast<std::size_t>(offset_count));
        for (std::uint64_t index = 0; index < offset_count; ++index) {
            section_offsets.push_back(reader.u64("section offset"));
        }

        auto read_type_map = [&] {
            std::unordered_map<std::uint16_t, std::string> result;
            if (type_map_offset == 0 || type_map_offset >= bytes.size()) return result;
            reader.seek(type_map_offset, "type map"); const auto count = reader.u32("type count");
            if (count > kMaxTableEntries) throw std::runtime_error("BCF type count exceeds limit");
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto id = reader.u16("type id");
                result[id] = reader.string16("type name");
            }
            return result;
        };
        auto read_byte_string_map = [&](std::uint64_t offset, std::string_view field) {
            std::unordered_map<std::uint8_t, std::string> result;
            if (offset == 0 || offset >= bytes.size()) return result;
            reader.seek(offset, field); const auto count = reader.u32(field);
            if (count > kMaxTableEntries) throw std::runtime_error("BCF map count exceeds limit");
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto id = reader.u8(field);
                result[id] = reader.string16(field);
            }
            return result;
        };
        const auto type_map = read_type_map();
        const auto state_name_map = read_byte_string_map(state_name_offset, "state name map");
        const auto state_value_map = read_byte_string_map(state_value_offset, "state value map");

        std::unordered_map<std::uint32_t, std::uint32_t> palette;
        if (palette_offset != 0 && palette_offset < bytes.size()) {
            reader.seek(palette_offset, "palette"); const auto count = reader.u32("palette count");
            if (count > kMaxTableEntries) throw std::runtime_error("BCF palette count exceeds limit");
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto id = reader.u32("palette id");
                const auto type_id = reader.u16("palette type id");
                const auto state_count = reader.u16("palette state count");
                std::map<std::string, std::string> states;
                for (std::uint16_t state_index = 0; state_index < state_count; ++state_index) {
                    const auto state_id = reader.u8("palette state id");
                    const auto value_id = reader.u8("palette value id");
                    const auto state = state_name_map.find(state_id);
                    const auto value = state_value_map.find(value_id);
                    if (state != state_name_map.end() && value != state_value_map.end() &&
                        !state->second.empty() && !value->second.empty()) states[state->second] = value->second;
                }
                const auto type = type_map.find(type_id);
                const auto block_name = type == type_map.end() || type->second.empty()
                    ? std::string("minecraft:air") : type->second;
                if (const auto runtime = resolve_palette(mRegistry, block_name, states)) palette[id] = *runtime;
            }
        }

        std::vector<Region> regions;
        BlockPos bounds_min{}, bounds_max{};
        bool populated = false;
        std::uint64_t expanded = 0;
        for (std::size_t section_index = 0; section_index < section_offsets.size(); ++section_index) {
            const auto offset = section_offsets[section_index];
            if (offset == 0 || offset >= bytes.size()) continue;
            try {
                reader.seek(offset, "section"); (void)reader.u64("section size");
                const BlockPos origin{ reader.i16("section x"), reader.i16("section y"), reader.i16("section z") };
                const auto count = reader.u32("region count");
                if (count > kMaxTableEntries) throw std::runtime_error("region count exceeds limit");
                for (std::uint32_t index = 0; index < count; ++index) {
                    const auto palette_id = reader.u32("region palette id");
                    const BlockPos first{ reader.i16("region x1"), reader.i16("region y1"), reader.i16("region z1") };
                    const BlockPos second{ reader.i16("region x2"), reader.i16("region y2"), reader.i16("region z2") };
                    const auto found = palette.find(palette_id);
                    if (found == palette.end() || found->second == 0 || found->second == mRegistry.air_runtime_id()) continue;
                    Region region;
                    region.runtime = found->second;
                    region.minimum = { origin.x + std::min(first.x, second.x), origin.y + std::min(first.y, second.y),
                        origin.z + std::min(first.z, second.z) };
                    region.maximum = { origin.x + std::max(first.x, second.x), origin.y + std::max(first.y, second.y),
                        origin.z + std::max(first.z, second.z) };
                    const auto volume = static_cast<std::uint64_t>(region.maximum.x - region.minimum.x + 1) *
                        static_cast<std::uint64_t>(region.maximum.y - region.minimum.y + 1) *
                        static_cast<std::uint64_t>(region.maximum.z - region.minimum.z + 1);
                    if (expanded > kMaxExpandedBlocks - volume) throw std::runtime_error("expanded block count exceeds limit");
                    expanded += volume;
                    // The Go oracle counts reversed BCF regions using abs(end-start+1),
                    // even though materialization normalizes the endpoints with min/max.
                    // Keep that historical quirk in the manifest count.
                    const auto go_span = [](std::int32_t first_value, std::int32_t second_value) {
                        const auto value = static_cast<std::int64_t>(second_value) - first_value + 1;
                        return static_cast<std::uint64_t>(value < 0 ? -value : value);
                    };
                    const auto count_x = go_span(first.x, second.x);
                    const auto count_y = go_span(first.y, second.y);
                    const auto count_z = go_span(first.z, second.z);
                    const auto saturating_multiply = [](std::uint64_t left, std::uint64_t right) {
                        if (left == 0 || right == 0) return std::uint64_t{ 0 };
                        if (left > std::numeric_limits<std::uint64_t>::max() / right) {
                            return std::numeric_limits<std::uint64_t>::max();
                        }
                        return left * right;
                    };
                    const auto counted_volume = saturating_multiply(
                        saturating_multiply(count_x, count_y), count_z);
                    mNonAirBlocks = mNonAirBlocks > std::numeric_limits<std::size_t>::max() -
                        std::min<std::uint64_t>(counted_volume, std::numeric_limits<std::size_t>::max())
                        ? std::numeric_limits<std::size_t>::max()
                        : mNonAirBlocks + static_cast<std::size_t>(counted_volume);
                    if (!populated) { populated = true; bounds_min = region.minimum; bounds_max = region.maximum; }
                    else {
                        bounds_min.x = std::min(bounds_min.x, region.minimum.x); bounds_min.y = std::min(bounds_min.y, region.minimum.y);
                        bounds_min.z = std::min(bounds_min.z, region.minimum.z); bounds_max.x = std::max(bounds_max.x, region.maximum.x);
                        bounds_max.y = std::max(bounds_max.y, region.maximum.y); bounds_max.z = std::max(bounds_max.z, region.maximum.z);
                    }
                    regions.push_back(region);
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("section index " + std::to_string(section_index) + ": " + error.what());
            }
        }
        if (!populated || regions.empty()) throw std::runtime_error("BCF has no non-air regions");
        const auto width = static_cast<std::int64_t>(bounds_max.x) - bounds_min.x + 1;
        const auto height = static_cast<std::int64_t>(bounds_max.y) - bounds_min.y + 1;
        const auto length = static_cast<std::int64_t>(bounds_max.z) - bounds_min.z + 1;
        if (width > std::numeric_limits<std::int32_t>::max() || height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("BCF bounds exceed int32");
        mStore.set_size({ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) });
        for (const auto& region : regions) {
            for (std::int32_t x = region.minimum.x; x <= region.maximum.x; ++x)
                for (std::int32_t y = region.minimum.y; y <= region.maximum.y; ++y)
                    for (std::int32_t z = region.minimum.z; z <= region.maximum.z; ++z)
                        mStore.put({ x - bounds_min.x, y - bounds_min.y, z - bounds_min.z }, region.runtime);
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse BCF failed: " + std::string(error.what()));
    }
}

Result<void> BcfStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> BcfStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("BCF has no Go FromMCWorld capability");
}

} // namespace water_structure
