#include "covstructure.hpp"

#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

struct PaletteEntry { std::string name; std::optional<std::int64_t> data; };

std::optional<std::int64_t> integer(const nlohmann::json& value)
{
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return static_cast<std::int64_t>(number);
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (std::isfinite(number) && number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) return static_cast<std::int64_t>(number);
    }
    if (value.is_string()) {
        auto text = value.get<std::string>();
        const auto first = text.find_first_not_of(" \t\r\n");
        const auto last = text.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) return std::nullopt;
        text = text.substr(first, last - first + 1);
        std::int64_t result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return result;
    }
    return std::nullopt;
}

std::vector<std::int32_t> dimensions(const nlohmann::json& value)
{
    std::vector<std::int32_t> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (const auto number = integer(item); number && *number >= std::numeric_limits<std::int32_t>::min() &&
            *number <= std::numeric_limits<std::int32_t>::max()) result.push_back(static_cast<std::int32_t>(*number));
    }
    return result;
}

bool has_palette_key(const nlohmann::json& value)
{
    if (!value.is_object()) return false;
    for (const auto key : { "name", "states", "properties", "id", "block", "data", "meta" })
        if (value.contains(key)) return true;
    return false;
}

std::vector<nlohmann::json> palette_entries(const nlohmann::json& raw)
{
    std::vector<nlohmann::json> result;
    if (raw.is_array()) {
        for (const auto& item : raw) if (item.is_object()) result.push_back(item);
        return result;
    }
    if (!raw.is_object()) return result;
    for (const auto& [_, value] : raw.items()) {
        if (!value.is_object()) continue;
        const auto found = value.find("block_palette");
        if (found != value.end() && found->is_array()) {
            for (const auto& item : *found) if (item.is_object()) result.push_back(item);
            return result;
        }
    }
    for (const auto& [_, value] : raw.items()) if (has_palette_key(value)) result.push_back(value);
    return result;
}

PaletteEntry decode_palette_entry(const nlohmann::json& value)
{
    PaletteEntry result{ "minecraft:air", std::nullopt };
    if (!value.is_object()) return result;
    for (const auto key : { "name", "block", "id" }) {
        const auto found = value.find(key);
        if (found != value.end() && found->is_string() && !found->get_ref<const std::string&>().empty()) {
            result.name = found->get<std::string>(); break;
        }
    }
    for (const auto key : { "data", "meta", "damage", "value" }) {
        const auto found = value.find(key);
        if (found != value.end()) if (const auto parsed = integer(*found)) { result.data = parsed; break; }
    }
    return result;
}

void flatten(const nlohmann::json& value, std::vector<const nlohmann::json*>& output)
{
    if (value.is_array()) {
        for (const auto& item : value) flatten(item, output);
    } else {
        output.push_back(&value);
    }
}

std::uint32_t runtime_id(RuntimeRegistry& registry, const PaletteEntry& entry)
{
    if (entry.data) if (const auto runtime = registry.legacy_runtime_id(entry.name,
        static_cast<std::uint16_t>(*entry.data))) return *runtime;
    if (const auto runtime = registry.find(entry.name)) return *runtime;
    if (const auto runtime = registry.java_runtime_id(entry.name)) return *runtime;
    if (const auto unknown = registry.find("minecraft:unknown")) return *unknown;
    return registry.register_state({ "minecraft:unknown", {}, 0 });
}

} // namespace

Result<void> CovStructureReader::read(const std::filesystem::path& path)
{
    mStore.clear(); mNonAirBlocks = 0;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open CovStructure file: " + path.string());
        const auto root = nlohmann::json::parse(input);
        if (!root.is_object()) throw std::runtime_error("root is not an object");
        const auto size_value = root.contains("size") ? &root["size"] :
            (root.contains("dimensions") ? &root["dimensions"] : nullptr);
        if (!size_value) throw std::runtime_error("size is missing");
        const auto dims = dimensions(*size_value);
        if (dims.size() < 3 || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
            throw std::runtime_error("size is invalid");
        }
        const Size size{ dims[0], dims[1], dims[2] };
        if (size.volume() <= 0) throw std::runtime_error("size volume is invalid");
        mStore.set_size(size);

        const nlohmann::json* structure = nullptr;
        if (const auto found = root.find("structure"); found != root.end() && found->is_object()) structure = &*found;
        const nlohmann::json* palette_raw = nullptr;
        if (structure) if (const auto found = structure->find("palette"); found != structure->end()) palette_raw = &*found;
        if (!palette_raw) if (const auto found = root.find("palette"); found != root.end()) palette_raw = &*found;
        std::unordered_map<std::int64_t, PaletteEntry> palette;
        if (palette_raw) {
            const auto entries = palette_entries(*palette_raw);
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto explicit_index = entries[index].find("val");
                const auto id = explicit_index != entries[index].end() && integer(*explicit_index)
                    ? *integer(*explicit_index) : static_cast<std::int64_t>(index);
                palette[id] = decode_palette_entry(entries[index]);
            }
        }

        const nlohmann::json* indices = nullptr;
        if (structure) {
            if (const auto found = structure->find("block_indices"); found != structure->end()) indices = &*found;
            else if (const auto found = structure->find("blocks"); found != structure->end()) indices = &*found;
        }
        if (!indices) return Result<void>::success();
        std::vector<const nlohmann::json*> flat;
        flatten(*indices, flat);

        auto resolve = [&](const nlohmann::json& raw) -> std::optional<PaletteEntry> {
            if (raw.is_object()) return decode_palette_entry(raw);
            if (const auto id = integer(raw)) {
                const auto found = palette.find(*id);
                if (found != palette.end()) return found->second;
            }
            return std::nullopt;
        };
        for (const auto* raw : flat) {
            const auto entry = resolve(*raw);
            if (!entry || entry->name.empty() || entry->name == "minecraft:air") continue;
            ++mNonAirBlocks;
        }

        const auto volume = static_cast<std::size_t>(size.volume());
        for (std::size_t index = 0; index < flat.size() && index < volume; ++index) {
            if (flat[index]->is_null()) continue;
            if (const auto value = integer(*flat[index]); value && *value == -1) continue;
            const auto entry = resolve(*flat[index]);
            if (!entry || entry->name.empty() || entry->name == "minecraft:air") continue;
            const auto x = static_cast<std::int32_t>(index % static_cast<std::size_t>(size.width));
            const auto z = static_cast<std::int32_t>((index / static_cast<std::size_t>(size.width)) %
                static_cast<std::size_t>(size.length));
            const auto y = static_cast<std::int32_t>(index /
                (static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.length)));
            mStore.put({ x, y, z }, runtime_id(mRegistry, *entry));
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("parse CovStructure failed: " + std::string(error.what()));
    }
}

Result<void> CovStructureReader::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> CovStructureReader::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("CovStructure has no Go FromMCWorld capability");
}

} // namespace water_structure
