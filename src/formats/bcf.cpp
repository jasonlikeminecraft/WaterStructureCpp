#include "bcf.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
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

constexpr std::uint64_t kMaxTableEntries = 1ull << 24;
constexpr std::uint64_t kMaxExpandedBlocks = 1ull << 28;
// One compact region is ~28 bytes plus allocator overhead. Keep the retained
// cuboid index bounded so a deliberately fragmented file cannot consume the
// entire test process before chunk materialization starts.
constexpr std::size_t kMaxRetainedRegions = 4'000'000;
constexpr std::size_t kOffsetBatchSize = 4096;

class Reader {
public:
    Reader(std::ifstream& input, std::uint64_t size) : mInput(input), mSize(size) {}
    std::uint64_t position() const noexcept { return mPosition; }
    void seek(std::uint64_t offset, std::string_view field) {
        if (offset > mSize || offset > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max())) fail(field);
        mInput.clear();
        mInput.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!mInput) fail(field);
        mPosition = offset;
    }
    std::uint8_t u8(std::string_view field) {
        std::uint8_t value = 0;
        read_into(&value, 1, field);
        return value;
    }
    std::uint16_t u16(std::string_view field) {
        std::array<std::uint8_t, 2> bytes{};
        read_into(bytes.data(), bytes.size(), field);
        return static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8);
    }
    std::int16_t i16(std::string_view field) { return static_cast<std::int16_t>(u16(field)); }
    std::uint32_t u32(std::string_view field) {
        std::array<std::uint8_t, 4> bytes{};
        read_into(bytes.data(), bytes.size(), field);
        std::uint32_t value = 0;
        for (unsigned index = 0; index < bytes.size(); ++index) {
            value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
        }
        return value;
    }
    std::uint64_t u64(std::string_view field) {
        std::array<std::uint8_t, 8> bytes{};
        read_into(bytes.data(), bytes.size(), field);
        std::uint64_t value = 0;
        for (unsigned index = 0; index < bytes.size(); ++index) {
            value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
        }
        return value;
    }
    std::string string16(std::string_view field) {
        return bytes(u16(field), field);
    }
    std::string bytes(std::size_t length, std::string_view field) {
        std::string value(length, '\0');
        if (length != 0) {
            read_into(reinterpret_cast<std::uint8_t*>(value.data()), length, field);
        }
        return value;
    }
private:
    [[noreturn]] void fail(std::string_view field) const {
        throw std::runtime_error("BCF " + std::string(field) + " truncated at offset " + std::to_string(mPosition));
    }
    void require(std::size_t length, std::string_view field) const {
        if (mPosition > mSize || static_cast<std::uint64_t>(length) > mSize - mPosition) fail(field);
    }
    void read_into(std::uint8_t* output, std::size_t length, std::string_view field) {
        require(length, field);
        if (length > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) fail(field);
        mInput.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(length));
        const auto count = mInput.gcount();
        if (count > 0) mPosition += static_cast<std::uint64_t>(count);
        if (count != static_cast<std::streamsize>(length)) fail(field);
    }
    std::ifstream& mInput;
    std::uint64_t mSize = 0;
    std::uint64_t mPosition = 0;
};

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

} // namespace

void BcfStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    const auto expanded = [](std::int32_t base, std::int32_t delta) noexcept {
        const auto magnitude = delta < 0
            ? -static_cast<std::int64_t>(delta)
            : static_cast<std::int64_t>(delta);
        const auto value = static_cast<std::int64_t>(base) + magnitude;
        return value > std::numeric_limits<std::int32_t>::max()
            ? std::numeric_limits<std::int32_t>::max()
            : static_cast<std::int32_t>(value);
    };
    mSize = {
        expanded(mOriginalSize.width, offset.x),
        expanded(mOriginalSize.height, offset.y),
        expanded(mOriginalSize.length, offset.z)
    };
}

Result<void> BcfStructure::read(const std::filesystem::path& path)
{
    mRegions.clear();
    mOriginalSize = {};
    mSize = {};
    mOffset = {};
    mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open BCF file: " + path.string());
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0) throw std::runtime_error("cannot determine BCF input size");
        const auto file_size = static_cast<std::uint64_t>(end);
        Reader reader(input, file_size);
        reader.seek(0, "header");
        if (reader.bytes(3, "magic") != "BCF") throw std::runtime_error("invalid BCF magic");
        (void)reader.u8("version");
        (void)reader.u16("width"); (void)reader.u16("length"); (void)reader.u16("height");
        (void)reader.u8("subchunk base size"); (void)reader.u64("subchunk count");
        const auto offsets_table = reader.u64("offset table pointer");
        const auto palette_offset = reader.u64("palette pointer");
        const auto type_map_offset = reader.u64("type map pointer");
        const auto state_name_offset = reader.u64("state name map pointer");
        const auto state_value_offset = reader.u64("state value map pointer");
        if (offsets_table == 0 || offsets_table >= file_size) throw std::runtime_error("BCF offset table pointer is invalid");

        reader.seek(offsets_table, "offset table");
        const auto offset_count = reader.u64("offset count");
        if (offset_count > kMaxTableEntries) throw std::runtime_error("BCF offset count exceeds limit");
        if (offsets_table > file_size || file_size - offsets_table < 8 ||
            offset_count > (file_size - offsets_table - 8) / 8) {
            throw std::runtime_error("BCF offset table is truncated");
        }

        auto read_type_map = [&] {
            std::unordered_map<std::uint16_t, std::string> result;
            if (type_map_offset == 0 || type_map_offset >= file_size) return result;
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
            if (offset == 0 || offset >= file_size) return result;
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
        if (palette_offset != 0 && palette_offset < file_size) {
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

        BlockPos bounds_min{}, bounds_max{};
        bool populated = false;
        std::uint64_t expanded = 0;
        std::size_t non_air_regions = 0;
        auto for_each_section_offset = [&](auto&& callback) {
            std::array<std::uint64_t, kOffsetBatchSize> offsets{};
            for (std::uint64_t batch_start = 0; batch_start < offset_count;
                 batch_start += kOffsetBatchSize) {
                const auto batch_size = static_cast<std::size_t>(std::min<std::uint64_t>(
                    kOffsetBatchSize, offset_count - batch_start));
                reader.seek(offsets_table + 8 + batch_start * 8, "offset table batch");
                for (std::size_t index = 0; index < batch_size; ++index) {
                    offsets[index] = reader.u64("section offset");
                }
                for (std::size_t index = 0; index < batch_size; ++index) {
                    callback(static_cast<std::size_t>(batch_start + index), offsets[index]);
                }
            }
        };
        auto scan_sections = [&](bool materialize) {
            for_each_section_offset([&](std::size_t section_index, std::uint64_t offset) {
                if (offset == 0 || offset >= file_size) return;
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
                        if (found == palette.end() || found->second == 0 ||
                            found->second == mRegistry.air_runtime_id()) continue;
                        Region region;
                        region.runtime = found->second;
                        region.minimum = { origin.x + std::min(first.x, second.x), origin.y + std::min(first.y, second.y),
                            origin.z + std::min(first.z, second.z) };
                        region.maximum = { origin.x + std::max(first.x, second.x), origin.y + std::max(first.y, second.y),
                            origin.z + std::max(first.z, second.z) };
                        if (materialize) {
                            if (mRegions.size() >= kMaxRetainedRegions) {
                                throw std::runtime_error("BCF non-air region count exceeds limit");
                            }
                            region.minimum = {
                                region.minimum.x - bounds_min.x,
                                region.minimum.y - bounds_min.y,
                                region.minimum.z - bounds_min.z
                            };
                            region.maximum = {
                                region.maximum.x - bounds_min.x,
                                region.maximum.y - bounds_min.y,
                                region.maximum.z - bounds_min.z
                            };
                            mRegions.push_back(region);
                            continue;
                        }
                        const auto volume = static_cast<std::uint64_t>(region.maximum.x - region.minimum.x + 1) *
                            static_cast<std::uint64_t>(region.maximum.y - region.minimum.y + 1) *
                            static_cast<std::uint64_t>(region.maximum.z - region.minimum.z + 1);
                        if (volume > kMaxExpandedBlocks ||
                            expanded > kMaxExpandedBlocks - volume) {
                            throw std::runtime_error("expanded block count exceeds limit");
                        }
                        expanded += volume;
                        const auto go_span = [](std::int32_t first_value, std::int32_t second_value) {
                            const auto value = static_cast<std::int64_t>(second_value) - first_value + 1;
                            return static_cast<std::uint64_t>(value < 0 ? -value : value);
                        };
                        const auto saturating_multiply = [](std::uint64_t left, std::uint64_t right) {
                            if (left == 0 || right == 0) return std::uint64_t{ 0 };
                            if (left > std::numeric_limits<std::uint64_t>::max() / right) {
                                return std::numeric_limits<std::uint64_t>::max();
                            }
                            return left * right;
                        };
                        const auto counted_volume = saturating_multiply(
                            saturating_multiply(go_span(first.x, second.x),
                                go_span(first.y, second.y)), go_span(first.z, second.z));
                        const auto add_count = std::min<std::uint64_t>(
                            counted_volume, std::numeric_limits<std::size_t>::max());
                        mNonAirBlocks = mNonAirBlocks > std::numeric_limits<std::size_t>::max() - add_count
                            ? std::numeric_limits<std::size_t>::max()
                            : mNonAirBlocks + static_cast<std::size_t>(add_count);
                        if (!populated) {
                            populated = true; bounds_min = region.minimum; bounds_max = region.maximum;
                        } else {
                            bounds_min.x = std::min(bounds_min.x, region.minimum.x);
                            bounds_min.y = std::min(bounds_min.y, region.minimum.y);
                            bounds_min.z = std::min(bounds_min.z, region.minimum.z);
                            bounds_max.x = std::max(bounds_max.x, region.maximum.x);
                            bounds_max.y = std::max(bounds_max.y, region.maximum.y);
                            bounds_max.z = std::max(bounds_max.z, region.maximum.z);
                        }
                        if (non_air_regions >= kMaxRetainedRegions) {
                            throw std::runtime_error("BCF non-air region count exceeds limit");
                        }
                        ++non_air_regions;
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error("section index " + std::to_string(section_index) + ": " + error.what());
                }
            });
        };
        scan_sections(false);
        if (!populated || non_air_regions == 0) throw std::runtime_error("BCF has no non-air regions");
        const auto width = static_cast<std::int64_t>(bounds_max.x) - bounds_min.x + 1;
        const auto height = static_cast<std::int64_t>(bounds_max.y) - bounds_min.y + 1;
        const auto length = static_cast<std::int64_t>(bounds_max.z) - bounds_min.z + 1;
        if (width > std::numeric_limits<std::int32_t>::max() || height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("BCF bounds exceed int32");
        mOriginalSize = { static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height), static_cast<std::int32_t>(length) };
        mSize = mOriginalSize;
        mRegions.reserve(std::min<std::size_t>(non_air_regions, kMaxRetainedRegions));
        scan_sections(true);
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse BCF failed: " + std::string(error.what()));
    }
}

Result<ChunkMap> BcfStructure::get_chunks(
    std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, true);
}

Result<ChunkMap> BcfStructure::get_chunks_layer0(
    std::span<const ChunkPos> positions) const
{
    return get_chunks_impl(positions, false);
}

Result<ChunkMap> BcfStructure::get_chunks_impl(
    std::span<const ChunkPos> positions, bool include_layer1) const
{
    ChunkMap result;
    for (const auto position : positions) result.emplace(position, ChunkData{});
    const auto air = mRegistry.air_runtime_id();
    // Regions remain in source order, so overlapping cuboids preserve the
    // original last-record-wins behaviour without expanding the entire BCF
    // into one std::map node per block.
    for (auto& [chunk_position, chunk] : result) {
        const auto chunk_min_x = static_cast<std::int64_t>(chunk_position.x) * 16;
        const auto chunk_min_z = static_cast<std::int64_t>(chunk_position.z) * 16;
        const auto chunk_max_x = chunk_min_x + 15;
        const auto chunk_max_z = chunk_min_z + 15;
        for (const auto& region : mRegions) {
            const auto region_min_x = static_cast<std::int64_t>(region.minimum.x) + mOffset.x;
            const auto region_min_y = static_cast<std::int64_t>(region.minimum.y) + mOffset.y;
            const auto region_min_z = static_cast<std::int64_t>(region.minimum.z) + mOffset.z;
            const auto region_max_x = static_cast<std::int64_t>(region.maximum.x) + mOffset.x;
            const auto region_max_y = static_cast<std::int64_t>(region.maximum.y) + mOffset.y;
            const auto region_max_z = static_cast<std::int64_t>(region.maximum.z) + mOffset.z;
            const auto minimum_x = std::max(region_min_x, chunk_min_x);
            const auto maximum_x = std::min(region_max_x, chunk_max_x);
            const auto minimum_z = std::max(region_min_z, chunk_min_z);
            const auto maximum_z = std::min(region_max_z, chunk_max_z);
            if (minimum_x > maximum_x || minimum_z > maximum_z) continue;
            if (region_min_y < std::numeric_limits<std::int32_t>::min() ||
                region_max_y > std::numeric_limits<std::int32_t>::max()) {
                return Result<ChunkMap>::failure("BCF offset moves Y outside int32 range");
            }
            for (auto y = region_min_y; y <= region_max_y; ++y) {
                const auto storage_y = y + kOverworldMinY;
                const auto sub_y_64 = floor_div64(storage_y, 16);
                if (sub_y_64 < std::numeric_limits<std::int32_t>::min() ||
                    sub_y_64 > std::numeric_limits<std::int32_t>::max()) {
                    return Result<ChunkMap>::failure("BCF subchunk Y exceeds int32 range");
                }
                const auto sub_y = static_cast<std::int32_t>(sub_y_64);
                auto [subchunk, inserted] = chunk.sub_chunks.try_emplace(sub_y);
                if (inserted) {
                    subchunk->second.layer0.fill(air);
                    if (include_layer1) subchunk->second.layer1.fill(air);
                }
                const auto local_y_64 = storage_y - sub_y_64 * 16;
                if (local_y_64 < 0 || local_y_64 >= 16) {
                    return Result<ChunkMap>::failure(
                        "BCF block materializes outside subchunk: y=" +
                        std::to_string(y) + ", subY=" + std::to_string(sub_y));
                }
                const auto local_y = static_cast<std::int32_t>(local_y_64);
                for (auto x = minimum_x; x <= maximum_x; ++x) {
                    const auto local_x = static_cast<std::int32_t>(x - chunk_min_x);
                    for (auto z = minimum_z; z <= maximum_z; ++z) {
                        const auto local_z = static_cast<std::int32_t>(z - chunk_min_z);
                        const auto index = static_cast<std::size_t>(
                            (local_y * 16 + local_z) * 16 + local_x);
                        subchunk->second.layer0[index] = region.runtime;
                    }
                }
            }
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<NbtChunkMap> BcfStructure::get_chunk_nbt(
    std::span<const ChunkPos> positions) const
{
    NbtChunkMap result;
    for (const auto position : positions) {
        result.emplace(position, std::vector<BlockEntity>{});
    }
    return Result<NbtChunkMap>::success(std::move(result));
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
