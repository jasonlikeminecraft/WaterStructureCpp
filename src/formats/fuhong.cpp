#include "fuhong.hpp"

#include "nbt_text.hpp"

#include <WaterStructure/world.hpp>

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <nbt_tags.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::string_view kFuHongV5Key = "FuHongBuild";
constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;

struct Bounds {
    bool populated = false;
    BlockPos minimum{};
    BlockPos maximum{};

    void add(BlockPos pos)
    {
        if (!populated) {
            populated = true;
            minimum = maximum = pos;
            return;
        }
        minimum.x = std::min(minimum.x, pos.x);
        minimum.y = std::min(minimum.y, pos.y);
        minimum.z = std::min(minimum.z, pos.z);
        maximum.x = std::max(maximum.x, pos.x);
        maximum.y = std::max(maximum.y, pos.y);
        maximum.z = std::max(maximum.z, pos.z);
    }

    Size size(std::string_view format) const
    {
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (!populated || width <= 0 || height <= 0 || length <= 0 ||
            width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error(std::string(format) + " bounds are invalid");
        }
        return { static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length) };
    }
};

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n\"");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n\"");
    return std::string(value.substr(first, last - first + 1));
}

std::string json_string(const nlohmann::json& value)
{
    if (value.is_null()) return "<nil>";
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

std::int64_t integer(const nlohmann::json& value, std::string_view field)
{
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(number);
        }
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (std::isfinite(number) &&
            number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(number);
        }
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        std::int64_t number = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return number;
    }
    throw std::runtime_error(std::string(field) + " is not an integer");
}

std::int32_t i32(const nlohmann::json& value, std::string_view field)
{
    const auto number = integer(value, field);
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(number);
}

std::int32_t checked_add(std::int32_t left, std::int32_t right, std::string_view field)
{
    const auto result = static_cast<std::int64_t>(left) + right;
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(result);
}

std::int32_t first_coordinate(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_array()) return i32(value, field);
    if (value.empty()) throw std::runtime_error(std::string(field) + " coordinate array is empty");
    return i32(value.front(), field);
}

BlockPos local_position(BlockPos world, BlockPos origin)
{
    return { world.x - origin.x, world.y - origin.y, world.z - origin.z };
}

NbtPayload serialize_nbt(const nbt::tag_compound& root)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", root, output, endian::little);
    const auto bytes = output.str();
    return { bytes.begin(), bytes.end() };
}

std::unique_ptr<nbt::tag_compound> decode_compound(const NbtPayload& payload)
{
    const std::string bytes(payload.begin(), payload.end());
    std::istringstream input(bytes, std::ios::binary);
    auto [_, root] = nbt::io::read_tag(input, endian::little);
    if (!root || root->get_type() != nbt::tag_type::Compound) return {};
    return std::unique_ptr<nbt::tag_compound>(
        static_cast<nbt::tag_compound*>(root.release()));
}

std::int8_t auto_byte(const nlohmann::json& value)
{
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_string()) {
        auto text = trim(value.get<std::string>());
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (text == "true") return 1;
        if (text == "false" || text.empty()) return 0;
    }
    try { return integer(value, "auto") != 0 ? 1 : 0; } catch (...) { return 0; }
}

std::unique_ptr<nbt::tag_compound> command_nbt(const nlohmann::json& values)
{
    if (!values.is_array() || values.empty()) return {};
    const auto command = json_string(values[0]);
    std::int32_t delay = 0;
    if (values.size() > 1) {
        try { delay = static_cast<std::int32_t>(integer(values[1], "TickDelay")); } catch (...) {}
    }
    const auto automatic = values.size() > 2 ? auto_byte(values[2]) : 0;
    const auto custom_name = values.size() > 3 ? json_string(values[3]) : std::string{};
    auto root = std::make_unique<nbt::tag_compound>();
    (*root)["id"] = nbt::tag_string("CommandBlock");
    (*root)["Command"] = nbt::tag_string(command);
    (*root)["CustomName"] = nbt::tag_string(custom_name);
    root->emplace<nbt::tag_byte>("ExecuteOnFirstTick", 1);
    root->emplace<nbt::tag_byte>("auto", automatic);
    root->emplace<nbt::tag_int>("TickDelay", delay);
    root->emplace<nbt::tag_byte>("conditionalMode", 0);
    root->emplace<nbt::tag_byte>("TrackOutput", 1);
    root->emplace<nbt::tag_int>("Version", command.find("execute") != std::string::npos ? 38 : 19);
    return root;
}

std::string container_id(std::string_view raw_name)
{
    const auto name = trim(raw_name);
    if (name == "minecraft:blast_furnace" || name == "minecraft:lit_blast_furnace") return "BlastFurnace";
    if (name == "minecraft:furnace" || name == "minecraft:lit_furnace") return "Furnace";
    if (name == "minecraft:smoker" || name == "minecraft:lit_smoker") return "Smoker";
    if (name == "minecraft:chest" || name == "minecraft:trapped_chest") return "Chest";
    if (name == "minecraft:hopper") return "Hopper";
    if (name == "minecraft:dispenser") return "Dispenser";
    if (name == "minecraft:dropper") return "Dropper";
    if (name == "minecraft:barrel") return "Barrel";
    if (name == "minecraft:crafter") return "Crafter";
    if (name.find("shulker_box") != std::string_view::npos) return "ShulkerBox";
    return {};
}

std::int64_t integer_or_zero(const nlohmann::json& object,
    std::initializer_list<std::string_view> names)
{
    for (const auto name : names) {
        const auto found = object.find(std::string(name));
        if (found == object.end() || found->is_null()) continue;
        try { return integer(*found, name); } catch (...) { return 0; }
    }
    return 0;
}

std::string string_field(const nlohmann::json& object,
    std::initializer_list<std::string_view> names)
{
    for (const auto name : names) {
        const auto found = object.find(std::string(name));
        if (found == object.end() || found->is_null()) continue;
        return json_string(*found);
    }
    return {};
}

std::string item_name(std::string name)
{
    name = trim(name);
    if (!name.empty() && name.find(':') == std::string::npos) name.insert(0, "minecraft:");
    return name;
}

nbt::tag_compound item_block_nbt(const std::string& name)
{
    nbt::tag_compound block;
    block["name"] = nbt::tag_string(name);
    block["states"] = nbt::tag_compound{};
    block.emplace<nbt::tag_short>("val", 0);
    block.emplace<nbt::tag_int>("version", 17959425);
    return block;
}

std::unique_ptr<nbt::tag_compound> container_nbt_v3(
    std::string_view block_name, const nlohmann::json& payload)
{
    const auto id = container_id(block_name);
    if (id.empty()) return {};
    nbt::tag_list items(nbt::tag_type::Compound);
    if (payload.is_array()) {
        for (const auto& raw : payload) {
            std::string name;
            std::int64_t damage = 0, count = 0, slot = 0;
            if (raw.is_array() && raw.size() >= 4) {
                name = item_name(json_string(raw[0]));
                try { damage = integer(raw[1], "Damage"); } catch (...) {}
                try { count = integer(raw[2], "Count"); } catch (...) {}
                try { slot = integer(raw[3], "Slot"); } catch (...) {}
            } else if (raw.is_object()) {
                name = item_name(string_field(raw, { "Name", "name", "ns" }));
                damage = integer_or_zero(raw, { "Damage", "damage", "aux" });
                count = integer_or_zero(raw, { "Count", "count", "num" });
                slot = integer_or_zero(raw, { "Slot", "slot" });
            } else {
                continue;
            }
            if (name.empty()) continue;
            nbt::tag_compound item;
            item["Name"] = nbt::tag_string(name);
            item.emplace<nbt::tag_byte>("Count", static_cast<std::int8_t>(count));
            item.emplace<nbt::tag_short>("Damage", static_cast<std::int16_t>(damage));
            item.emplace<nbt::tag_byte>("Slot", static_cast<std::int8_t>(slot));
            item["Block"] = item_block_nbt(name);
            items.push_back(std::move(item));
        }
    }
    auto root = std::make_unique<nbt::tag_compound>();
    (*root)["id"] = nbt::tag_string(id);
    root->emplace<nbt::tag_byte>("Findable", 0);
    root->emplace<nbt::tag_byte>("IsOpened", 0);
    root->emplace<nbt::tag_byte>("isMovable", 1);
    (*root)["Items"] = std::move(items);
    return root;
}

std::unique_ptr<nbt::tag_compound> container_nbt_v2(
    std::string_view block_name, const nlohmann::json& raw)
{
    if (!raw.is_array() || raw.empty()) return {};
    nbt::tag_list items(nbt::tag_type::Compound);
    for (const auto& value : raw) {
        if (!value.is_object()) continue;
        auto name = item_name(string_field(value, { "name", "Name" }));
        nbt::tag_compound item;
        item["Name"] = nbt::tag_string(name);
        item.emplace<nbt::tag_short>("Damage", static_cast<std::int16_t>(
            integer_or_zero(value, { "damage", "Damage" })));
        item.emplace<nbt::tag_byte>("Count", static_cast<std::int8_t>(
            integer_or_zero(value, { "count", "Count" })));
        item.emplace<nbt::tag_byte>("Slot", static_cast<std::int8_t>(
            integer_or_zero(value, { "slot", "Slot" })));
        items.push_back(std::move(item));
    }
    if (items.size() == 0) return {};
    auto root = std::make_unique<nbt::tag_compound>();
    if (const auto id = container_id(block_name); !id.empty()) (*root)["id"] = nbt::tag_string(id);
    (*root)["Items"] = std::move(items);
    return root;
}

std::unique_ptr<nbt::tag_compound> sign_nbt(
    std::string_view block_name, const nlohmann::json& payload)
{
    std::string text;
    if (payload.is_array()) {
        for (std::size_t index = 0; index < payload.size(); ++index) {
            if (index != 0) text.push_back('\n');
            text += json_string(payload[index]);
        }
    } else {
        text = json_string(payload);
    }
    const auto name = trim(block_name);
    const auto hanging = name.size() >= 12 && name.ends_with("hanging_sign");
    auto text_compound = [](std::string value) {
        nbt::tag_compound result;
        result["FilteredText"] = nbt::tag_string("");
        result.emplace<nbt::tag_byte>("HideGlowOutline", 0);
        result.emplace<nbt::tag_byte>("IgnoreLighting", 0);
        result.emplace<nbt::tag_byte>("PersistFormatting", 1);
        result.emplace<nbt::tag_int>("SignTextColor", -16777216);
        result["Text"] = nbt::tag_string(std::move(value));
        result["TextOwner"] = nbt::tag_string("");
        return result;
    };
    auto root = std::make_unique<nbt::tag_compound>();
    (*root)["id"] = nbt::tag_string(hanging ? "HangingSign" : "Sign");
    root->emplace<nbt::tag_byte>("IsWaxed", 0);
    root->emplace<nbt::tag_byte>("isMovable", 1);
    (*root)["BackText"] = text_compound("");
    (*root)["FrontText"] = text_compound(std::move(text));
    return root;
}

std::unique_ptr<nbt::tag_compound> extra_nbt(
    std::string_view block_name, const nlohmann::json& payload)
{
    auto lower = std::string(block_name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("command_block") != std::string::npos) return command_nbt(payload);
    if (!container_id(block_name).empty()) return container_nbt_v3(block_name, payload);
    if (lower.find("sign") != std::string::npos) return sign_nbt(block_name, payload);
    return {};
}

// V5 used to materialize compressed input, inflated ciphertext and decrypted
// JSON at the same time.  These two stream buffers retain only 64 KiB windows;
// nlohmann consumes the decrypted bytes directly from the outer stream.
class FuHongV5InflateBuffer final : public std::streambuf {
public:
    explicit FuHongV5InflateBuffer(const std::filesystem::path& path)
        : mInput(path, std::ios::binary)
    {
        if (!mInput) throw std::runtime_error("cannot open FuHongV5 file: " + path.string());
        if (inflateInit(&mStream) != Z_OK) {
            throw std::runtime_error("initialize FuHongV5 zlib failed");
        }
        mInitialized = true;
        setg(mDecoded.data(), mDecoded.data(), mDecoded.data());
    }

    ~FuHongV5InflateBuffer() override
    {
        if (mInitialized) inflateEnd(&mStream);
    }

protected:
    int_type underflow() override
    {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (mFinished) return traits_type::eof();

        while (true) {
            if (mStream.avail_in == 0 && !mInputEof) {
                mInput.read(reinterpret_cast<char*>(mCompressed.data()),
                    static_cast<std::streamsize>(mCompressed.size()));
                const auto count = mInput.gcount();
                if (count > 0) {
                    mStream.next_in = mCompressed.data();
                    mStream.avail_in = static_cast<uInt>(count);
                } else {
                    if (mInput.bad()) {
                        throw std::runtime_error(
                            "FuHongV5 zlib read failed at compressed offset " +
                            std::to_string(mStream.total_in));
                    }
                    mInputEof = true;
                }
            }

            mStream.next_out = reinterpret_cast<Bytef*>(mDecoded.data());
            mStream.avail_out = static_cast<uInt>(mDecoded.size());
            const auto status = inflate(&mStream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                throw std::runtime_error(
                    "FuHongV5 zlib decode failed at compressed offset " +
                    std::to_string(mStream.total_in));
            }
            const auto produced = mDecoded.size() - mStream.avail_out;
            if (mDecodedBytes > kMaxDecodedBytes - produced) {
                throw std::runtime_error("FuHongV5 decoded payload exceeds 2 GiB");
            }
            mDecodedBytes += produced;
            if (status == Z_STREAM_END) mFinished = true;
            if (mInputEof && !mFinished && produced == 0) {
                throw std::runtime_error(
                    "FuHongV5 zlib stream truncated at compressed offset " +
                    std::to_string(mStream.total_in));
            }
            if (produced != 0) {
                setg(mDecoded.data(), mDecoded.data(), mDecoded.data() + produced);
                return traits_type::to_int_type(*gptr());
            }
            if (mFinished) return traits_type::eof();
        }
    }

private:
    std::ifstream mInput;
    z_stream mStream{};
    bool mInitialized = false;
    bool mInputEof = false;
    bool mFinished = false;
    std::size_t mDecodedBytes = 0;
    std::array<Bytef, kStreamChunk> mCompressed{};
    std::array<char, kStreamChunk> mDecoded{};
};

class FuHongV5DecryptBuffer final : public std::streambuf {
public:
    explicit FuHongV5DecryptBuffer(std::streambuf& encrypted) : mEncrypted(encrypted)
    {
        setg(mPlain.data(), mPlain.data(), mPlain.data());
    }

protected:
    int_type underflow() override
    {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (mFinished) return traits_type::eof();

        std::size_t produced = 0;
        while (produced + 4 <= mPlain.size()) {
            const auto first = read_byte();
            if (!first) {
                if (mRuneIndex == 0) {
                    throw std::runtime_error("FuHongV5 payload is empty");
                }
                mFinished = true;
                break;
            }
            const auto rune_offset = mEncryptedOffset - 1;
            std::size_t length = 0;
            std::uint32_t value = 0;
            if (*first < 0x80) { length = 1; value = *first; }
            else if ((*first & 0xe0) == 0xc0) { length = 2; value = *first & 0x1f; }
            else if ((*first & 0xf0) == 0xe0) { length = 3; value = *first & 0x0f; }
            else if ((*first & 0xf8) == 0xf0) { length = 4; value = *first & 0x07; }
            else {
                throw std::runtime_error(
                    "FuHongV5 invalid UTF-8 at decoded offset " +
                    std::to_string(rune_offset));
            }
            for (std::size_t index = 1; index < length; ++index) {
                const auto next = read_byte();
                if (!next) {
                    throw std::runtime_error(
                        "FuHongV5 UTF-8 truncated at decoded offset " +
                        std::to_string(rune_offset));
                }
                if ((*next & 0xc0) != 0x80) {
                    throw std::runtime_error(
                        "FuHongV5 invalid UTF-8 at decoded offset " +
                        std::to_string(rune_offset));
                }
                value = (value << 6) | (*next & 0x3f);
            }
            constexpr std::array<std::uint32_t, 5> minimum{
                0, 0, 0x80, 0x800, 0x10000
            };
            if (value < minimum[length] || value > 0x10ffff ||
                (value >= 0xd800 && value <= 0xdfff)) {
                throw std::runtime_error(
                    "FuHongV5 invalid UTF-8 rune at decoded offset " +
                    std::to_string(rune_offset));
            }

            const auto shifted = static_cast<std::int64_t>(value) -
                static_cast<std::int64_t>(mRuneIndex % 3);
            const auto plain = shifted ^ static_cast<unsigned char>(
                kFuHongV5Key[mRuneIndex % kFuHongV5Key.size()]);
            if (plain < 0 || plain > 0x10ffff ||
                (plain >= 0xd800 && plain <= 0xdfff)) {
                throw std::runtime_error(
                    "FuHongV5 invalid decrypted rune at rune index " +
                    std::to_string(mRuneIndex));
            }
            produced += append_utf8(
                mPlain.data() + produced, static_cast<std::uint32_t>(plain));
            ++mRuneIndex;
        }

        if (produced == 0) return traits_type::eof();
        setg(mPlain.data(), mPlain.data(), mPlain.data() + produced);
        return traits_type::to_int_type(*gptr());
    }

private:
    std::optional<std::uint8_t> read_byte()
    {
        if (mEncryptedCursor == mEncryptedSize) {
            const auto count = mEncrypted.sgetn(
                reinterpret_cast<char*>(mEncryptedChunk.data()),
                static_cast<std::streamsize>(mEncryptedChunk.size()));
            if (count <= 0) return std::nullopt;
            mEncryptedCursor = 0;
            mEncryptedSize = static_cast<std::size_t>(count);
        }
        ++mEncryptedOffset;
        return mEncryptedChunk[mEncryptedCursor++];
    }

    static std::size_t append_utf8(char* output, std::uint32_t value)
    {
        if (value <= 0x7f) {
            output[0] = static_cast<char>(value);
            return 1;
        }
        if (value <= 0x7ff) {
            output[0] = static_cast<char>(0xc0 | (value >> 6));
            output[1] = static_cast<char>(0x80 | (value & 0x3f));
            return 2;
        }
        if (value <= 0xffff) {
            output[0] = static_cast<char>(0xe0 | (value >> 12));
            output[1] = static_cast<char>(0x80 | ((value >> 6) & 0x3f));
            output[2] = static_cast<char>(0x80 | (value & 0x3f));
            return 3;
        }
        output[0] = static_cast<char>(0xf0 | (value >> 18));
        output[1] = static_cast<char>(0x80 | ((value >> 12) & 0x3f));
        output[2] = static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        output[3] = static_cast<char>(0x80 | (value & 0x3f));
        return 4;
    }

    std::streambuf& mEncrypted;
    bool mFinished = false;
    std::size_t mEncryptedOffset = 0;
    std::size_t mRuneIndex = 0;
    std::size_t mEncryptedCursor = 0;
    std::size_t mEncryptedSize = 0;
    std::array<std::uint8_t, kStreamChunk> mEncryptedChunk{};
    std::array<char, kStreamChunk> mPlain{};
};

class FuHongV5InputStream final : public std::istream {
public:
    explicit FuHongV5InputStream(const std::filesystem::path& path)
        : std::istream(nullptr), mInflated(path), mDecrypted(mInflated)
    {
        rdbuf(&mDecrypted);
        clear();
        exceptions(std::ios::badbit);
    }

private:
    FuHongV5InflateBuffer mInflated;
    FuHongV5DecryptBuffer mDecrypted;
};

template <typename Callback>
void parse_json_pass(const std::filesystem::path& path, StructureId version,
                     Callback&& callback)
{
    if (version == StructureId::FuHongV5) {
        FuHongV5InputStream input(path);
        const auto discarded = nlohmann::json::parse(
            input, std::forward<Callback>(callback));
        (void)discarded;
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot open " + std::string(version == StructureId::FuHongV1 ? "FuHongV1" :
                version == StructureId::FuHongV2 ? "FuHongV2" :
                version == StructureId::FuHongV3 ? "FuHongV3" : "FuHongV4") +
            " file: " + path.string());
    }
    const auto discarded = nlohmann::json::parse(
        input, std::forward<Callback>(callback));
    (void)discarded;
}

} // namespace

std::string_view FuHongStructure::name() const noexcept
{
    switch (mVersion) {
    case StructureId::FuHongV1: return "FuHongV1";
    case StructureId::FuHongV2: return "FuHongV2";
    case StructureId::FuHongV3: return "FuHongV3";
    case StructureId::FuHongV4: return "FuHongV4";
    case StructureId::FuHongV5: return "FuHongV5";
    default: return "FuHong";
    }
}

std::uint32_t FuHongStructure::runtime_id(
    std::string name, std::int64_t aux, const std::map<std::string, std::string>& states)
{
    name = trim(name);
    auto effective_states = states;
    if (const auto bracket = name.find('['); bracket != std::string::npos && name.ends_with(']')) {
        const auto encoded_states = name.substr(bracket + 1, name.size() - bracket - 2);
        name = trim(name.substr(0, bracket));
        std::size_t begin = 0;
        while (begin <= encoded_states.size()) {
            const auto end = encoded_states.find(',', begin);
            const auto pair = trim(encoded_states.substr(begin,
                end == std::string::npos ? std::string::npos : end - begin));
            if (const auto equals = pair.find('='); equals != std::string::npos) {
                effective_states.try_emplace(
                    trim(pair.substr(0, equals)), trim(pair.substr(equals + 1)));
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    std::ostringstream key;
    key << name << '|';
    if (effective_states.empty()) key << aux;
    else for (const auto& [property, value] : effective_states) key << property << '=' << value << ',';
    if (const auto cached = mPaletteCache.find(key.str()); cached != mPaletteCache.end()) {
        return cached->second;
    }

    std::optional<std::uint32_t> runtime;
    if (!effective_states.empty()) {
        // FuHong V2 stores a partial Bedrock state map. The Go block resolver
        // fills omitted properties with the palette default before lookup.
        if (const auto base = mRegistry.find(name); base) {
            if (auto state = mRegistry.state(*base)) {
                auto merged = state->states;
                for (auto& property : merged) {
                    if (const auto provided = effective_states.find(property.name);
                        provided != effective_states.end()) {
                        property.value = provided->second;
                        if (property.value == "true") property.value = "1";
                        else if (property.value == "false") property.value = "0";
                    }
                }
                for (const auto& [property_name, property_value] : effective_states) {
                    if (std::ranges::none_of(merged, [&](const auto& item) {
                        return item.name == property_name;
                    })) {
                        BlockStateProperty property;
                        property.name = property_name;
                        property.value = property_value;
                        if (property.value == "true" || property.value == "false") {
                            property.type = BlockStateValueType::Byte;
                            property.value = property.value == "true" ? "1" : "0";
                        } else {
                            const auto is_number = !property.value.empty() &&
                                std::ranges::all_of(property.value, [](unsigned char c) {
                                    return std::isdigit(c) || c == '-';
                                });
                            property.type = is_number ? BlockStateValueType::Int : BlockStateValueType::String;
                        }
                        merged.push_back(std::move(property));
                    }
                }
                runtime = mRegistry.find(name, merged);
            }
        }
        std::ostringstream java;
        if (!runtime) {
            java << name << '[';
            std::size_t index = 0;
            for (const auto& [property, value] : effective_states) {
                if (index++ != 0) java << ',';
                java << property << '=' << value;
            }
            java << ']';
            runtime = mRegistry.java_runtime_id(java.str());
        }
    }
    if (!runtime) runtime = mRegistry.legacy_runtime_id(name, static_cast<std::uint16_t>(aux));
    if (!runtime) runtime = mRegistry.java_runtime_id(name);
    if (!runtime) {
        if (const auto unknown = mRegistry.find("minecraft:unknown")) runtime = *unknown;
        else runtime = mRegistry.register_state({ "minecraft:unknown", {}, 0 });
    }
    mPaletteCache.emplace(key.str(), *runtime);
    return *runtime;
}

Result<void> FuHongStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mPaletteCache.clear();
    mNonAirBlocks = 0;
    try {
        Bounds bounds;
        std::size_t materialized_blocks = 0;

        if (mVersion == StructureId::FuHongV1) {
            // The first SAX pass determines the origin.  The second pass writes
            // directly into SparseBlockStore, so there is never a second full
            // coordinate->block map beside the final sparse representation.
            const auto parse_v1 = [&](bool materialize) {
                bool root_is_array = false;
                std::size_t block_index = 0;
                const auto consume = [&](const nlohmann::json& raw) {
                    const auto index = block_index++;
                    try {
                        if (!raw.is_object()) {
                            throw std::runtime_error("block is not an object");
                        }
                        const auto block_name = trim(raw.value("name", std::string{}));
                        if (block_name.empty()) {
                            throw std::runtime_error("block name is empty");
                        }
                        if (!raw.contains("x") || !raw.contains("y") ||
                            !raw.contains("z")) {
                            throw std::runtime_error("block coordinates are missing");
                        }
                        const auto aux = raw.contains("aux") && !raw["aux"].is_null()
                            ? integer(raw["aux"], "aux") : 0;
                        const BlockPos world{ first_coordinate(raw["x"], "x"),
                            first_coordinate(raw["y"], "y"),
                            first_coordinate(raw["z"], "z") };
                        if (materialize) {
                            mStore.put(local_position(world, bounds.minimum),
                                runtime_id(block_name, aux));
                            ++materialized_blocks;
                        } else {
                            bounds.add(world);
                        }
                    } catch (const std::exception& error) {
                        throw std::runtime_error("block index " + std::to_string(index) +
                            ": " + error.what());
                    }
                };
                const auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                                          nlohmann::json& parsed) -> bool {
                    if (depth == 0) {
                        if (event == nlohmann::json::parse_event_t::array_start) {
                            root_is_array = true;
                            return true;
                        }
                        return false;
                    }
                    if (!root_is_array) return false;
                    if (depth == 1) {
                        if (event == nlohmann::json::parse_event_t::object_start ||
                            event == nlohmann::json::parse_event_t::array_start) {
                            return true;
                        }
                        if (event == nlohmann::json::parse_event_t::object_end ||
                            event == nlohmann::json::parse_event_t::array_end ||
                            event == nlohmann::json::parse_event_t::value) {
                            consume(parsed);
                            return false;
                        }
                    }
                    return true;
                };
                parse_json_pass(path, mVersion, callback);
                if (!root_is_array || block_index == 0) {
                    throw std::runtime_error("root is not a non-empty array");
                }
            };

            parse_v1(false);
            if (!bounds.populated) throw std::runtime_error("structure has no valid blocks");
            mStore.set_size(bounds.size(name()));
            parse_v1(true);
        } else if (mVersion == StructureId::FuHongV2) {
            const auto parse_v2 = [&](bool materialize) {
                bool root_is_object = false;
                bool target_seen = false;
                bool target_is_array = false;
                bool in_target = false;
                bool in_chunk = false;
                bool in_block_list = false;
                bool chunk_has_block_list = false;
                std::string root_key;
                std::string chunk_key;
                std::size_t chunk_index = 0;
                std::size_t entry_index = 0;

                const auto consume_entry = [&](const nlohmann::json& entry) {
                    const auto current_entry = entry_index++;
                    if (!entry.is_object() || !entry.contains("n")) return;
                    try {
                            const auto block_name = trim(json_string(entry["n"]));
                            if (block_name.empty()) throw std::runtime_error("block name is empty");
                            const auto& xs = entry.at("x");
                            const auto& ys = entry.at("y");
                            const auto& zs = entry.at("z");
                            if (!xs.is_array()) throw std::runtime_error("x is not an array");
                            if (!ys.is_array()) throw std::runtime_error("y is not an array");
                            if (!zs.is_array()) throw std::runtime_error("z is not an array");
                            if (xs.size() != ys.size() || xs.size() != zs.size()) {
                                throw std::runtime_error("coordinate array lengths differ");
                            }
                            if (!materialize) {
                                for (std::size_t i = 0; i < xs.size(); ++i) {
                                    bounds.add({ i32(xs[i], "x"), i32(ys[i], "y"),
                                        i32(zs[i], "z") });
                                }
                                return;
                            }

                            std::map<std::string, std::string> states;
                            if (const auto raw_states = entry.find("state"); raw_states != entry.end() && raw_states->is_array()) {
                                for (const auto& raw_state : *raw_states) {
                                    if (!raw_state.is_string()) continue;
                                    const auto pair = raw_state.get<std::string>();
                                    if (const auto equals = pair.find('='); equals != std::string::npos) {
                                        states[trim(pair.substr(0, equals))] = trim(pair.substr(equals + 1));
                                    }
                                }
                            }
                            const auto aux = entry.find("a");
                            std::int64_t scalar_aux = 0;
                            if (aux != entry.end() && !aux->is_null() && !aux->is_array()) {
                                try { scalar_aux = integer(*aux, "a"); } catch (...) {}
                            }
                            std::int64_t array_aux = 0;
                            const auto command = entry.find("c");
                            const auto data = entry.find("d");
                            for (std::size_t i = 0; i < xs.size(); ++i) {
                                const BlockPos world{ i32(xs[i], "x"), i32(ys[i], "y"),
                                    i32(zs[i], "z") };
                                auto effective_aux = scalar_aux;
                                if (aux != entry.end() && aux->is_array()) {
                                    if (i < aux->size()) {
                                        try { array_aux = integer((*aux)[i], "a"); }
                                        catch (...) { array_aux = 0; }
                                    }
                                    effective_aux = array_aux;
                                }
                                std::unique_ptr<nbt::tag_compound> entity;
                                if (command != entry.end() && command->is_object()) {
                                    const auto select = [&](std::string_view field) -> nlohmann::json {
                                        const auto found = command->find(std::string(field));
                                        if (found == command->end() || !found->is_array() || found->empty()) return nullptr;
                                        return (*found)[std::min(i, found->size() - 1)];
                                    };
                                    entity = command_nbt(nlohmann::json::array({
                                        select("c"), select("t"), select("a"), select("n") }));
                                }
                                std::unique_ptr<nbt::tag_compound> container;
                                if (data != entry.end() && data->is_array() && i < data->size()) {
                                    const auto& value = (*data)[i];
                                    if (value.is_object()) {
                                        if (const auto encoded = value.find("e"); encoded != value.end() && encoded->is_string()) {
                                            const auto parsed = parse_mianyang_nbt(encoded->get<std::string>());
                                            if (parsed && !parsed.value().empty()) container = decode_compound(parsed.value());
                                        } else if (const auto items = value.find("d"); items != value.end()) {
                                            container = container_nbt_v2(block_name, *items);
                                        }
                                    }
                                }
                                if (container) {
                                    if (!entity) entity = std::move(container);
                                    else {
                                        for (const auto& [key, tag] : *container) {
                                            (*entity)[key] = nbt::value(tag);
                                        }
                                    }
                                }
                                if (entity) {
                                    if (const auto conditional = states.find("conditional_bit");
                                        conditional != states.end()) {
                                        auto value = conditional->second;
                                        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                                            return static_cast<char>(std::tolower(c));
                                        });
                                        entity->emplace<nbt::tag_byte>("conditionalMode", value == "true" ? 1 : 0);
                                    }
                                    if (entity->has_key("CustomName", nbt::tag_type::String) &&
                                        entity->at("CustomName").as<nbt::tag_string>().get() == "") {
                                        entity->erase("CustomName");
                                    }
                                }
                                const auto local = local_position(world, bounds.minimum);
                                mStore.put(local,
                                    runtime_id(block_name, effective_aux, states));
                                if (entity) mStore.put_entity(local, serialize_nbt(*entity));
                                ++materialized_blocks;
                            }
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " +
                                std::to_string(current_entry) + ": " + error.what());
                        }
                };

                const auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                                          nlohmann::json& parsed) -> bool {
                    if (depth == 0) {
                        if (event == nlohmann::json::parse_event_t::object_start) {
                            root_is_object = true;
                            return true;
                        }
                        return false;
                    }
                    if (!root_is_object) return false;
                    if (depth == 1) {
                        if (event == nlohmann::json::parse_event_t::key) {
                            root_key = parsed.get<std::string>();
                            return true;
                        }
                        if (root_key == "FuHongBuild_FinalFormat") {
                            target_seen = true;
                            if (event == nlohmann::json::parse_event_t::array_start) {
                                target_is_array = true;
                                in_target = true;
                                return true;
                            }
                            if (event == nlohmann::json::parse_event_t::array_end) {
                                in_target = false;
                            }
                        }
                        return false;
                    }
                    if (!in_target) return false;
                    if (depth == 2) {
                        if (event == nlohmann::json::parse_event_t::object_start) {
                            in_chunk = true;
                            in_block_list = false;
                            chunk_has_block_list = false;
                            chunk_key.clear();
                            entry_index = 0;
                            return true;
                        }
                        if (event == nlohmann::json::parse_event_t::object_end) {
                            if (!chunk_has_block_list) {
                                throw std::runtime_error("chunk index " +
                                    std::to_string(chunk_index) + ": block list is missing");
                            }
                            in_chunk = false;
                            ++chunk_index;
                            return false;
                        }
                        throw std::runtime_error("chunk index " +
                            std::to_string(chunk_index) + ": block list is missing");
                    }
                    if (!in_chunk) return false;
                    if (depth == 3) {
                        if (event == nlohmann::json::parse_event_t::key) {
                            chunk_key = parsed.get<std::string>();
                            return true;
                        }
                        if (chunk_key == "block") {
                            if (event == nlohmann::json::parse_event_t::array_start) {
                                chunk_has_block_list = true;
                                in_block_list = true;
                                return true;
                            }
                            if (event == nlohmann::json::parse_event_t::array_end) {
                                in_block_list = false;
                                return false;
                            }
                            throw std::runtime_error("chunk index " +
                                std::to_string(chunk_index) + ": block list is missing");
                        }
                        return false;
                    }
                    if (in_block_list && depth == 4) {
                        if (event == nlohmann::json::parse_event_t::object_start ||
                            event == nlohmann::json::parse_event_t::array_start) {
                            return true;
                        }
                        if (event == nlohmann::json::parse_event_t::object_end ||
                            event == nlohmann::json::parse_event_t::array_end ||
                            event == nlohmann::json::parse_event_t::value) {
                            consume_entry(parsed);
                            return false;
                        }
                    }
                    return true;
                };

                parse_json_pass(path, mVersion, callback);
                if (!root_is_object || !target_seen || !target_is_array ||
                    chunk_index == 0) {
                    throw std::runtime_error(
                        "FuHongBuild_FinalFormat is not a non-empty array");
                }
            };

            parse_v2(false);
            if (!bounds.populated) throw std::runtime_error("structure has no valid blocks");
            mStore.set_size(bounds.size(name()));
            parse_v2(true);
        } else {
            // Palette, per-chunk origins and global bounds are collected in a
            // detached-entry pass.  A fresh pass then materializes blocks.  V5
            // simply rebuilds its bounded inflate/decrypt pipeline; no spool or
            // whole decrypted payload is needed even when root fields reorder.
            std::vector<std::string> palette;
            std::vector<BlockPos> chunk_origins;
            bool root_is_object = false;
            bool palette_seen = false;
            bool palette_is_array = false;
            bool build_seen = false;
            bool build_is_array = false;
            bool in_palette = false;
            bool in_build = false;
            bool in_chunk = false;
            bool in_block_list = false;
            bool chunk_has_block_list = false;
            bool has_start_x = false;
            bool has_start_z = false;
            std::int32_t start_x = 0;
            std::int32_t start_z = 0;
            Bounds relative_bounds;
            std::size_t minimum_x_entry = 0;
            std::size_t maximum_x_entry = 0;
            std::size_t minimum_z_entry = 0;
            std::size_t maximum_z_entry = 0;
            std::string root_key;
            std::string chunk_key;
            std::size_t chunk_index = 0;
            std::size_t entry_index = 0;

            const auto add_relative = [&](BlockPos position, std::size_t source_entry) {
                if (!relative_bounds.populated) {
                    relative_bounds.add(position);
                    minimum_x_entry = maximum_x_entry = source_entry;
                    minimum_z_entry = maximum_z_entry = source_entry;
                    return;
                }
                if (position.x < relative_bounds.minimum.x) minimum_x_entry = source_entry;
                if (position.x > relative_bounds.maximum.x) maximum_x_entry = source_entry;
                if (position.z < relative_bounds.minimum.z) minimum_z_entry = source_entry;
                if (position.z > relative_bounds.maximum.z) maximum_z_entry = source_entry;
                relative_bounds.add(position);
            };

            const auto consume_bounds_tuple = [&](const nlohmann::json& tuple) {
                const auto current_entry = entry_index++;
                try {
                    if (!tuple.is_array() || tuple.size() < 5) {
                        throw std::runtime_error("tuple is invalid");
                    }
                    if (tuple[0].is_string()) return;
                    (void)integer(tuple[0], "palette index");
                    (void)integer(tuple[1], "aux");
                    const auto& xs = tuple[2];
                    const auto& ys = tuple[3];
                    const auto& zs = tuple[4];
                    if (!xs.is_array()) throw std::runtime_error("xs is not an array");
                    if (!ys.is_array()) throw std::runtime_error("ys is not an array");
                    if (!zs.is_array()) throw std::runtime_error("zs is not an array");
                    if (xs.size() != ys.size() || xs.size() != zs.size()) {
                        throw std::runtime_error("coordinate array lengths differ");
                    }
                    for (std::size_t i = 0; i < xs.size(); ++i) {
                        add_relative({ i32(xs[i], "xs"), i32(ys[i], "ys"),
                            i32(zs[i], "zs") }, current_entry);
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error("entry index " +
                        std::to_string(current_entry) + ": " + error.what());
                }
            };

            const auto finish_chunk = [&] {
                try {
                    if (!chunk_has_block_list) {
                        throw std::runtime_error("block list is missing");
                    }
                    if (mVersion == StructureId::FuHongV3 && !has_start_x) {
                        throw std::runtime_error("startX is missing");
                    }
                    if (mVersion == StructureId::FuHongV3 && !has_start_z) {
                        throw std::runtime_error("startZ is missing");
                    }
                    const BlockPos origin{
                        mVersion == StructureId::FuHongV3 ? start_x : 0,
                        0,
                        mVersion == StructureId::FuHongV3 ? start_z : 0
                    };
                    chunk_origins.push_back(origin);
                    if (relative_bounds.populated) {
                        BlockPos minimum{};
                        BlockPos maximum{};
                        try {
                            minimum.x = checked_add(origin.x,
                                relative_bounds.minimum.x, "x");
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " +
                                std::to_string(minimum_x_entry) + ": " + error.what());
                        }
                        try {
                            maximum.x = checked_add(origin.x,
                                relative_bounds.maximum.x, "x");
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " +
                                std::to_string(maximum_x_entry) + ": " + error.what());
                        }
                        try {
                            minimum.z = checked_add(origin.z,
                                relative_bounds.minimum.z, "z");
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " +
                                std::to_string(minimum_z_entry) + ": " + error.what());
                        }
                        try {
                            maximum.z = checked_add(origin.z,
                                relative_bounds.maximum.z, "z");
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " +
                                std::to_string(maximum_z_entry) + ": " + error.what());
                        }
                        minimum.y = relative_bounds.minimum.y;
                        maximum.y = relative_bounds.maximum.y;
                        bounds.add(minimum);
                        bounds.add(maximum);
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error("chunk index " +
                        std::to_string(chunk_index) + ": " + error.what());
                }
                ++chunk_index;
            };

            const auto metadata_callback = [&](int depth,
                                               nlohmann::json::parse_event_t event,
                                               nlohmann::json& parsed) -> bool {
                if (depth == 0) {
                    if (event == nlohmann::json::parse_event_t::object_start) {
                        root_is_object = true;
                        return true;
                    }
                    return false;
                }
                if (!root_is_object) return false;
                if (depth == 1) {
                    if (event == nlohmann::json::parse_event_t::key) {
                        root_key = parsed.get<std::string>();
                        return true;
                    }
                    if (root_key == "BlocksList") {
                        palette_seen = true;
                        if (event == nlohmann::json::parse_event_t::array_start) {
                            palette_is_array = true;
                            in_palette = true;
                            return true;
                        }
                        if (event == nlohmann::json::parse_event_t::array_end) {
                            in_palette = false;
                        }
                        return false;
                    }
                    if (root_key == "FuHongBuild") {
                        build_seen = true;
                        if (event == nlohmann::json::parse_event_t::array_start) {
                            build_is_array = true;
                            in_build = true;
                            return true;
                        }
                        if (event == nlohmann::json::parse_event_t::array_end) {
                            in_build = false;
                        }
                        return false;
                    }
                    return false;
                }
                if (in_palette && depth == 2) {
                    if (event == nlohmann::json::parse_event_t::value) {
                        if (!parsed.is_string()) {
                            throw std::runtime_error("BlocksList entry " +
                                std::to_string(palette.size()) + " is not a string");
                        }
                        palette.push_back(parsed.get<std::string>());
                        return false;
                    }
                    if (event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start) {
                        throw std::runtime_error("BlocksList entry " +
                            std::to_string(palette.size()) + " is not a string");
                    }
                }
                if (!in_build) return false;
                if (depth == 2) {
                    if (event == nlohmann::json::parse_event_t::object_start) {
                        in_chunk = true;
                        in_block_list = false;
                        chunk_has_block_list = false;
                        has_start_x = false;
                        has_start_z = false;
                        start_x = 0;
                        start_z = 0;
                        relative_bounds = {};
                        chunk_key.clear();
                        entry_index = 0;
                        return true;
                    }
                    if (event == nlohmann::json::parse_event_t::object_end) {
                        finish_chunk();
                        in_chunk = false;
                        return false;
                    }
                    throw std::runtime_error("chunk index " +
                        std::to_string(chunk_index) + ": block list is missing");
                }
                if (!in_chunk) return false;
                if (depth == 3) {
                    if (event == nlohmann::json::parse_event_t::key) {
                        chunk_key = parsed.get<std::string>();
                        return true;
                    }
                    if (chunk_key == "block") {
                        if (event == nlohmann::json::parse_event_t::array_start) {
                            chunk_has_block_list = true;
                            in_block_list = true;
                            return true;
                        }
                        if (event == nlohmann::json::parse_event_t::array_end) {
                            in_block_list = false;
                            return false;
                        }
                        throw std::runtime_error("chunk index " +
                            std::to_string(chunk_index) + ": block list is missing");
                    }
                    if (mVersion == StructureId::FuHongV3 &&
                        (chunk_key == "startX" || chunk_key == "startZ")) {
                        if (event != nlohmann::json::parse_event_t::value) {
                            throw std::runtime_error(chunk_key + " is not an integer");
                        }
                        if (chunk_key == "startX") {
                            start_x = i32(parsed, "startX");
                            has_start_x = true;
                        } else {
                            start_z = i32(parsed, "startZ");
                            has_start_z = true;
                        }
                    }
                    return false;
                }
                if (in_block_list && depth == 4) {
                    if (event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start) {
                        return true;
                    }
                    if (event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end ||
                        event == nlohmann::json::parse_event_t::value) {
                        consume_bounds_tuple(parsed);
                        return false;
                    }
                }
                return true;
            };

            parse_json_pass(path, mVersion, metadata_callback);
            if (!root_is_object || !palette_seen || !palette_is_array || palette.empty()) {
                throw std::runtime_error("BlocksList is not a non-empty array");
            }
            if (!build_seen || !build_is_array) {
                throw std::runtime_error("FuHongBuild is not an array");
            }
            if (!bounds.populated) throw std::runtime_error("structure has no valid blocks");
            mStore.set_size(bounds.size(name()));

            root_is_object = false;
            root_key.clear();
            chunk_key.clear();
            in_build = false;
            in_chunk = false;
            in_block_list = false;
            chunk_has_block_list = false;
            chunk_index = 0;
            entry_index = 0;
            std::unordered_set<BlockPos, BlockPosHash> entity_positions;

            const auto consume_tuple = [&](const nlohmann::json& tuple) {
                const auto current_entry = entry_index++;
                        try {
                            if (!tuple.is_array() || tuple.size() < 5) throw std::runtime_error("tuple is invalid");
                            if (tuple[0].is_string()) return;
                            const auto palette_index = integer(tuple[0], "palette index");
                            if (palette_index < 0 || static_cast<std::size_t>(palette_index) >= palette.size()) {
                                throw std::runtime_error("palette index " + std::to_string(palette_index) + " is out of range");
                            }
                            const auto aux = integer(tuple[1], "aux");
                            const auto& xs = tuple[2];
                            const auto& ys = tuple[3];
                            const auto& zs = tuple[4];
                            if (!xs.is_array()) throw std::runtime_error("xs is not an array");
                            if (!ys.is_array()) throw std::runtime_error("ys is not an array");
                            if (!zs.is_array()) throw std::runtime_error("zs is not an array");
                            if (xs.size() != ys.size() || xs.size() != zs.size()) {
                                throw std::runtime_error("coordinate array lengths differ");
                            }
                            const auto& block_name = palette[static_cast<std::size_t>(palette_index)];
                            const auto runtime = runtime_id(block_name, aux);
                            const auto origin = chunk_origins.at(chunk_index);
                            for (std::size_t i = 0; i < xs.size(); ++i) {
                                const BlockPos world{
                                    checked_add(origin.x, i32(xs[i], "xs"), "x"),
                                    i32(ys[i], "ys"),
                                    checked_add(origin.z, i32(zs[i], "zs"), "z")
                                };
                                std::optional<NbtPayload> entity;
                                if (tuple.size() >= 6 && tuple[5].is_array() && i < tuple[5].size()) {
                                    if (auto compound = extra_nbt(block_name, tuple[5][i])) {
                                        entity = serialize_nbt(*compound);
                                    }
                                }
                                const auto local = local_position(world, bounds.minimum);
                                mStore.put(local, runtime);
                                if (entity && entity_positions.insert(local).second) {
                                    mStore.put_entity(local, std::move(*entity));
                                }
                                ++materialized_blocks;
                            }
                        } catch (const std::exception& error) {
                            throw std::runtime_error("entry index " +
                                std::to_string(current_entry) + ": " + error.what());
                        }
            };

            const auto payload_callback = [&](int depth,
                                              nlohmann::json::parse_event_t event,
                                              nlohmann::json& parsed) -> bool {
                if (depth == 0) {
                    if (event == nlohmann::json::parse_event_t::object_start) {
                        root_is_object = true;
                        return true;
                    }
                    return false;
                }
                if (!root_is_object) return false;
                if (depth == 1) {
                    if (event == nlohmann::json::parse_event_t::key) {
                        root_key = parsed.get<std::string>();
                        return true;
                    }
                    if (root_key == "FuHongBuild" &&
                        event == nlohmann::json::parse_event_t::array_start) {
                        in_build = true;
                        return true;
                    }
                    if (root_key == "FuHongBuild" &&
                        event == nlohmann::json::parse_event_t::array_end) {
                        in_build = false;
                    }
                    return false;
                }
                if (!in_build) return false;
                if (depth == 2) {
                    if (event == nlohmann::json::parse_event_t::object_start) {
                        if (chunk_index >= chunk_origins.size()) {
                            throw std::runtime_error("FuHongBuild changed between parse passes");
                        }
                        in_chunk = true;
                        in_block_list = false;
                        chunk_has_block_list = false;
                        chunk_key.clear();
                        entry_index = 0;
                        return true;
                    }
                    if (event == nlohmann::json::parse_event_t::object_end) {
                        if (!chunk_has_block_list) {
                            throw std::runtime_error("chunk index " +
                                std::to_string(chunk_index) + ": block list is missing");
                        }
                        ++chunk_index;
                        in_chunk = false;
                        return false;
                    }
                    throw std::runtime_error("chunk index " +
                        std::to_string(chunk_index) + ": block list is missing");
                }
                if (!in_chunk) return false;
                if (depth == 3) {
                    if (event == nlohmann::json::parse_event_t::key) {
                        chunk_key = parsed.get<std::string>();
                        return true;
                    }
                    if (chunk_key == "block") {
                        if (event == nlohmann::json::parse_event_t::array_start) {
                            chunk_has_block_list = true;
                            in_block_list = true;
                            return true;
                        }
                        if (event == nlohmann::json::parse_event_t::array_end) {
                            in_block_list = false;
                            return false;
                        }
                        throw std::runtime_error("chunk index " +
                            std::to_string(chunk_index) + ": block list is missing");
                    }
                    return false;
                }
                if (in_block_list && depth == 4) {
                    if (event == nlohmann::json::parse_event_t::object_start ||
                        event == nlohmann::json::parse_event_t::array_start) {
                        return true;
                    }
                    if (event == nlohmann::json::parse_event_t::object_end ||
                        event == nlohmann::json::parse_event_t::array_end ||
                        event == nlohmann::json::parse_event_t::value) {
                        consume_tuple(parsed);
                        return false;
                    }
                }
                return true;
            };

            parse_json_pass(path, mVersion, payload_callback);
            if (chunk_index != chunk_origins.size()) {
                throw std::runtime_error("FuHongBuild changed between parse passes");
            }
        }

        if (materialized_blocks == 0 || mPaletteCache.empty()) {
            throw std::runtime_error("structure has no valid blocks");
        }
        mNonAirBlocks = mStore.count_non_air();
        return Result<void>::success();
    } catch (const std::exception& error) {
        mStore.clear();
        mPaletteCache.clear();
        mNonAirBlocks = 0;
        return Result<void>::failure("parse " + std::string(name()) + " failed: " + error.what());
    }
}

Result<void> FuHongStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> FuHongStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(
        std::string(name()) + " file writer is not implemented yet");
}

} // namespace water_structure
