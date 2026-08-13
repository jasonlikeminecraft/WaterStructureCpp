#include <WaterStructure/runtime_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace water_structure {

namespace {

using PropertyMap = std::map<std::string, BlockStateProperty>;

struct ValueRemap {
    BlockStateProperty old_value;
    BlockStateProperty new_value;
};

struct FlattenedName {
    std::string prefix;
    std::string property;
    std::string suffix;
    std::unordered_map<std::string, std::string> value_remaps;
};

struct StateRemap {
    PropertyMap old_properties;
    std::string new_name;
    FlattenedName flattened;
    PropertyMap new_properties;
    std::vector<std::string> copied_properties;
};

struct UpgradeSchema {
    std::int32_t id = 0;
    std::unordered_map<std::string, std::string> renamed_ids;
    std::unordered_map<std::string, PropertyMap> added_properties;
    std::unordered_map<std::string, std::vector<std::string>> removed_properties;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> renamed_properties;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<ValueRemap>>> remapped_property_values;
    std::unordered_map<std::string, std::vector<StateRemap>> remapped_states;
};

std::optional<BlockStateProperty> schema_property(
    std::string name,
    const nlohmann::json& encoded)
{
    BlockStateProperty result;
    result.name = std::move(name);
    if (encoded.contains("byte") && encoded["byte"].is_number_integer()) {
        result.type = BlockStateValueType::Byte;
        result.value = std::to_string(encoded["byte"].get<std::uint8_t>());
    } else if (encoded.contains("int") && encoded["int"].is_number_integer()) {
        result.type = BlockStateValueType::Int;
        result.value = std::to_string(encoded["int"].get<std::int32_t>());
    } else if (encoded.contains("string") && encoded["string"].is_string()) {
        result.type = BlockStateValueType::String;
        result.value = encoded["string"].get<std::string>();
    } else {
        return std::nullopt;
    }
    return result;
}

PropertyMap schema_properties(const nlohmann::json& encoded)
{
    PropertyMap result;
    if (encoded.is_null()) return result;
    if (!encoded.is_object()) throw std::runtime_error("upgrade schema 属性不是 object");
    for (const auto& [name, value] : encoded.items()) {
        auto property = schema_property(name, value);
        if (!property) throw std::runtime_error("upgrade schema 属性类型无效: " + name);
        result.emplace(name, std::move(*property));
    }
    return result;
}

bool property_equal(const BlockStateProperty& left, const BlockStateProperty& right)
{
    return left.type == right.type && left.value == right.value;
}

PropertyMap property_map(std::span<const BlockStateProperty> properties)
{
    PropertyMap result;
    for (const auto& property : properties) result[property.name] = property;
    return result;
}

std::vector<BlockStateProperty> property_vector(PropertyMap properties)
{
    std::vector<BlockStateProperty> result;
    result.reserve(properties.size());
    for (auto& [_, property] : properties) result.push_back(std::move(property));
    return result;
}

const std::vector<ValueRemap>* value_remaps_for(
    const UpgradeSchema& schema,
    const std::string& block,
    const std::string& property)
{
    const auto block_it = schema.remapped_property_values.find(block);
    if (block_it == schema.remapped_property_values.end()) return nullptr;
    const auto property_it = block_it->second.find(property);
    return property_it == block_it->second.end() ? nullptr : &property_it->second;
}

BlockStateProperty remap_property_value(
    const UpgradeSchema& schema,
    const std::string& block,
    const std::string& property,
    BlockStateProperty value)
{
    const auto* remaps = value_remaps_for(schema, block, property);
    if (!remaps) return value;
    for (const auto& remap : *remaps) {
        if (property_equal(value, remap.old_value)) {
            auto result = remap.new_value;
            result.name = value.name;
            return result;
        }
    }
    return value;
}

Result<std::function<BlockState(BlockState)>> load_upgrade_schemas(const std::filesystem::path& directory)
{
    try {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        auto schemas = std::make_shared<std::vector<UpgradeSchema>>();
        schemas->reserve(files.size());
        for (const auto& file : files) {
            std::ifstream input(file, std::ios::binary);
            if (!input) throw std::runtime_error("无法读取 " + file.string());
            nlohmann::json root;
            input >> root;
            UpgradeSchema schema;
            schema.id = static_cast<std::int32_t>(
                (root.value("maxVersionMajor", 0) << 24) |
                (root.value("maxVersionMinor", 0) << 16) |
                (root.value("maxVersionPatch", 0) << 8) |
                root.value("maxVersionRevision", 0));
            if (root.contains("renamedIds")) {
                schema.renamed_ids = root["renamedIds"].get<decltype(schema.renamed_ids)>();
            }
            if (root.contains("addedProperties")) {
                for (const auto& [block, properties] : root["addedProperties"].items()) {
                    schema.added_properties.emplace(block, schema_properties(properties));
                }
            }
            if (root.contains("removedProperties")) {
                schema.removed_properties = root["removedProperties"].get<decltype(schema.removed_properties)>();
            }
            if (root.contains("renamedProperties")) {
                schema.renamed_properties = root["renamedProperties"].get<decltype(schema.renamed_properties)>();
            }

            std::unordered_map<std::string, std::vector<ValueRemap>> value_indices;
            if (root.contains("remappedPropertyValuesIndex")) {
                for (const auto& [index, values] : root["remappedPropertyValuesIndex"].items()) {
                    auto& destination = value_indices[index];
                    for (const auto& value : values) {
                        auto old_value = schema_property("", value.at("old"));
                        auto new_value = schema_property("", value.at("new"));
                        if (!old_value || !new_value) throw std::runtime_error("upgrade value remap 无效");
                        destination.push_back({ std::move(*old_value), std::move(*new_value) });
                    }
                }
            }
            if (root.contains("remappedPropertyValues")) {
                for (const auto& [block, properties] : root["remappedPropertyValues"].items()) {
                    for (const auto& [property, index] : properties.items()) {
                        const auto found = value_indices.find(index.get<std::string>());
                        if (found == value_indices.end()) throw std::runtime_error("upgrade value remap index 缺失");
                        schema.remapped_property_values[block][property] = found->second;
                    }
                }
            }
            if (root.contains("remappedStates")) {
                for (const auto& [block, remaps] : root["remappedStates"].items()) {
                    for (const auto& encoded : remaps) {
                        StateRemap remap;
                        remap.old_properties = schema_properties(encoded.value("oldState", nlohmann::json{}));
                        remap.new_name = encoded.value("newName", std::string{});
                        remap.new_properties = schema_properties(encoded.value("newState", nlohmann::json{}));
                        remap.copied_properties = encoded.value("copiedState", std::vector<std::string>{});
                        if (encoded.contains("newFlattenedName") && encoded["newFlattenedName"].is_object()) {
                            const auto& flattened = encoded["newFlattenedName"];
                            remap.flattened.prefix = flattened.value("prefix", std::string{});
                            remap.flattened.property = flattened.value("flattenedProperty", std::string{});
                            remap.flattened.suffix = flattened.value("suffix", std::string{});
                            remap.flattened.value_remaps = flattened.value(
                                "flattenedValueRemaps", std::unordered_map<std::string, std::string>{});
                        }
                        schema.remapped_states[block].push_back(std::move(remap));
                    }
                }
            }
            schemas->push_back(std::move(schema));
        }
        if (schemas->empty()) {
            return Result<std::function<BlockState(BlockState)>>::failure("block upgrade schema 目录为空");
        }

        auto upgrade = [schemas](BlockState state) {
            auto properties = property_map(state.states);
            for (const auto& schema : *schemas) {
                if (state.version > schema.id) continue;
                const auto old_name = state.name;
                const auto old_properties = properties;
                bool state_remapped = false;
                if (const auto remaps = schema.remapped_states.find(old_name);
                    remaps != schema.remapped_states.end()) {
                    for (const auto& remap : remaps->second) {
                        if (remap.old_properties.size() > old_properties.size()) continue;
                        bool matches = true;
                        for (const auto& [name, expected] : remap.old_properties) {
                            const auto actual = old_properties.find(name);
                            if (actual == old_properties.end() || !property_equal(actual->second, expected)) {
                                matches = false;
                                break;
                            }
                        }
                        if (!matches) continue;
                        auto new_properties = remap.new_properties;
                        for (const auto& name : remap.copied_properties) {
                            if (const auto found = old_properties.find(name); found != old_properties.end()) {
                                new_properties[name] = found->second;
                            }
                        }
                        auto new_name = remap.new_name;
                        if (new_name.empty()) {
                            const auto flattened = old_properties.find(remap.flattened.property);
                            if (flattened == old_properties.end() ||
                                flattened->second.type != BlockStateValueType::String) continue;
                            auto value = flattened->second.value;
                            if (const auto renamed = remap.flattened.value_remaps.find(value);
                                renamed != remap.flattened.value_remaps.end()) value = renamed->second;
                            new_name = remap.flattened.prefix + value + remap.flattened.suffix;
                        }
                        state.name = std::move(new_name);
                        properties = std::move(new_properties);
                        state.version = schema.id;
                        state_remapped = true;
                        break;
                    }
                }
                if (state_remapped) continue;

                bool modified = false;
                auto new_name = old_name;
                if (const auto renamed = schema.renamed_ids.find(old_name); renamed != schema.renamed_ids.end()) {
                    new_name = renamed->second;
                    modified = true;
                }
                if (const auto added = schema.added_properties.find(old_name);
                    added != schema.added_properties.end()) {
                    for (const auto& [name, value] : added->second) {
                        if (!properties.contains(name)) {
                            properties.emplace(name, value);
                            modified = true;
                        }
                    }
                }
                if (const auto removed = schema.removed_properties.find(old_name);
                    removed != schema.removed_properties.end()) {
                    for (const auto& name : removed->second) modified = properties.erase(name) != 0 || modified;
                }
                if (const auto renamed = schema.renamed_properties.find(old_name);
                    renamed != schema.renamed_properties.end()) {
                    for (const auto& [old_property, new_property] : renamed->second) {
                        const auto found = properties.find(old_property);
                        if (found == properties.end()) continue;
                        auto value = remap_property_value(schema, old_name, old_property, found->second);
                        properties.erase(found);
                        value.name = new_property;
                        properties[new_property] = std::move(value);
                        modified = true;
                    }
                }
                if (const auto block_remaps = schema.remapped_property_values.find(old_name);
                    block_remaps != schema.remapped_property_values.end()) {
                    for (const auto& [property, _] : block_remaps->second) {
                        const auto found = properties.find(property);
                        if (found == properties.end()) continue;
                        auto value = remap_property_value(schema, old_name, property, found->second);
                        if (!property_equal(found->second, value)) {
                            found->second = std::move(value);
                            modified = true;
                        }
                    }
                }
                if (modified) {
                    state.name = std::move(new_name);
                    state.version = schema.id;
                }
            }
            state.states = property_vector(std::move(properties));
            return state;
        };
        return Result<std::function<BlockState(BlockState)>>::success(std::move(upgrade));
    } catch (const std::exception& error) {
        return Result<std::function<BlockState(BlockState)>>::failure(
            "加载 block upgrade schemas 失败: " + std::string(error.what()));
    }
}

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string normalize_java_state(std::string_view encoded)
{
    const auto open = encoded.find_first_of("[{");
    auto name = trim(encoded.substr(0, open));
    if (name.find(':') == std::string::npos) {
        name.insert(0, "minecraft:");
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (open == std::string_view::npos) {
        return name;
    }
    const auto close = encoded.find_last_of("]}");
    const auto contents = encoded.substr(open + 1,
        (close == std::string_view::npos ? encoded.size() : close) - open - 1);
    std::vector<std::pair<std::string, std::string>> properties;
    std::size_t start = 0;
    while (start <= contents.size()) {
        const auto comma = contents.find(',', start);
        const auto part = contents.substr(start,
            comma == std::string_view::npos ? contents.size() - start : comma - start);
        if (const auto equals = part.find('='); equals != std::string_view::npos) {
            properties.emplace_back(trim(part.substr(0, equals)), trim(part.substr(equals + 1)));
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    std::sort(properties.begin(), properties.end());
    std::ostringstream result;
    result << name;
    if (!properties.empty()) {
        result << '[';
        for (std::size_t i = 0; i < properties.size(); ++i) {
            if (i != 0) result << ',';
            result << properties[i].first << '=' << properties[i].second;
        }
        result << ']';
    }
    return result.str();
}

std::string encode_java_state(const BlockState& state)
{
    auto name = state.name;
    if (name.find(':') == std::string::npos) name.insert(0, "minecraft:");
    if (state.states.empty()) return normalize_java_state(name);
    auto properties = state.states;
    std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    name.push_back('[');
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) name.push_back(',');
        name += properties[index].name;
        name.push_back('=');
        if (properties[index].type == BlockStateValueType::Byte) {
            name += properties[index].value == "0" ? "false" : "true";
        } else {
            name += properties[index].value;
        }
    }
    name.push_back(']');
    return normalize_java_state(name);
}

std::unordered_map<std::string, std::string> java_properties(std::string_view encoded)
{
    std::unordered_map<std::string, std::string> result;
    const auto begin = encoded.find('[');
    if (begin == std::string_view::npos) return result;
    const auto end = encoded.find_last_of(']');
    const auto contents = encoded.substr(begin + 1,
        (end == std::string_view::npos ? encoded.size() : end) - begin - 1);
    std::size_t offset = 0;
    while (offset <= contents.size()) {
        const auto comma = contents.find(',', offset);
        const auto part = contents.substr(offset,
            comma == std::string_view::npos ? contents.size() - offset : comma - offset);
        if (const auto equal = part.find('='); equal != std::string_view::npos) {
            result.emplace(std::string(part.substr(0, equal)), std::string(part.substr(equal + 1)));
        }
        if (comma == std::string_view::npos) break;
        offset = comma + 1;
    }
    return result;
}

std::vector<std::pair<std::string_view, std::string_view>> java_property_views(
    std::string_view encoded)
{
    std::vector<std::pair<std::string_view, std::string_view>> result;
    const auto begin = encoded.find('[');
    if (begin == std::string_view::npos) return result;
    const auto end = encoded.find_last_of(']');
    const auto contents = encoded.substr(begin + 1,
        (end == std::string_view::npos ? encoded.size() : end) - begin - 1);
    std::size_t offset = 0;
    while (offset <= contents.size()) {
        const auto comma = contents.find(',', offset);
        const auto part = contents.substr(offset,
            comma == std::string_view::npos ? contents.size() - offset : comma - offset);
        if (const auto equal = part.find('='); equal != std::string_view::npos) {
            const auto name = part.substr(0, equal);
            const auto duplicate = std::ranges::find_if(result, [name](const auto& property) {
                return property.first == name;
            });
            if (duplicate == result.end()) {
                result.emplace_back(name, part.substr(equal + 1));
            }
        }
        if (comma == std::string_view::npos) break;
        offset = comma + 1;
    }
    return result;
}

bool ascii_equal_ignore_case(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

std::optional<std::int64_t> fuzzy_java_integer(std::string_view value)
{
    if (ascii_equal_ignore_case(value, "true") || ascii_equal_ignore_case(value, "1b") || value == "1") {
        return 1;
    }
    if (ascii_equal_ignore_case(value, "false") || ascii_equal_ignore_case(value, "0b") || value == "0") {
        return 0;
    }
    if (value.empty()) return std::nullopt;
    const auto* first = value.data();
    const auto* last = first + value.size();
    if (*first == '+') {
        ++first;
        if (first == last) return std::nullopt;
    }
    std::int64_t result = 0;
    const auto parsed = std::from_chars(first, last, result);
    if (parsed.ec != std::errc{} || parsed.ptr != last) return std::nullopt;
    return result;
}

bool fuzzy_java_equal(std::string_view left, std::string_view right)
{
    const auto left_integer = fuzzy_java_integer(left);
    const auto right_integer = fuzzy_java_integer(right);
    if (left_integer || right_integer) {
        return left_integer && right_integer && *left_integer == *right_integer;
    }
    return ascii_equal_ignore_case(left, right);
}

std::string fuzzy_java_value(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value == "true" || value == "1b" || value == "1") return "1";
    if (value == "false" || value == "0b" || value == "0") return "0";
    try {
        std::size_t consumed = 0;
        const auto number = std::stoll(value, &consumed);
        if (consumed == value.size()) return std::to_string(number);
    } catch (const std::exception&) {
        // Non-numeric strings compare by their literal value.
    }
    return value;
}

std::string canonical_java_order_key(std::string_view encoded)
{
    const auto normalized = normalize_java_state(encoded);
    const auto open = normalized.find('[');
    const auto name = normalized.substr(0, open);
    const auto properties = java_properties(normalized);
    std::vector<std::string> entries;
    entries.reserve(properties.size());
    for (const auto& [property, value] : properties) {
        entries.push_back(property + "=" + fuzzy_java_value(value));
    }
    std::sort(entries.begin(), entries.end());
    std::string result = name;
    if (!entries.empty()) {
        result += '[';
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (index != 0) result += ',';
            result += entries[index];
        }
        result += ']';
    }
    return result;
}

std::string legacy_state_search_key(std::span<const BlockStateProperty> states)
{
    std::vector<BlockStateProperty> sorted(states.begin(), states.end());
    for (auto& property : sorted) {
        for (std::size_t position = property.name.find("minecraft:");
             position != std::string::npos;
             position = property.name.find("minecraft:")) {
            property.name.erase(position, 10);
        }
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::ostringstream key;
    key << '{';
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        if (index != 0) key << ',';
        const auto& property = sorted[index];
        key << property.name << ':';
        if (property.type == BlockStateValueType::Byte) {
            key << (property.value == "0" ? "0b" : "1b");
        } else if (property.type == BlockStateValueType::String) {
            if (property.value == "true" || property.value == "1b") key << "1b";
            else if (property.value == "false" || property.value == "0b") key << "0b";
            else key << property.value;
        } else {
            key << property.value;
        }
    }
    key << '}';
    return key.str();
}

} // namespace

RuntimeRegistry::RuntimeRegistry():
    mSchematicMapping(256 * 256)
{
    BlockState air;
    air.name = "minecraft:air";
    mAirRuntimeId = register_state(std::move(air));
}

std::string RuntimeRegistry::normalize_name(std::string_view name)
{
    std::string result(name);
    if (result.empty()) {
        return "minecraft:unknown";
    }
    if (result.find(':') == std::string::npos) {
        result.insert(0, "minecraft:");
    }
    return result;
}

std::string RuntimeRegistry::normalize_legacy_name(std::string_view name)
{
    auto result = normalize_name(name);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string RuntimeRegistry::key_for(const BlockState& state)
{
    return key_for(state.name, state.states, state.version);
}

std::string RuntimeRegistry::state_key_for(const BlockState& state)
{
    return state_key_for(state.name, state.states);
}

std::string RuntimeRegistry::state_key_for(
    std::string_view name,
    std::span<const BlockStateProperty> states
)
{
    std::vector<BlockStateProperty> sorted(states.begin(), states.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        if (left.name != right.name) return left.name < right.name;
        if (left.type != right.type) return left.type < right.type;
        return left.value < right.value;
    });

    std::ostringstream key;
    key << normalize_name(name);
    for (const auto& state : sorted) {
        key << '|' << state.name << ':' << static_cast<int>(state.type) << '=' << state.value;
    }
    return key.str();
}

std::string RuntimeRegistry::key_for(
    std::string_view name,
    std::span<const BlockStateProperty> states,
    std::int32_t version
)
{
    std::vector<BlockStateProperty> sorted(states.begin(), states.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }
        return left.value < right.value;
    });

    std::ostringstream key;
    key << normalize_name(name) << '|' << version;
    for (const auto& state : sorted) {
        key << '|' << state.name << ':' << static_cast<int>(state.type) << '=' << state.value;
    }
    return key.str();
}

std::uint32_t RuntimeRegistry::register_state(BlockState state)
{
    std::function<BlockState(BlockState)> upgrade_state;
    {
        std::lock_guard lock(mMutex);
        upgrade_state = mUpgradeState;
    }
    if (upgrade_state) {
        state = upgrade_state(std::move(state));
    }
    state.name = normalize_name(state.name);
    const auto key = key_for(state);
    const auto state_key = state_key_for(state);
    std::lock_guard lock(mMutex);
    if (const auto it = mByKey.find(key); it != mByKey.end()) {
        return it->second;
    }
    if (upgrade_state) {
        if (const auto it = mByStateKey.find(state_key); it != mByStateKey.end()) {
            return it->second;
        }
    }

    const auto runtime_id = mNextRuntimeId++;
    mByKey.emplace(key, runtime_id);
    mByStateKey.try_emplace(state_key, runtime_id);
    const auto normalized_name = state.name;
    const auto existing_name = mByName.find(normalized_name);
    if (existing_name == mByName.end() || state.states.empty()) {
        mByName[normalized_name] = runtime_id;
    }
    mByRuntimeId.emplace(runtime_id, std::move(state));
    return runtime_id;
}

std::optional<std::uint32_t> RuntimeRegistry::find(
    std::string_view name,
    std::span<const BlockStateProperty> states,
    std::int32_t version
) const
{
    const auto key = key_for(name, states, version);
    std::lock_guard lock(mMutex);
    const auto it = mByKey.find(key);
    if (it != mByKey.end()) return it->second;
    if (version == 0) {
        const auto by_state = mByStateKey.find(state_key_for(name, states));
        if (by_state != mByStateKey.end()) return by_state->second;
    }
    if (states.empty() && version == 0) {
        const auto by_name = mByName.find(normalize_name(name));
        if (by_name != mByName.end()) return by_name->second;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> RuntimeRegistry::find_compatible(BlockState state) const
{
    std::function<BlockState(BlockState)> upgrade_state;
    {
        std::lock_guard lock(mMutex);
        upgrade_state = mUpgradeState;
    }
    if (upgrade_state) state = upgrade_state(std::move(state));
    state.name = normalize_name(state.name);

    const auto key = key_for(state);
    const auto state_key = state_key_for(state);
    std::lock_guard lock(mMutex);
    if (const auto found = mByKey.find(key); found != mByKey.end()) return found->second;
    if (const auto found = mByStateKey.find(state_key); found != mByStateKey.end()) return found->second;
    if (state.states.empty()) {
        if (const auto found = mByName.find(state.name); found != mByName.end()) return found->second;
    }
    return std::nullopt;
}

std::optional<BlockState> RuntimeRegistry::state(std::uint32_t runtime_id) const
{
    std::lock_guard lock(mMutex);
    const auto it = mByRuntimeId.find(runtime_id);
    return it == mByRuntimeId.end() ? std::nullopt : std::optional<BlockState>(it->second);
}

std::optional<BlockState> RuntimeRegistry::java_state(std::uint32_t runtime_id) const
{
    std::lock_guard lock(mMutex);
    const auto it = mJavaByRuntimeId.find(runtime_id);
    return it == mJavaByRuntimeId.end() ? std::nullopt : std::optional<BlockState>(it->second);
}

std::unordered_map<std::uint32_t, BlockState> RuntimeRegistry::java_states_snapshot() const
{
    std::lock_guard lock(mMutex);
    return mJavaByRuntimeId;
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> RuntimeRegistry::schematic_block(
    std::uint32_t runtime_id) const
{
    std::lock_guard lock(mMutex);
    const auto it = mSchematicReverse.find(runtime_id);
    return it == mSchematicReverse.end() ? std::nullopt :
        std::optional<std::pair<std::uint8_t, std::uint8_t>>(it->second);
}

Result<void> RuntimeRegistry::load_legacy_pool(const std::filesystem::path& path, std::uint8_t pool_id)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::failure("无法打开 legacy runtime pool: " + path.string());
    }

    nlohmann::json root;
    try {
        input >> root;
    } catch (const std::exception& error) {
        return Result<void>::failure("解析 legacy runtime pool 失败: " + std::string(error.what()));
    }
    if (!root.is_array()) {
        return Result<void>::failure("legacy runtime pool 根节点不是数组");
    }

    std::vector<std::uint32_t> pool;
    pool.reserve(root.size());
    for (const auto& item : root) {
        if (!item.is_array() || item.size() < 2 || !item[0].is_string() || !item[1].is_number_integer()) {
            pool.push_back(mAirRuntimeId);
            continue;
        }
        BlockState state;
        state.name = item[0].get<std::string>();
        const auto runtime_id = register_state(std::move(state));
        pool.push_back(runtime_id);
    }

    std::lock_guard lock(mMutex);
    mLegacyPools[pool_id] = std::move(pool);
    return Result<void>::success();
}

Result<void> RuntimeRegistry::load_block_mappings(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::failure("无法打开方块映射资产: " + path.string());
    }

    nlohmann::json root;
    try {
        input >> root;
        if (!root.is_object() || root.value("schema", 0) != 1 || !root.contains("palette") ||
            !root["palette"].is_array()) {
            return Result<void>::failure("方块映射资产 schema 无效");
        }

        auto upgrade_state = load_upgrade_schemas(path.parent_path() / "block_upgrade_schemas");
        if (!upgrade_state) {
            return Result<void>::failure(upgrade_state.error());
        }

        std::vector<std::uint32_t> palette;
        std::vector<std::pair<std::uint32_t, BlockState>> loaded_states;
        palette.reserve(root["palette"].size());
        loaded_states.reserve(root["palette"].size());
        std::uint32_t source_air_id = 0;
        bool found_source_air = false;
        std::uint32_t maximum_source_id = 0;
        for (std::size_t palette_index = 0; palette_index < root["palette"].size(); ++palette_index) {
            const auto& item = root["palette"][palette_index];
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
                return Result<void>::failure("方块映射 palette 条目无效");
            }
            BlockState state;
            state.name = item["name"].get<std::string>();
            state.version = item.value("version", 0);
            if (item.contains("states") && item["states"].is_object()) {
                for (const auto& [name, encoded] : item["states"].items()) {
                    if (!encoded.is_object() || !encoded.contains("type") || !encoded.contains("value")) {
                        return Result<void>::failure("方块映射 state 条目无效");
                    }
                    BlockStateProperty property;
                    property.name = name;
                    property.value = encoded["value"].get<std::string>();
                    const auto type = encoded["type"].get<std::string>();
                    if (type == "byte") property.type = BlockStateValueType::Byte;
                    else if (type == "short") property.type = BlockStateValueType::Short;
                    else if (type == "int") property.type = BlockStateValueType::Int;
                    else if (type == "long") property.type = BlockStateValueType::Long;
                    else if (type == "string") property.type = BlockStateValueType::String;
                    else return Result<void>::failure("方块映射 state 类型无效: " + type);
                    state.states.push_back(std::move(property));
                }
            }
            const auto source_runtime_id = item.value(
                "source_runtime_id", static_cast<std::uint32_t>(palette_index));
            maximum_source_id = std::max(maximum_source_id, source_runtime_id);
            if ((state.name == "air" || state.name == "minecraft:air") && state.states.empty()) {
                source_air_id = source_runtime_id;
                found_source_air = true;
            }
            palette.push_back(source_runtime_id);
            loaded_states.emplace_back(source_runtime_id, std::move(state));
        }
        if (!found_source_air) {
            return Result<void>::failure("方块映射 palette 缺少 minecraft:air");
        }

        {
            std::lock_guard lock(mMutex);
            mByKey.clear();
            mByStateKey.clear();
            mByName.clear();
            mByRuntimeId.clear();
            for (auto& [runtime_id, state] : loaded_states) {
                state.name = normalize_name(state.name);
                const auto key = key_for(state);
                if (mByRuntimeId.contains(runtime_id)) {
                    return Result<void>::failure("方块映射 source_runtime_id 重复: " + std::to_string(runtime_id));
                }
                mByKey.emplace(key, runtime_id);
                mByStateKey.try_emplace(state_key_for(state), runtime_id);
                if (!mByName.contains(state.name) || state.states.empty()) {
                    mByName[state.name] = runtime_id;
                }
                mByRuntimeId.emplace(runtime_id, std::move(state));
            }
            mAirRuntimeId = source_air_id;
            mNextRuntimeId = maximum_source_id + 1;
        }

        std::vector<std::uint32_t> schematic(256 * 256);
        bool has_schematic = false;
        if (root.contains("schematic_indices") && root["schematic_indices"].is_array()) {
            if (root["schematic_indices"].size() != schematic.size()) {
                return Result<void>::failure("schematic 映射长度不是 65536");
            }
            for (std::size_t i = 0; i < schematic.size(); ++i) {
                schematic[i] = root["schematic_indices"][i].get<std::uint32_t>();
            }
            has_schematic = true;
        }

        std::unordered_map<std::uint32_t, std::pair<std::uint8_t, std::uint8_t>> schematic_reverse;
        const auto schematic_reverse_path = path.parent_path() / "schematic_reverse.json";
        if (std::filesystem::exists(schematic_reverse_path)) {
            std::ifstream reverse_input(schematic_reverse_path);
            if (!reverse_input) {
                return Result<void>::failure("无法打开 Schematic 反向映射资产: " +
                    schematic_reverse_path.string());
            }
            nlohmann::json reverse_root;
            reverse_input >> reverse_root;
            if (!reverse_root.is_object()) {
                return Result<void>::failure("Schematic 反向映射资产不是对象");
            }
            schematic_reverse.reserve(reverse_root.size());
            for (const auto& [runtime_text, encoded] : reverse_root.items()) {
                std::size_t consumed = 0;
                const auto runtime = std::stoul(runtime_text, &consumed);
                if (consumed != runtime_text.size() || runtime > UINT32_MAX ||
                    !encoded.is_object() || !encoded.contains("block") ||
                    !encoded.contains("data") || !encoded["block"].is_number_unsigned() ||
                    !encoded["data"].is_number_unsigned() ||
                    encoded["block"].get<std::uint32_t>() > UINT8_MAX ||
                    encoded["data"].get<std::uint32_t>() > UINT8_MAX) {
                    return Result<void>::failure("Schematic 反向映射条目无效: " + runtime_text);
                }
                schematic_reverse.emplace(static_cast<std::uint32_t>(runtime), std::make_pair(
                    static_cast<std::uint8_t>(encoded["block"].get<std::uint32_t>()),
                    static_cast<std::uint8_t>(encoded["data"].get<std::uint32_t>())));
            }
        }

        std::vector<std::uint32_t> bdx117;
        if (root.contains("bdx_117_indices") && root["bdx_117_indices"].is_array()) {
            bdx117.reserve(root["bdx_117_indices"].size());
            for (const auto& item : root["bdx_117_indices"]) {
                bdx117.push_back(item.get<std::uint32_t>());
            }
        }

        std::unordered_map<std::string, std::uint32_t> java_mapping;
        if (root.contains("java_to_runtime") && root["java_to_runtime"].is_object()) {
            java_mapping.reserve(root["java_to_runtime"].size());
            for (const auto& [java_state, source_value] : root["java_to_runtime"].items()) {
                java_mapping[normalize_java_state(java_state)] = source_value.get<std::uint32_t>();
            }
        }
		std::unordered_map<std::string, std::uint32_t> java_defaults;
		if (root.contains("java_default_runtime") && root["java_default_runtime"].is_object()) {
			java_defaults.reserve(root["java_default_runtime"].size());
			for (const auto& [name, runtime_id] : root["java_default_runtime"].items()) {
				java_defaults.emplace(normalize_legacy_name(name), runtime_id.get<std::uint32_t>());
			}
		}
        std::unordered_map<std::string, std::size_t> java_order;
        const auto order_path = path.parent_path() / "java_conversion_order.json";
        if (std::filesystem::exists(order_path)) {
            std::ifstream order_input(order_path);
            if (!order_input) {
                return Result<void>::failure("无法打开 Java 方块转换顺序资产: " + order_path.string());
            }
            nlohmann::json order_root;
            order_input >> order_root;
            if (!order_root.is_object()) {
                return Result<void>::failure("Java 方块转换顺序资产不是对象");
            }
            java_order.reserve(order_root.size());
            for (const auto& [java_state, entry] : order_root.items()) {
                const auto normalized = normalize_java_state(java_state);
                if (entry.is_number_unsigned()) {
                    java_order.emplace(normalized, entry.get<std::size_t>());
                    continue;
                }
                if (!entry.is_object() || !entry.contains("runtime") || !entry.contains("order") ||
                    !entry["runtime"].is_number_unsigned() || !entry["order"].is_number_unsigned()) {
                    return Result<void>::failure("Java 方块转换顺序值无效: " + java_state);
                }
                java_mapping.try_emplace(normalized, entry["runtime"].get<std::uint32_t>());
                java_order.emplace(normalized, entry["order"].get<std::size_t>());
            }
        }

        std::unordered_map<std::uint32_t, BlockState> java_by_runtime;
        const auto java_runtime_path = path.parent_path() / "java_runtime_states.json";
        if (std::filesystem::exists(java_runtime_path)) {
            std::ifstream java_runtime_input(java_runtime_path);
            if (!java_runtime_input) {
                return Result<void>::failure("无法打开 Java runtime 反向映射资产: " +
                    java_runtime_path.string());
            }
            nlohmann::json java_runtime_root;
            java_runtime_input >> java_runtime_root;
            if (!java_runtime_root.is_array()) {
                return Result<void>::failure("Java runtime 反向映射资产不是数组");
            }
            java_by_runtime.reserve(java_runtime_root.size());
            for (const auto& item : java_runtime_root) {
                if (!item.is_object() || !item.contains("runtime") ||
                    !item["runtime"].is_number_unsigned() || !item.contains("name") ||
                    !item["name"].is_string()) {
                    return Result<void>::failure("Java runtime 反向映射条目无效");
                }
                BlockState state;
                state.name = normalize_name(item["name"].get<std::string>());
                if (item.contains("states")) {
                    if (!item["states"].is_object()) {
                        return Result<void>::failure("Java runtime states 不是对象");
                    }
                    for (const auto& [name, encoded] : item["states"].items()) {
                        if (!encoded.is_object() || !encoded.contains("type") ||
                            !encoded["type"].is_string() || !encoded.contains("value") ||
                            !encoded["value"].is_string()) {
                            return Result<void>::failure("Java runtime state 条目无效");
                        }
                        BlockStateProperty property;
                        property.name = name;
                        property.value = encoded["value"].get<std::string>();
                        const auto type = encoded["type"].get<std::string>();
                        if (type == "byte") property.type = BlockStateValueType::Byte;
                        else if (type == "short") property.type = BlockStateValueType::Short;
                        else if (type == "int") property.type = BlockStateValueType::Int;
                        else if (type == "long") property.type = BlockStateValueType::Long;
                        else if (type == "string") property.type = BlockStateValueType::String;
                        else return Result<void>::failure("Java runtime state 类型无效: " + type);
                        state.states.push_back(std::move(property));
                    }
                }
                java_by_runtime.emplace(item["runtime"].get<std::uint32_t>(), std::move(state));
            }
        }
        std::unordered_set<std::string> boolean_java_properties;
        for (const auto& [encoded, _] : java_mapping) {
            const auto open = encoded.find('[');
            const auto name = encoded.substr(0, open);
            for (const auto& [property, value] : java_property_views(encoded)) {
                if (ascii_equal_ignore_case(value, "true") ||
                    ascii_equal_ignore_case(value, "false")) {
                    boolean_java_properties.insert(name + '\n' + std::string(property));
                }
            }
        }
        for (auto& [_, state] : java_by_runtime) {
            const auto name = normalize_java_state(state.name);
            for (auto& property : state.states) {
                if (property.type == BlockStateValueType::Byte &&
                    !boolean_java_properties.contains(name + '\n' + property.name)) {
                    property.type = BlockStateValueType::Int;
                }
            }
        }
        std::unordered_map<std::string, std::uint32_t> java_round_trip_mapping;
        std::unordered_set<std::string> ambiguous_java_states;
        java_round_trip_mapping.reserve(java_by_runtime.size());
        for (const auto& [runtime_id, state] : java_by_runtime) {
            auto encoded = encode_java_state(state);
            if (ambiguous_java_states.contains(encoded)) continue;
            const auto [found, inserted] = java_round_trip_mapping.emplace(encoded, runtime_id);
            if (!inserted && found->second != runtime_id) {
                java_round_trip_mapping.erase(found);
                ambiguous_java_states.insert(std::move(encoded));
            }
        }

        std::unordered_map<std::string, std::unordered_map<std::uint16_t, std::uint32_t>> legacy_by_name;
        if (root.contains("legacy_to_runtime") && root["legacy_to_runtime"].is_object()) {
            for (const auto& [name, values] : root["legacy_to_runtime"].items()) {
                if (!values.is_object()) continue;
                auto& destination = legacy_by_name[normalize_legacy_name(name)];
                for (const auto& [data, runtime_id] : values.items()) {
                    std::size_t consumed = 0;
                    const auto parsed = std::stoul(data, &consumed);
                    if (consumed != data.size() || parsed > std::numeric_limits<std::uint16_t>::max()) {
                        return Result<void>::failure("legacy_to_runtime data 无效: " + data);
                    }
                    destination.emplace(static_cast<std::uint16_t>(parsed), runtime_id.get<std::uint32_t>());
                }
            }
        }
        std::unordered_map<std::string, std::uint32_t> legacy_defaults;
        if (root.contains("legacy_default_runtime") && root["legacy_default_runtime"].is_object()) {
            legacy_defaults.reserve(root["legacy_default_runtime"].size());
            for (const auto& [name, runtime_id] : root["legacy_default_runtime"].items()) {
                legacy_defaults.emplace(normalize_legacy_name(name), runtime_id.get<std::uint32_t>());
            }
        }

        std::unordered_map<std::string, std::unordered_map<std::string, std::uint32_t>> legacy_by_state;
        if (root.contains("legacy_state_to_runtime") && root["legacy_state_to_runtime"].is_object()) {
            for (const auto& [name, values] : root["legacy_state_to_runtime"].items()) {
                if (!values.is_object()) continue;
                auto& destination = legacy_by_state[normalize_legacy_name(name)];
                for (const auto& [state_key, runtime_id] : values.items()) {
                    destination.emplace(state_key, runtime_id.get<std::uint32_t>());
                }
            }
        }
        std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> legacy_state_order;
        if (root.contains("legacy_state_order") && root["legacy_state_order"].is_object()) {
            for (const auto& [name, values] : root["legacy_state_order"].items()) {
                if (!values.is_object()) continue;
                auto& destination = legacy_state_order[normalize_legacy_name(name)];
                for (const auto& [state_key, order] : values.items()) {
                    destination.emplace(state_key, order.get<std::size_t>());
                }
            }
        }

        std::lock_guard lock(mMutex);
        mUpgradeState = std::move(upgrade_state.value());
        mSchematicMapping = std::move(schematic);
        mHasSchematicMapping = has_schematic;
        mSchematicReverse = std::move(schematic_reverse);
        if (!bdx117.empty()) {
            mLegacyPools[117] = std::move(bdx117);
        }
        mJavaMapping = std::move(java_mapping);
        mJavaRoundTripMapping = std::move(java_round_trip_mapping);
        mJavaDefaultByName = std::move(java_defaults);
        mJavaOrder = std::move(java_order);
        mJavaCandidatesByName.clear();
        mJavaCandidatesByName.reserve(mJavaDefaultByName.size());
        for (const auto& [encoded, runtime_id] : mJavaMapping) {
            const auto open = encoded.find('[');
            JavaCandidate candidate;
            candidate.encoded = encoded;
            candidate.runtime_id = runtime_id;
            const auto properties = java_property_views(encoded);
            candidate.properties.reserve(properties.size());
            for (const auto& [name, value] : properties) {
                candidate.properties.push_back({ name, value });
            }
            mJavaCandidatesByName[encoded.substr(0, open)].push_back(std::move(candidate));
        }
        mJavaByRuntimeId = std::move(java_by_runtime);
        mLegacyByName = std::move(legacy_by_name);
        mLegacyDefaultByName = std::move(legacy_defaults);
        mLegacyByState = std::move(legacy_by_state);
        mLegacyStateOrder = std::move(legacy_state_order);
        if (const auto air = mJavaMapping.find("minecraft:air"); air != mJavaMapping.end()) {
            mAirRuntimeId = air->second;
        }
        for (const auto& [encoded, runtime_id] : mJavaMapping) {
            const auto open = encoded.find('[');
            const auto base_name = encoded.substr(0, open);
            BlockState state;
            if (const auto by_name = mByName.find(base_name); by_name != mByName.end()) {
                if (const auto by_runtime = mByRuntimeId.find(by_name->second); by_runtime != mByRuntimeId.end()) {
                    state = by_runtime->second;
                }
            }
            if (state.name.empty()) state.name = base_name;
            if (!mByRuntimeId.contains(runtime_id)) {
                mByRuntimeId.emplace(runtime_id, state);
            }
            if (open == std::string::npos) mByName[base_name] = runtime_id;
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure("解析方块映射资产失败: " + std::string(error.what()));
    }
}

std::optional<std::uint32_t> RuntimeRegistry::legacy_runtime_id(std::uint8_t pool_id, std::size_t index) const
{
    std::lock_guard lock(mMutex);
    const auto pool = mLegacyPools.find(pool_id);
    if (pool == mLegacyPools.end() || index >= pool->second.size()) {
        return std::nullopt;
    }
    return pool->second[index];
}

std::vector<std::uint32_t> RuntimeRegistry::legacy_pool_snapshot(std::uint8_t pool_id) const
{
    std::lock_guard lock(mMutex);
    const auto found = mLegacyPools.find(pool_id);
    return found == mLegacyPools.end() ? std::vector<std::uint32_t>{} : found->second;
}

std::optional<std::uint32_t> RuntimeRegistry::legacy_runtime_id(
    std::string_view name,
    std::uint16_t data) const
{
    std::lock_guard lock(mMutex);
    const auto normalized = normalize_legacy_name(name);
    const auto block = mLegacyByName.find(normalized);
    if (block != mLegacyByName.end()) {
        const auto value = block->second.find(data);
        if (value != block->second.end()) return value->second;
    }
    const auto fallback = mLegacyDefaultByName.find(normalized);
    return fallback == mLegacyDefaultByName.end()
        ? std::nullopt
        : std::optional<std::uint32_t>(fallback->second);
}

std::optional<std::uint32_t> RuntimeRegistry::legacy_state_runtime_id(
    std::string_view name,
    std::span<const BlockStateProperty> states) const
{
    const auto normalized = normalize_legacy_name(name);
    const auto state_key = legacy_state_search_key(states);
    std::lock_guard lock(mMutex);
    const auto block = mLegacyByState.find(normalized);
    if (block == mLegacyByState.end()) return std::nullopt;
    const auto found = block->second.find(state_key);
    return found == block->second.end()
        ? std::nullopt
        : std::optional<std::uint32_t>(found->second);
}

std::optional<std::uint32_t> RuntimeRegistry::schematic_runtime_id(std::uint8_t block_id, std::uint8_t data) const
{
    std::lock_guard lock(mMutex);
    if (!mHasSchematicMapping) {
        return std::nullopt;
    }
    return mSchematicMapping[static_cast<std::size_t>(block_id) * 256 + data];
}

std::optional<std::uint32_t> RuntimeRegistry::java_runtime_id(std::string_view block_state) const
{
    const auto key = normalize_java_state(block_state);
    std::lock_guard lock(mMutex);
    const auto round_trip = mJavaRoundTripMapping.find(key);
    if (round_trip != mJavaRoundTripMapping.end()) return round_trip->second;
    const auto it = mJavaMapping.find(key);
    return it == mJavaMapping.end() ? std::nullopt : std::optional<std::uint32_t>(it->second);
}

std::optional<std::uint32_t> RuntimeRegistry::compatible_java_runtime_id(
    std::string_view block_state) const
{
    const auto requested = normalize_java_state(block_state);
    const auto open = requested.find('[');
    const auto requested_name = requested.substr(0, open);
    const auto requested_property_views = java_property_views(requested);
    if (requested_property_views.empty()) {
        std::lock_guard lock(mMutex);
        const auto round_trip = mJavaRoundTripMapping.find(requested);
        if (round_trip != mJavaRoundTripMapping.end()) return round_trip->second;
        const auto java_default = mJavaDefaultByName.find(requested_name);
        if (java_default != mJavaDefaultByName.end()) return java_default->second;
    }
    std::lock_guard lock(mMutex);
    const auto round_trip = mJavaRoundTripMapping.find(requested);
    if (round_trip != mJavaRoundTripMapping.end()) return round_trip->second;
    const auto exact = mJavaMapping.find(requested);
    if (exact != mJavaMapping.end()) return exact->second;
    const JavaCandidate* best_candidate = nullptr;
    std::size_t java_best_same = 0;
    std::size_t java_best_penalty = std::numeric_limits<std::size_t>::max();
    const auto candidate_order = [this](const JavaCandidate& candidate) {
        if (!candidate.order) {
            const auto order = mJavaOrder.find(canonical_java_order_key(candidate.encoded));
            candidate.order = order == mJavaOrder.end()
                ? std::numeric_limits<std::size_t>::max() : order->second;
        }
        return *candidate.order;
    };
    const auto indexed_candidates = mJavaCandidatesByName.find(requested_name);
    if (indexed_candidates != mJavaCandidatesByName.end()) {
        for (const auto& candidate : indexed_candidates->second) {
            std::size_t same = 0;
            std::size_t penalty = 0;
            for (const auto& [name, value] : requested_property_views) {
                const auto found = std::ranges::find_if(candidate.properties, [name](const auto& property) {
                    return property.name == name;
                });
                if (found == candidate.properties.end()) {
                    ++penalty;
                } else if (fuzzy_java_equal(found->value, value)) {
                    ++same;
                } else {
                    ++penalty;
                }
            }
            for (const auto& property : candidate.properties) {
                const auto found = std::ranges::find_if(
                    requested_property_views,
                    [&property](const auto& requested) {
                        return requested.first == property.name;
                    });
                if (found == requested_property_views.end()) ++penalty;
            }

            // Resolve canonical conversion order only for a primary-score tie.
            // This preserves same/penalty/order/runtime ordering without
            // canonicalizing every candidate during registry startup.
            auto better = !best_candidate || same > java_best_same ||
                (same == java_best_same && penalty < java_best_penalty);
            if (!better && best_candidate &&
                same == java_best_same && penalty == java_best_penalty) {
                const auto order = candidate_order(candidate);
                const auto best_order = candidate_order(*best_candidate);
                better = order < best_order ||
                    (order == best_order && candidate.runtime_id < best_candidate->runtime_id);
            }
            if (better) {
                java_best_same = same;
                java_best_penalty = penalty;
                best_candidate = &candidate;
            }
        }
    }

    if (best_candidate) return best_candidate->runtime_id;
    const auto requested_properties = java_properties(requested);
    const auto legacy = mLegacyByState.find(requested_name);
    if (legacy == mLegacyByState.end()) return std::nullopt;
    const auto legacy_order = mLegacyStateOrder.find(requested_name);
    std::optional<std::uint32_t> best_runtime;
    std::size_t best_same = 0;
    std::size_t best_penalty = std::numeric_limits<std::size_t>::max();
    std::size_t best_order = std::numeric_limits<std::size_t>::max();
    auto parse_legacy_properties = [](std::string_view encoded) {
        std::unordered_map<std::string, std::string> result;
        if (encoded.size() >= 2 && encoded.front() == '{' && encoded.back() == '}') {
            encoded.remove_prefix(1);
            encoded.remove_suffix(1);
        }
        std::size_t offset = 0;
        while (offset <= encoded.size()) {
            const auto comma = encoded.find(',', offset);
            const auto part = encoded.substr(offset,
                comma == std::string_view::npos ? encoded.size() - offset : comma - offset);
            if (const auto colon = part.find(':'); colon != std::string_view::npos) {
                result.emplace(std::string(part.substr(0, colon)), std::string(part.substr(colon + 1)));
            }
            if (comma == std::string_view::npos) break;
            offset = comma + 1;
        }
        return result;
    };
    for (const auto& [candidate, runtime] : legacy->second) {
        const auto candidate_properties = parse_legacy_properties(candidate);
        std::size_t same = 0;
        std::size_t penalty = 0;
        for (const auto& [name, value] : requested_properties) {
            const auto found = candidate_properties.find(name);
            if (found == candidate_properties.end()) ++penalty;
            else if (fuzzy_java_value(found->second) == fuzzy_java_value(value)) ++same;
            else ++penalty;
        }
        for (const auto& [name, _] : candidate_properties) {
            if (!requested_properties.contains(name)) ++penalty;
        }
        auto candidate_order = std::numeric_limits<std::size_t>::max();
        if (legacy_order != mLegacyStateOrder.end()) {
            if (const auto found = legacy_order->second.find(candidate);
                found != legacy_order->second.end()) candidate_order = found->second;
        }
        if (!best_runtime || same > best_same ||
            (same == best_same && (penalty < best_penalty ||
                (penalty == best_penalty && (candidate_order < best_order ||
                    (candidate_order == best_order && runtime < *best_runtime)))))) {
            best_same = same;
            best_penalty = penalty;
            best_order = candidate_order;
            best_runtime = runtime;
        }
    }
    return best_runtime;
}

void RuntimeRegistry::install_as_bwo_resolver()
{
    BedrockWorldOperator::setBlockRuntimeResolver({
        [this](std::uint32_t runtime_id) -> std::optional<std::string> {
            const auto value = state(runtime_id);
            return value ? std::optional<std::string>(value->name) : std::nullopt;
        },
        [this](std::uint32_t runtime_id) -> std::optional<BedrockWorldOperator::BlockState> {
            const auto value = state(runtime_id);
            if (!value) {
                return std::nullopt;
            }
            BedrockWorldOperator::BlockState result;
            result.name = value->name;
            result.version = value->version;
            result.states.reserve(value->states.size());
            for (const auto& property : value->states) {
                BedrockWorldOperator::BlockStateProperty item;
                item.name = property.name;
                switch (property.type) {
                case BlockStateValueType::Byte:
                    item.type = BedrockWorldOperator::BlockStateValueType::Byte;
                    break;
                case BlockStateValueType::Short:
                    item.type = BedrockWorldOperator::BlockStateValueType::Short;
                    break;
                case BlockStateValueType::Int:
                    item.type = BedrockWorldOperator::BlockStateValueType::Int;
                    break;
                case BlockStateValueType::Long:
                    item.type = BedrockWorldOperator::BlockStateValueType::Long;
                    break;
                case BlockStateValueType::String:
                    item.type = BedrockWorldOperator::BlockStateValueType::String;
                    break;
                }
                if (property.type == BlockStateValueType::String) {
                    item.stringValue = property.value;
                } else {
                    item.intValue = std::stoll(property.value);
                }
                result.states.push_back(std::move(item));
            }
            return result;
        },
        [this](std::string_view name) -> std::optional<std::uint32_t> {
            return find(name);
        },
        [this]() -> std::uint32_t { return air_runtime_id(); },
        [this](const BedrockWorldOperator::BlockState& source) -> std::optional<std::uint32_t> {
            BlockState state;
            state.name = source.name;
            state.version = source.version;
            state.states.reserve(source.states.size());
            for (const auto& source_property : source.states) {
                BlockStateProperty property;
                property.name = source_property.name;
                switch (source_property.type) {
                case BedrockWorldOperator::BlockStateValueType::Byte:
                    property.type = BlockStateValueType::Byte;
                    property.value = std::to_string(static_cast<std::int8_t>(source_property.intValue));
                    break;
                case BedrockWorldOperator::BlockStateValueType::Short:
                    property.type = BlockStateValueType::Short;
                    property.value = std::to_string(static_cast<std::int16_t>(source_property.intValue));
                    break;
                case BedrockWorldOperator::BlockStateValueType::Int:
                    property.type = BlockStateValueType::Int;
                    property.value = std::to_string(static_cast<std::int32_t>(source_property.intValue));
                    break;
                case BedrockWorldOperator::BlockStateValueType::Long:
                    property.type = BlockStateValueType::Long;
                    property.value = std::to_string(source_property.intValue);
                    break;
                case BedrockWorldOperator::BlockStateValueType::String:
                    property.type = BlockStateValueType::String;
                    property.value = source_property.stringValue;
                    break;
                }
                state.states.push_back(std::move(property));
            }
            return register_state(std::move(state));
        }
    });
}

} // namespace water_structure
