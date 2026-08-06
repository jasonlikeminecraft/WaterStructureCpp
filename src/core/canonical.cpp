#include <WaterStructure/canonical.hpp>

#include <io/stream_reader.h>
#include <nbt_tags.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace water_structure {
namespace {

template <typename T>
void append_integer(std::vector<std::uint8_t>& output, T value)
{
    using Unsigned = std::make_unsigned_t<T>;
    const auto encoded = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output.push_back(static_cast<std::uint8_t>(encoded >> (i * 8u)));
    }
}

void append_string(std::vector<std::uint8_t>& output, std::string_view value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("canonical 字符串过长");
    }
    append_integer(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

std::int64_t parse_integer(std::string_view value)
{
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("方块状态整数无效: " + std::string(value));
    }
    return result;
}

void append_block_property(std::vector<std::uint8_t>& output, const BlockStateProperty& property)
{
    append_string(output, property.name);
    switch (property.type) {
    case BlockStateValueType::Byte:
        output.push_back(1);
        append_integer(output, static_cast<std::int8_t>(parse_integer(property.value)));
        break;
    case BlockStateValueType::Short:
        output.push_back(2);
        append_integer(output, static_cast<std::int16_t>(parse_integer(property.value)));
        break;
    case BlockStateValueType::Int:
        output.push_back(3);
        append_integer(output, static_cast<std::int32_t>(parse_integer(property.value)));
        break;
    case BlockStateValueType::Long:
        output.push_back(4);
        append_integer(output, parse_integer(property.value));
        break;
    case BlockStateValueType::String:
        output.push_back(8);
        append_string(output, property.value);
        break;
    }
}

void append_tag(std::vector<std::uint8_t>& output, const nbt::tag& value)
{
    const auto type = value.get_type();
    output.push_back(static_cast<std::uint8_t>(type));
    switch (type) {
    case nbt::tag_type::Byte:
        append_integer(output, value.as<nbt::tag_byte>().get());
        break;
    case nbt::tag_type::Short:
        append_integer(output, value.as<nbt::tag_short>().get());
        break;
    case nbt::tag_type::Int:
        append_integer(output, value.as<nbt::tag_int>().get());
        break;
    case nbt::tag_type::Long:
        append_integer(output, value.as<nbt::tag_long>().get());
        break;
    case nbt::tag_type::Float:
        append_integer(output, std::bit_cast<std::uint32_t>(value.as<nbt::tag_float>().get()));
        break;
    case nbt::tag_type::Double:
        append_integer(output, std::bit_cast<std::uint64_t>(value.as<nbt::tag_double>().get()));
        break;
    case nbt::tag_type::Byte_Array: {
        const auto& array = value.as<nbt::tag_byte_array>();
        append_integer(output, static_cast<std::uint32_t>(array.size()));
        for (const auto item : array) append_integer(output, item);
        break;
    }
    case nbt::tag_type::String:
        append_string(output, value.as<nbt::tag_string>().get());
        break;
    case nbt::tag_type::List: {
        const auto& list = value.as<nbt::tag_list>();
        append_integer(output, static_cast<std::uint32_t>(list.size()));
        for (const auto& item : list) append_tag(output, item.get());
        break;
    }
    case nbt::tag_type::Compound: {
        const auto& compound = value.as<nbt::tag_compound>();
        append_integer(output, static_cast<std::uint32_t>(compound.size()));
        for (const auto& [key, item] : compound) {
            append_string(output, key);
            append_tag(output, item.get());
        }
        break;
    }
    case nbt::tag_type::Int_Array: {
        const auto& array = value.as<nbt::tag_int_array>();
        append_integer(output, static_cast<std::uint32_t>(array.size()));
        for (const auto item : array) append_integer(output, item);
        break;
    }
    case nbt::tag_type::Long_Array: {
        const auto& array = value.as<nbt::tag_long_array>();
        append_integer(output, static_cast<std::uint32_t>(array.size()));
        for (const auto item : array) append_integer(output, item);
        break;
    }
    case nbt::tag_type::End:
    case nbt::tag_type::Null:
        throw std::runtime_error("canonical NBT 包含无效 tag 类型");
    }
}

std::string escape_json_pointer(std::string_view value)
{
    std::string output;
    output.reserve(value.size());
    for (const auto character : value) {
        if (character == '~') output += "~0";
        else if (character == '/') output += "~1";
        else output.push_back(character);
    }
    return output;
}

void collect_nbt_fields(
    std::vector<CanonicalNbtField>& fields,
    std::string path,
    const nbt::tag& value)
{
    std::vector<std::uint8_t> encoded;
    append_tag(encoded, value);
    fields.push_back({ path, std::move(encoded) });

    if (value.get_type() == nbt::tag_type::Compound) {
        const auto& compound = value.as<nbt::tag_compound>();
        std::vector<std::pair<std::string_view, const nbt::tag*>> children;
        children.reserve(compound.size());
        for (const auto& [key, child] : compound) children.emplace_back(key, &child.get());
        std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        for (const auto& [key, child] : children) {
            collect_nbt_fields(fields, path + "/" + escape_json_pointer(key), *child);
        }
    } else if (value.get_type() == nbt::tag_type::List) {
        const auto& list = value.as<nbt::tag_list>();
        for (std::size_t index = 0; index < list.size(); ++index) {
            collect_nbt_fields(fields, path + "/" + std::to_string(index), list[index].get());
        }
    }
}

std::unique_ptr<nbt::tag_compound> read_compound(std::span<const std::uint8_t> payload)
{
    const std::string bytes(reinterpret_cast<const char*>(payload.data()), payload.size());
    std::istringstream input(bytes, std::ios::binary);
    auto [_, compound] = nbt::io::read_compound(input, endian::little);
    return std::move(compound);
}

} // namespace

Result<std::vector<std::uint8_t>> canonical_block_state(const BlockState& state)
{
    try {
        std::vector<std::uint8_t> output{ 'W', 'S', 'B', 'S', 1 };
        append_string(output, state.name);
        append_integer(output, state.version);
        auto properties = state.states;
        std::sort(properties.begin(), properties.end(), [](const auto& left, const auto& right) {
            if (left.name != right.name) return left.name < right.name;
            if (left.type != right.type) return left.type < right.type;
            return left.value < right.value;
        });
        append_integer(output, static_cast<std::uint32_t>(properties.size()));
        for (const auto& property : properties) append_block_property(output, property);
        return Result<std::vector<std::uint8_t>>::success(std::move(output));
    } catch (const std::exception& error) {
        return Result<std::vector<std::uint8_t>>::failure(
            "canonical block state 失败: " + std::string(error.what()));
    }
}

Result<std::vector<std::uint8_t>> canonical_nbt(std::span<const std::uint8_t> payload)
{
    try {
        const auto compound = read_compound(payload);
        std::vector<std::uint8_t> output{ 'W', 'S', 'N', 'B', 1 };
        append_tag(output, *compound);
        return Result<std::vector<std::uint8_t>>::success(std::move(output));
    } catch (const std::exception& error) {
        return Result<std::vector<std::uint8_t>>::failure(
            "canonical NBT 失败: " + std::string(error.what()));
    }
}

Result<std::vector<CanonicalNbtField>> canonical_nbt_fields(std::span<const std::uint8_t> payload)
{
    try {
        const auto compound = read_compound(payload);
        std::vector<CanonicalNbtField> fields;
        std::vector<std::pair<std::string_view, const nbt::tag*>> children;
        children.reserve(compound->size());
        for (const auto& [key, child] : *compound) children.emplace_back(key, &child.get());
        std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        for (const auto& [key, child] : children) {
            collect_nbt_fields(fields, "/" + escape_json_pointer(key), *child);
        }
        return Result<std::vector<CanonicalNbtField>>::success(std::move(fields));
    } catch (const std::exception& error) {
        return Result<std::vector<CanonicalNbtField>>::failure(
            "canonical NBT 字段失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
