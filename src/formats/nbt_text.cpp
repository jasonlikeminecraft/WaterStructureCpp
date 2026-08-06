#include "nbt_text.hpp"

#include <io/stream_writer.h>
#include <nbt_tags.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace water_structure {
namespace {

using TagPtr = std::unique_ptr<nbt::tag>;

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

void append_utf8(std::string& output, std::uint32_t codepoint)
{
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        throw std::runtime_error("Unicode codepoint 越界");
    }
}

class SnbtParser {
public:
    explicit SnbtParser(std::string_view input) : mInput(input) {}

    TagPtr parse()
    {
        skip_space();
        auto result = parse_value();
        skip_space();
        if (mPosition != mInput.size()) fail("根标签后存在多余内容");
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const
    {
        throw std::runtime_error(
            std::string(message) + " (SNBT offset " + std::to_string(mPosition) + ")");
    }

    void skip_space()
    {
        while (mPosition < mInput.size()) {
            const auto c = static_cast<unsigned char>(mInput[mPosition]);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++mPosition;
        }
    }

    bool consume(char expected)
    {
        skip_space();
        if (mPosition >= mInput.size() || mInput[mPosition] != expected) return false;
        ++mPosition;
        return true;
    }

    void expect(char expected, std::string_view message)
    {
        if (!consume(expected)) fail(message);
    }

    char hex_digit()
    {
        if (mPosition >= mInput.size()) fail("Unicode 转义被截断");
        const auto c = mInput[mPosition++];
        if (c >= '0' && c <= '9') return static_cast<char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<char>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<char>(c - 'A' + 10);
        fail("Unicode 转义包含非法十六进制字符");
    }

    std::uint32_t unicode_escape()
    {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = value * 16 + static_cast<unsigned char>(hex_digit());
        return value;
    }

    std::string quoted_string()
    {
        skip_space();
        if (mPosition >= mInput.size() || (mInput[mPosition] != '"' && mInput[mPosition] != '\'')) {
            fail("期望引号字符串");
        }
        const auto quote = mInput[mPosition++];
        std::string output;
        while (mPosition < mInput.size()) {
            const auto c = mInput[mPosition++];
            if (c == quote) return output;
            if (c != '\\') {
                output.push_back(c);
                continue;
            }
            if (mPosition >= mInput.size()) fail("字符串转义被截断");
            const auto escaped = mInput[mPosition++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\'': output.push_back('\''); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                auto codepoint = unicode_escape();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (mPosition + 2 > mInput.size() || mInput[mPosition] != '\\' ||
                        mInput[mPosition + 1] != 'u') fail("Unicode 高代理项缺少低代理项");
                    mPosition += 2;
                    const auto low = unicode_escape();
                    if (low < 0xdc00 || low > 0xdfff) fail("Unicode 低代理项无效");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    fail("Unicode 低代理项缺少高代理项");
                }
                append_utf8(output, codepoint);
                break;
            }
            default:
                // Bedrock vendor SNBT often escapes punctuation more broadly
                // than JSON. Preserve the escaped byte in those cases.
                output.push_back(escaped);
                break;
            }
        }
        fail("字符串没有结束引号");
    }

    std::string bare_token(bool key = false)
    {
        skip_space();
        const auto start = mPosition;
        while (mPosition < mInput.size()) {
            const auto c = mInput[mPosition];
            if (c == ',' || c == '}' || c == ']' || (key && c == ':') ||
                c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            ++mPosition;
        }
        if (mPosition == start) fail(key ? "compound key 为空" : "标签值为空");
        return std::string(mInput.substr(start, mPosition - start));
    }

    std::string parse_key()
    {
        skip_space();
        if (mPosition < mInput.size() && (mInput[mPosition] == '"' || mInput[mPosition] == '\'')) {
            return quoted_string();
        }
        return bare_token(true);
    }

    template <typename T>
    T parse_integer_value(std::string_view token, std::string_view type)
    {
        T result{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
            fail(std::string(type) + " 数值无效");
        }
        return result;
    }

    TagPtr numeric_or_string(std::string token)
    {
        if (token == "true" || token == "TRUE") return std::make_unique<nbt::tag_byte>(1);
        if (token == "false" || token == "FALSE") return std::make_unique<nbt::tag_byte>(0);
        if (token.empty()) fail("标签值为空");

        const auto suffix = token.back();
        const auto has_suffix = suffix == 'b' || suffix == 'B' || suffix == 's' || suffix == 'S' ||
            suffix == 'l' || suffix == 'L' || suffix == 'f' || suffix == 'F' || suffix == 'd' || suffix == 'D';
        const auto number = std::string_view(token).substr(0, token.size() - (has_suffix ? 1 : 0));
        const auto looks_numeric = !number.empty() &&
            (number.front() == '+' || number.front() == '-' || (number.front() >= '0' && number.front() <= '9'));
        if (!looks_numeric) return std::make_unique<nbt::tag_string>(std::move(token));

        try {
            if (suffix == 'b' || suffix == 'B') {
                return std::make_unique<nbt::tag_byte>(parse_integer_value<std::int8_t>(number, "byte"));
            }
            if (suffix == 's' || suffix == 'S') {
                return std::make_unique<nbt::tag_short>(parse_integer_value<std::int16_t>(number, "short"));
            }
            if (suffix == 'l' || suffix == 'L') {
                return std::make_unique<nbt::tag_long>(parse_integer_value<std::int64_t>(number, "long"));
            }
            if (suffix == 'f' || suffix == 'F' || suffix == 'd' || suffix == 'D' ||
                number.find_first_of(".eE") != std::string_view::npos) {
                char* end = nullptr;
                const auto numeric = std::string(number);
                const auto value = std::strtod(numeric.c_str(), &end);
                if (end != numeric.c_str() + numeric.size() || !std::isfinite(value)) {
                    fail("浮点数值无效");
                }
                if (suffix == 'f' || suffix == 'F') {
                    return std::make_unique<nbt::tag_float>(static_cast<float>(value));
                }
                return std::make_unique<nbt::tag_double>(value);
            }
            return std::make_unique<nbt::tag_int>(parse_integer_value<std::int32_t>(number, "int"));
        } catch (const std::runtime_error&) {
            throw;
        }
    }

    TagPtr parse_compound()
    {
        expect('{', "期望 compound 起始符");
        auto compound = std::make_unique<nbt::tag_compound>();
        skip_space();
        if (consume('}')) return compound;
        while (true) {
            auto key = parse_key();
            expect(':', "compound key 后缺少冒号");
            auto value = parse_value();
            compound->operator[](key).set_ptr(std::move(value));
            if (consume('}')) return compound;
            expect(',', "compound 项之间缺少逗号");
        }
    }

    TagPtr parse_typed_array(char type)
    {
        expect(';', "typed array 缺少分号");
        if (type == 'B' || type == 'b') {
            std::vector<std::int8_t> values;
            if (consume(']')) return std::make_unique<nbt::tag_byte_array>(std::move(values));
            while (true) {
                auto value = parse_value();
                if (value->get_type() != nbt::tag_type::Byte) fail("byte array 包含非 byte 值");
                values.push_back(value->as<nbt::tag_byte>().get());
                if (consume(']')) break;
                expect(',', "byte array 项之间缺少逗号");
            }
            return std::make_unique<nbt::tag_byte_array>(std::move(values));
        }
        if (type == 'I' || type == 'i') {
            std::vector<std::int32_t> values;
            if (consume(']')) return std::make_unique<nbt::tag_int_array>(std::move(values));
            while (true) {
                auto value = parse_value();
                if (value->get_type() != nbt::tag_type::Int) fail("int array 包含非 int 值");
                values.push_back(value->as<nbt::tag_int>().get());
                if (consume(']')) break;
                expect(',', "int array 项之间缺少逗号");
            }
            return std::make_unique<nbt::tag_int_array>(std::move(values));
        }
        std::vector<std::int64_t> values;
        if (consume(']')) return std::make_unique<nbt::tag_long_array>(std::move(values));
        while (true) {
            auto value = parse_value();
            if (value->get_type() != nbt::tag_type::Long) fail("long array 包含非 long 值");
            values.push_back(value->as<nbt::tag_long>().get());
            if (consume(']')) break;
            expect(',', "long array 项之间缺少逗号");
        }
        return std::make_unique<nbt::tag_long_array>(std::move(values));
    }

    TagPtr parse_list()
    {
        expect('[', "期望 list 起始符");
        skip_space();
        if (mPosition + 1 < mInput.size() &&
            (mInput[mPosition] == 'B' || mInput[mPosition] == 'b' ||
             mInput[mPosition] == 'I' || mInput[mPosition] == 'i' ||
             mInput[mPosition] == 'L' || mInput[mPosition] == 'l') &&
            mInput[mPosition + 1] == ';') {
            return parse_typed_array(mInput[mPosition++]);
        }
        auto list = std::make_unique<nbt::tag_list>();
        if (consume(']')) return list;
        while (true) {
            auto value = parse_value();
            try {
                list->push_back(nbt::value_initializer(std::move(value)));
            } catch (const std::invalid_argument&) {
                fail("NBT list 包含不同 tag 类型");
            }
            if (consume(']')) return list;
            expect(',', "list 项之间缺少逗号");
        }
    }

    TagPtr parse_value()
    {
        skip_space();
        if (mPosition >= mInput.size()) fail("标签值被截断");
        if (mInput[mPosition] == '{') return parse_compound();
        if (mInput[mPosition] == '[') return parse_list();
        if (mInput[mPosition] == '"' || mInput[mPosition] == '\'') {
            return std::make_unique<nbt::tag_string>(quoted_string());
        }
        return numeric_or_string(bare_token());
    }

    std::string_view mInput;
    std::size_t mPosition = 0;
};

TagPtr json_tag(const nlohmann::json& value)
{
    if (value.is_null()) throw std::runtime_error("JSON NBT 不支持 null");
    if (value.is_boolean()) {
        return std::make_unique<nbt::tag_byte>(value.get<bool>() ? 1 : 0);
    }
    // Go's encoding/json fallback decodes every JSON number as float64.
    if (value.is_number()) return std::make_unique<nbt::tag_double>(value.get<double>());
    if (value.is_string()) return std::make_unique<nbt::tag_string>(value.get<std::string>());
    if (value.is_object()) {
        auto compound = std::make_unique<nbt::tag_compound>();
        for (const auto& [key, item] : value.items()) {
            compound->operator[](key).set_ptr(json_tag(item));
        }
        return compound;
    }
    if (value.is_array()) {
        auto list = std::make_unique<nbt::tag_list>();
        for (const auto& item : value) {
            try {
                list->push_back(nbt::value_initializer(json_tag(item)));
            } catch (const std::invalid_argument&) {
                throw std::runtime_error("JSON NBT 数组包含不同 tag 类型");
            }
        }
        return list;
    }
    throw std::runtime_error("JSON NBT 包含不支持的值类型");
}

NbtPayload serialize_compound(const nbt::tag_compound& compound)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", compound, output, endian::little);
    const auto bytes = output.str();
    return NbtPayload(bytes.begin(), bytes.end());
}

std::string query_unescape(std::string_view input)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+') {
            output.push_back(' ');
        } else if (input[i] == '%') {
            if (i + 2 >= input.size()) throw std::runtime_error("URL percent escape 被截断");
            const auto high = hex(input[i + 1]);
            const auto low = hex(input[i + 2]);
            if (high < 0 || low < 0) throw std::runtime_error("URL percent escape 无效");
            output.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else {
            output.push_back(input[i]);
        }
    }
    return output;
}

TagPtr decode_snbt(std::string_view encoded)
{
    auto decoded = trim(query_unescape(encoded));
    if (decoded.empty()) return {};
    if (decoded.starts_with("\"\"")) decoded = "{" + decoded + "}";
    if (decoded.front() != '{' && decoded.front() != '[') {
        auto compound = std::make_unique<nbt::tag_compound>();
        compound->operator[]("" ) = nbt::tag_string(std::move(decoded));
        return compound;
    }
    return SnbtParser(decoded).parse();
}

NbtPayload compound_payload(TagPtr root)
{
    if (!root) return {};
    if (root->get_type() != nbt::tag_type::Compound) {
        throw std::runtime_error("NBT 根标签不是 compound");
    }
    auto& compound = root->as<nbt::tag_compound>();
    if (compound.has_key("", nbt::tag_type::Compound)) {
        return serialize_compound(compound.at("").as<nbt::tag_compound>());
    }
    return serialize_compound(compound);
}

} // namespace

Result<NbtPayload> json_compound_to_nbt(const nlohmann::json& input)
{
    try {
        if (!input.is_object()) {
            return Result<NbtPayload>::failure("JSON NBT 根节点不是 object");
        }
        return Result<NbtPayload>::success(compound_payload(json_tag(input)));
    } catch (const std::exception& error) {
        return Result<NbtPayload>::failure("JSON 转 typed NBT 失败: " + std::string(error.what()));
    }
}

Result<NbtPayload> parse_mianyang_nbt(std::string_view input)
{
    try {
        const auto candidate = trim(input);
        if (candidate.empty()) return Result<NbtPayload>::success({});
        const auto parsed = nlohmann::json::parse(candidate, nullptr, false);
        if (parsed.is_object()) {
            if (const auto complete = parsed.find("blockCompleteNBT");
                complete != parsed.end() && complete->is_string()) {
                return Result<NbtPayload>::success(compound_payload(decode_snbt(complete->get<std::string>())));
            }
            return json_compound_to_nbt(parsed);
        }
        return Result<NbtPayload>::success(compound_payload(decode_snbt(candidate)));
    } catch (const std::exception& error) {
        return Result<NbtPayload>::failure("解析 MianYang typed NBT 失败: " + std::string(error.what()));
    }
}

} // namespace water_structure
