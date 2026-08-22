#include "gangban.hpp"

#include "nbt_text.hpp"

#include <WaterStructure/world.hpp>

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
#include <set>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <string_view>
#include <functional>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::size_t kMaxDecodedBytes = 2ull * 1024 * 1024 * 1024;

struct PendingBlock {
    BlockPos world{};
    std::uint32_t runtime_id = 0;
    std::optional<NbtPayload> nbt;
};

struct Bounds {
    bool populated = false;
    BlockPos minimum{};
    BlockPos maximum{};

    void add(BlockPos pos)
    {
        if (!populated) {
            minimum = maximum = pos;
            populated = true;
            return;
        }
        minimum.x = std::min(minimum.x, pos.x);
        minimum.y = std::min(minimum.y, pos.y);
        minimum.z = std::min(minimum.z, pos.z);
        maximum.x = std::max(maximum.x, pos.x);
        maximum.y = std::max(maximum.y, pos.y);
        maximum.z = std::max(maximum.z, pos.z);
    }

    Size size() const
    {
        if (!populated) throw std::runtime_error("structure has no blocks");
        const auto width = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const auto height = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const auto length = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (width <= 0 || height <= 0 || length <= 0 ||
            width > std::numeric_limits<std::int32_t>::max() ||
            height > std::numeric_limits<std::int32_t>::max() ||
            length > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("structure bounds exceed int32 dimensions");
        }
        return {
            static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height),
            static_cast<std::int32_t>(length)
        };
    }
};

using Json = nlohmann::json;

// V7 used to inflate the complete JSON payload into a vector before parsing it.
// This stream buffer keeps both compressed and decoded working sets fixed at
// 64 KiB while retaining the old compressed-offset diagnostics and 2 GiB
// decoded-stream limit.
class ZlibInflateBuffer final : public std::streambuf {
public:
    explicit ZlibInflateBuffer(const std::filesystem::path& path)
        : mInput(path, std::ios::binary)
    {
        if (!mInput) {
            throw std::runtime_error("cannot open compressed GangBan file: " + path.string());
        }
        if (inflateInit(&mStream) != Z_OK) {
            throw std::runtime_error("cannot initialize zlib decoder");
        }
        mInitialized = true;
        setg(mDecoded.data(), mDecoded.data(), mDecoded.data());
    }

    ~ZlibInflateBuffer() override
    {
        if (mInitialized) inflateEnd(&mStream);
    }

    const std::string& error() const noexcept { return mError; }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        if (mFinished || !mError.empty()) return traits_type::eof();

        while (true) {
            if (mStream.avail_in == 0) {
                mInput.read(reinterpret_cast<char*>(mCompressed.data()),
                    static_cast<std::streamsize>(mCompressed.size()));
                const auto count = mInput.gcount();
                if (count <= 0) {
                    mError = "zlib stream truncated at compressed offset " +
                        std::to_string(mStream.total_in);
                    return traits_type::eof();
                }
                mStream.next_in = mCompressed.data();
                mStream.avail_in = static_cast<uInt>(count);
            }

            mStream.next_out = reinterpret_cast<Bytef*>(mDecoded.data());
            mStream.avail_out = static_cast<uInt>(mDecoded.size());
            const auto status = inflate(&mStream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                mError = "zlib decode failed at compressed offset " +
                    std::to_string(mStream.total_in);
                return traits_type::eof();
            }
            const auto produced = mDecoded.size() - mStream.avail_out;
            if (mDecodedBytes > kMaxDecodedBytes - produced) {
                mError = "decoded GangBan payload exceeds 2 GiB";
                return traits_type::eof();
            }
            mDecodedBytes += produced;
            if (status == Z_STREAM_END) mFinished = true;
            if (produced != 0) {
                setg(mDecoded.data(), mDecoded.data(),
                    mDecoded.data() + static_cast<std::ptrdiff_t>(produced));
                return traits_type::to_int_type(*gptr());
            }
            if (mFinished) return traits_type::eof();
        }
    }

private:
    std::ifstream mInput;
    z_stream mStream{};
    bool mInitialized = false;
    bool mFinished = false;
    std::size_t mDecodedBytes = 0;
    std::string mError;
    std::array<Bytef, kStreamChunk> mCompressed{};
    std::array<char, kStreamChunk> mDecoded{};
};

class ZlibInflateInput final {
public:
    explicit ZlibInflateInput(const std::filesystem::path& path)
        : mBuffer(path), mStream(&mBuffer) {}

    std::istream& stream() noexcept { return mStream; }

    void finish()
    {
        // A SAX consumer may reject JSON before reaching the compressed tail.
        // Draining preserves the former inflate-before-parse error precedence.
        std::array<char, 4096> scratch{};
        while (mStream.read(scratch.data(), static_cast<std::streamsize>(scratch.size())) ||
            mStream.gcount() != 0) {
        }
        if (!mBuffer.error().empty()) throw std::runtime_error(mBuffer.error());
    }

private:
    ZlibInflateBuffer mBuffer;
    std::istream mStream;
};

// Builds at most one direct child of the root array. This is deliberately not
// a whole-document DOM: completed entries are moved to the caller immediately.
class RootArrayEntrySax final : public nlohmann::json_sax<Json> {
public:
    using Callback = std::function<void(std::size_t, Json&&)>;

    explicit RootArrayEntrySax(Callback callback) : mCallback(std::move(callback)) {}

    bool null() override { return scalar(nullptr); }
    bool boolean(bool value) override { return scalar(value); }
    bool number_integer(number_integer_t value) override { return scalar(value); }
    bool number_unsigned(number_unsigned_t value) override { return scalar(value); }
    bool number_float(number_float_t value, const string_t&) override { return scalar(value); }
    bool string(string_t& value) override { return scalar(std::move(value)); }
    bool binary(binary_t& value) override { return scalar(Json::binary(std::move(value))); }

    bool start_object(std::size_t) override
    {
        if (!mRootStarted) return fail("root is not an array");
        if (mRootEnded) return fail("JSON value follows the root array");
        mFrames.push_back({ Json::object(), {}, false });
        return true;
    }

    bool key(string_t& value) override
    {
        if (mFrames.empty() || !mFrames.back().value.is_object()) {
            return fail("object key appears outside an object");
        }
        mFrames.back().key = std::move(value);
        mFrames.back().has_key = true;
        return true;
    }

    bool end_object() override
    {
        if (mFrames.empty() || !mFrames.back().value.is_object()) {
            return fail("mismatched object terminator");
        }
        return complete_container();
    }

    bool start_array(std::size_t) override
    {
        if (!mRootStarted) {
            mRootStarted = true;
            return true;
        }
        if (mRootEnded) return fail("JSON value follows the root array");
        mFrames.push_back({ Json::array(), {}, false });
        return true;
    }

    bool end_array() override
    {
        if (mFrames.empty()) {
            if (!mRootStarted || mRootEnded) return fail("mismatched array terminator");
            mRootEnded = true;
            return true;
        }
        if (!mFrames.back().value.is_array()) return fail("mismatched array terminator");
        return complete_container();
    }

    bool parse_error(
        std::size_t position, const std::string&, const nlohmann::detail::exception& error) override
    {
        mError = "JSON parse error at byte " + std::to_string(position) + ": " + error.what();
        return false;
    }

    const std::string& error() const noexcept { return mError; }

    void finish() const
    {
        if (!mError.empty()) throw std::runtime_error(mError);
        if (!mRootStarted || !mRootEnded || !mFrames.empty()) {
            throw std::runtime_error("root array is incomplete");
        }
    }

private:
    struct Frame {
        Json value;
        std::string key;
        bool has_key = false;
    };

    template <typename Value>
    bool scalar(Value&& value)
    {
        if (!mRootStarted) return fail("root is not an array");
        if (mRootEnded) return fail("JSON value follows the root array");
        return append(Json(std::forward<Value>(value)));
    }

    bool complete_container()
    {
        auto value = std::move(mFrames.back().value);
        mFrames.pop_back();
        return append(std::move(value));
    }

    bool append(Json value)
    {
        if (mFrames.empty()) {
            mCallback(mIndex++, std::move(value));
            return true;
        }
        auto& parent = mFrames.back();
        if (parent.value.is_array()) {
            parent.value.push_back(std::move(value));
            return true;
        }
        if (!parent.value.is_object() || !parent.has_key) {
            return fail("object value is missing its key");
        }
        parent.value[std::move(parent.key)] = std::move(value);
        parent.key.clear();
        parent.has_key = false;
        return true;
    }

    bool fail(std::string message)
    {
        if (mError.empty()) mError = std::move(message);
        return false;
    }

    Callback mCallback;
    std::vector<Frame> mFrames;
    std::size_t mIndex = 0;
    bool mRootStarted = false;
    bool mRootEnded = false;
    std::string mError;
};

template <typename Callback>
void for_each_root_entry(
    const std::filesystem::path& path, bool compressed, Callback&& callback)
{
    auto parse = [&](std::istream& input) {
        RootArrayEntrySax sax(std::forward<Callback>(callback));
        const auto parsed = Json::sax_parse(input, &sax);
        if (!parsed) {
            throw std::runtime_error(
                sax.error().empty() ? "cannot parse GangBan root array" : sax.error());
        }
        sax.finish();
    };

    if (compressed) {
        ZlibInflateInput input(path);
        try {
            parse(input.stream());
        } catch (...) {
            input.finish();
            throw;
        }
        input.finish();
        return;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open GangBan file: " + path.string());
    parse(input);
}

std::int64_t json_integer(const nlohmann::json& value, std::string_view field)
{
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error(std::string(field) + " exceeds int64");
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number) ||
            number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error(std::string(field) + " is outside int64");
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        std::int64_t number = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return number;
    }
    throw std::runtime_error(std::string(field) + " is not an integer");
}

std::int32_t json_i32(const nlohmann::json& value, std::string_view field)
{
    const auto number = json_integer(value, field);
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds int32");
    }
    return static_cast<std::int32_t>(number);
}

std::uint16_t json_u16(const nlohmann::json& value, std::string_view field)
{
    const auto number = json_integer(value, field);
    if (number < 0 || number > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds uint16");
    }
    return static_cast<std::uint16_t>(number);
}

std::vector<std::string> string_palette(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_array() || value.empty()) {
        throw std::runtime_error(std::string(field) + " is not a non-empty array");
    }
    std::vector<std::string> result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!value[index].is_string()) {
            throw std::runtime_error(std::string(field) + " entry " + std::to_string(index) + " is not a string");
        }
        result.push_back(value[index].get<std::string>());
    }
    return result;
}

BlockPos position3(const nlohmann::json& value, std::string_view field)
{
    if (!value.is_array() || value.size() != 3) {
        throw std::runtime_error(std::string(field) + " must contain three coordinates");
    }
    return {
        json_i32(value[0], std::string(field) + ".x"),
        json_i32(value[1], std::string(field) + ".y"),
        json_i32(value[2], std::string(field) + ".z")
    };
}

BlockPos subtract(BlockPos value, BlockPos origin)
{
    const auto x = static_cast<std::int64_t>(value.x) - origin.x;
    const auto y = static_cast<std::int64_t>(value.y) - origin.y;
    const auto z = static_cast<std::int64_t>(value.z) - origin.z;
    if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
        y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max() ||
        z < std::numeric_limits<std::int32_t>::min() || z > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("local block coordinate exceeds int32");
    }
    return { static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z) };
}

NbtPayload serialize_nbt(const nbt::tag_compound& root)
{
    std::ostringstream output(std::ios::binary);
    nbt::io::write_tag("", root, output, endian::little);
    const auto bytes = output.str();
    return { bytes.begin(), bytes.end() };
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return character >= 'A' && character <= 'Z'
            ? static_cast<char>(character - 'A' + 'a')
            : static_cast<char>(character);
    });
    return value;
}

std::optional<std::int32_t> command_mode(std::string value)
{
    value = lower_ascii(std::move(value));
    if (value == "tick" || value == "impulse" || value == "normal") return 0;
    if (value == "repeating" || value == "repeat") return 1;
    if (value == "chain") return 2;
    return std::nullopt;
}

NbtPayload command_nbt_v1(const nlohmann::json& value)
{
    if (!value.is_object()) throw std::runtime_error("cmds is not an object");
    const auto automatic = value.value("auto", false);
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("CommandBlock");
    root["Command"] = nbt::tag_string(value.value("cmd", std::string{}));
    root["CustomName"] = nbt::tag_string(value.value("name", std::string{}));
    root["LastOutput"] = nbt::tag_string(value.value("last", std::string{}));
    root.emplace<nbt::tag_int>("TickDelay", value.value("tick", 0));
    root.emplace<nbt::tag_byte>("ExecuteOnFirstTick", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("TrackOutput", value.value("should", false) ? 1 : 0);
    root.emplace<nbt::tag_byte>("conditionalMode", value.value("condition", false) ? 1 : 0);
    root.emplace<nbt::tag_byte>("auto", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("Powered", value.value("on", false) ? 1 : 0);
    if (const auto mode = command_mode(value.value("mode", std::string{}))) {
        root.emplace<nbt::tag_int>("LPCommandMode", *mode);
    }
    return serialize_nbt(root);
}

std::string command_block_name(std::int32_t variant)
{
    switch (variant) {
    case 0: return "minecraft:command_block";
    case 1: return "minecraft:repeating_command_block";
    case 2: return "minecraft:chain_command_block";
    default: return "minecraft:command_block";
    }
}

NbtPayload command_nbt_v5(const nlohmann::json& value, std::int32_t variant)
{
    nbt::tag_compound root;
    root["id"] = nbt::tag_string("CommandBlock");
    root["Command"] = nbt::tag_string(
        value.contains("cmd") && value["cmd"].is_string() ? value["cmd"].get<std::string>() : std::string{});
    if (value.contains("name") && value["name"].is_string()) {
        root["CustomName"] = nbt::tag_string(value["name"].get<std::string>());
    }
    if (value.contains("delay")) {
        try {
            root.emplace<nbt::tag_int>("TickDelay", json_i32(value["delay"], "command delay"));
        } catch (...) {
        }
    }
    const auto automatic = value.contains("auto") && value["auto"].is_boolean() && value["auto"].get<bool>();
    const auto conditional = value.contains("condition") &&
        value["condition"].is_boolean() && value["condition"].get<bool>();
    root.emplace<nbt::tag_byte>("ExecuteOnFirstTick", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("TrackOutput", 0);
    root.emplace<nbt::tag_byte>("conditionalMode", conditional ? 1 : 0);
    root.emplace<nbt::tag_byte>("auto", automatic ? 1 : 0);
    root.emplace<nbt::tag_byte>("Powered", 0);
    root.emplace<nbt::tag_int>("LPCommandMode", variant);
    root["LastOutput"] = nbt::tag_string("");
    return serialize_nbt(root);
}

std::string container_id(std::string_view name)
{
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

std::optional<NbtPayload> container_nbt(std::string_view block_name, const nlohmann::json& payload)
{
    if (!payload.is_array()) return std::nullopt;
    nbt::tag_list items(nbt::tag_type::Compound);
    for (const auto& raw : payload) {
        if (!raw.is_object()) continue;
        const auto integer_or_zero = [&](std::string_view key) -> std::int64_t {
            const auto found = raw.find(std::string(key));
            if (found == raw.end()) return 0;
            try { return json_integer(*found, key); } catch (...) { return 0; }
        };
        nbt::tag_compound item;
        item["Slot"] = nbt::tag_byte(static_cast<std::int8_t>(integer_or_zero("slot")));
        item["Name"] = nbt::tag_string(
            raw.contains("ns") && raw["ns"].is_string() ? raw["ns"].get<std::string>() : std::string{});
        item["Count"] = nbt::tag_byte(static_cast<std::int8_t>(integer_or_zero("num")));
        item["Damage"] = nbt::tag_short(static_cast<std::int16_t>(integer_or_zero("aux")));
        items.push_back(std::move(item));
    }

    const auto id = container_id(block_name);
    if (id.empty() && items.size() == 0) return std::nullopt;
    nbt::tag_compound root;
    if (!id.empty()) root["id"] = nbt::tag_string(id);
    if (items.size() != 0) root["Items"] = std::move(items);
    return serialize_nbt(root);
}

std::vector<std::string> parse_v3_palette(std::string_view encoded)
{
    std::map<std::int32_t, std::string> indexed;
    std::int32_t maximum = 0;
    std::size_t position = 0;
    while (position < encoded.size()) {
        const auto open = encoded.find('[', position);
        if (open == std::string_view::npos) break;
        const auto close = encoded.find(']', open + 1);
        const auto next_open = close == std::string_view::npos
            ? std::string_view::npos : encoded.find('[', close + 1);
        const auto next_close = next_open == std::string_view::npos
            ? std::string_view::npos : encoded.find(']', next_open + 1);
        if (close == std::string_view::npos || next_open == std::string_view::npos ||
            next_close == std::string_view::npos) break;
        std::int32_t first = 0, second = 0;
        const auto first_text = encoded.substr(open + 1, close - open - 1);
        const auto second_text = encoded.substr(next_open + 1, next_close - next_open - 1);
        const auto first_result = std::from_chars(first_text.data(), first_text.data() + first_text.size(), first);
        const auto second_result = std::from_chars(second_text.data(), second_text.data() + second_text.size(), second);
        if (first_result.ec == std::errc{} && first_result.ptr == first_text.data() + first_text.size() &&
            second_result.ec == std::errc{} && second_result.ptr == second_text.data() + second_text.size() &&
            first == second) {
            indexed[first] = std::string(encoded.substr(close + 1, next_open - close - 1));
            maximum = std::max(maximum, first);
        }
        position = next_close + 1;
    }
    if (indexed.empty()) throw std::runtime_error("V3 palette string has no entries");
    std::vector<std::string> result(static_cast<std::size_t>(maximum) + 1);
    for (std::int32_t index = 0; index <= maximum; ++index) {
        const auto found = indexed.find(index);
        if (found == indexed.end()) throw std::runtime_error("V3 palette is missing index " + std::to_string(index));
        result[static_cast<std::size_t>(index)] = found->second;
    }
    return result;
}

std::optional<NbtPayload> snbt_payload(const nlohmann::json& entry)
{
    if (!entry.is_array() || entry.size() < 7 || !entry[5].is_string() ||
        entry[5].get<std::string>() != "nbt" || !entry[6].is_string()) return std::nullopt;
    auto parsed = parse_mianyang_nbt(entry[6].get<std::string>());
    if (!parsed || parsed.value().empty()) return std::nullopt;
    return std::move(parsed).value();
}

void validate_palette_index(std::int32_t index, std::size_t size, std::size_t block_index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= size) {
        throw std::runtime_error(
            "block index " + std::to_string(block_index) + " palette index " +
            std::to_string(index) + " is out of range");
    }
}

} // namespace

std::string_view GangBanStructure::name() const noexcept
{
    switch (mVersion) {
    case StructureId::GangBanV1: return "GangBanV1";
    case StructureId::GangBanV2: return "GangBanV2";
    case StructureId::GangBanV3: return "GangBanV3";
    case StructureId::GangBanV4: return "GangBanV4";
    case StructureId::GangBanV5: return "GangBanV5";
    case StructureId::GangBanV6: return "GangBanV6";
    case StructureId::GangBanV7: return "GangBanV7";
    default: return "GangBan";
    }
}

std::uint32_t GangBanStructure::runtime_id(std::string_view name, std::uint16_t data)
{
    if (const auto runtime = mRegistry.legacy_runtime_id(name, data)) return *runtime;
    if (const auto unknown = mRegistry.find("minecraft:unknown")) return *unknown;
    return mRegistry.register_state({ "minecraft:unknown", {}, 0 });
}

Result<void> GangBanStructure::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    try {
        const auto compressed = mVersion == StructureId::GangBanV7;

        if (mVersion == StructureId::GangBanV1 || mVersion == StructureId::GangBanV2) {
            std::size_t document_size = 0;
            std::optional<Json> penultimate;
            std::optional<Json> last;
            for_each_root_entry(path, compressed, [&](std::size_t index, Json&& entry) {
                document_size = index + 1;
                penultimate = std::move(last);
                last = std::move(entry);
            });

            const auto has_range = mVersion == StructureId::GangBanV1 ||
                (document_size >= 2 && penultimate && penultimate->is_object() &&
                    penultimate->contains("start") && penultimate->contains("end"));
            if (document_size < (has_range ? 2u : 1u) || !last) {
                throw std::runtime_error("root array is too short");
            }
            const auto& palette_object = *last;
            if (!palette_object.is_object() || !palette_object.contains("list")) {
                throw std::runtime_error("palette object is missing list");
            }
            const auto palette = string_palette(palette_object["list"], "palette.list");
            const auto block_count = document_size - (has_range ? 2 : 1);

            BlockPos origin{};
            Size declared_size{};
            if (has_range) {
                if (!penultimate) throw std::runtime_error("range object is missing start/end");
                const auto& range = *penultimate;
                if (!range.is_object() || !range.contains("start") || !range.contains("end")) {
                    throw std::runtime_error("range object is missing start/end");
                }
                const auto start = position3(range["start"], "range.start");
                const auto end = position3(range["end"], "range.end");
                Bounds declared;
                declared.add(start);
                declared.add(end);
                origin = declared.minimum;
                declared_size = declared.size();
            }

            // Release the JSON form of the tail before the block passes. The
            // string palette is the only tail data required from this point.
            penultimate.reset();
            last.reset();

            auto decode_block = [&](const Json& entry, std::size_t index) -> PendingBlock {
                try {
                    if (!entry.is_object() || !entry.contains("id") || !entry.contains("p")) {
                        throw std::runtime_error("entry is missing id/p");
                    }
                    const auto palette_index = json_i32(entry["id"], "id");
                    validate_palette_index(palette_index, palette.size(), index);
                    const auto aux = entry.contains("aux") ? json_u16(entry["aux"], "aux") : 0;
                    const auto world = position3(entry["p"], "p");
                    PendingBlock block{
                        world,
                        runtime_id(palette[static_cast<std::size_t>(palette_index)], aux),
                        std::nullopt
                    };
                    if (entry.contains("cmds") && !entry["cmds"].is_null()) {
                        block.nbt = command_nbt_v1(entry["cmds"]);
                    }
                    return block;
                } catch (const std::exception& error) {
                    throw std::runtime_error("block index " + std::to_string(index) + ": " + error.what());
                }
            };

            // The validation pass preserves the old behavior of parsing every
            // block before checking whether any block lies outside a declared
            // range. It also infers V2 bounds without retaining PendingBlock[];
            // a final pass materializes directly into the sparse store.
            Bounds inferred;
            for_each_root_entry(path, compressed, [&](std::size_t index, Json&& entry) {
                if (index >= block_count) return;
                const auto block = decode_block(entry, index);
                inferred.add(block.world);
            });

            if (!has_range) {
                origin = inferred.minimum;
                declared_size = inferred.size();
            }
            mStore.set_size(declared_size);
            for_each_root_entry(path, compressed, [&](std::size_t index, Json&& entry) {
                if (index >= block_count) return;
                auto block = decode_block(entry, index);
                const auto local = subtract(block.world, origin);
                if (has_range && (local.x < 0 || local.y < 0 || local.z < 0 ||
                    local.x >= declared_size.width || local.y >= declared_size.height ||
                    local.z >= declared_size.length)) {
                    throw std::runtime_error("block index " + std::to_string(index) + " is outside declared range");
                }
                mStore.put(local, block.runtime_id);
                if (block.nbt) mStore.put_entity(local, std::move(*block.nbt));
            });
            mNonAirBlocks = mStore.count_non_air();
            return Result<void>::success();
        }

        if (mVersion == StructureId::GangBanV3 || mVersion == StructureId::GangBanV4) {
            struct ChunkMetadata {
                std::int32_t x = 0;
                std::int32_t z = 0;
                std::size_t data_occurrences = 0;
            };

            std::optional<Json> header;
            std::optional<Json> raw_palette;
            std::vector<ChunkMetadata> chunk_metadata;

            auto parse_callback_pass = [&](auto&& callback) {
                std::ifstream input(path, std::ios::binary);
                if (!input) throw std::runtime_error("cannot open GangBan file: " + path.string());
                const auto discarded = nlohmann::json::parse(
                    input, std::forward<decltype(callback)>(callback));
                (void)discarded;
            };

            bool root_is_array = false;
            std::size_t root_index = 0;
            bool inside_chunk = false;
            bool inside_data = false;
            std::string chunk_key;
            Json grids_raw;
            bool has_grids = false;
            std::size_t data_occurrences = 0;
            bool final_data_is_array = false;
            const auto metadata_callback = [&](int depth, Json::parse_event_t event,
                                               Json& parsed) -> bool {
                if (depth == 0 && event == Json::parse_event_t::array_start) {
                    root_is_array = true;
                    return true;
                }
                if (depth == 0 && (event == Json::parse_event_t::object_start ||
                        event == Json::parse_event_t::value)) {
                    return false;
                }
                if (depth == 1 && event == Json::parse_event_t::object_start) {
                    if (root_index < 2) return true;
                    inside_chunk = true;
                    inside_data = false;
                    chunk_key.clear();
                    grids_raw = {};
                    has_grids = false;
                    data_occurrences = 0;
                    final_data_is_array = false;
                    return true;
                }
                if (depth == 1 && event == Json::parse_event_t::array_start) {
                    if (root_index < 2) return true;
                    throw std::runtime_error(
                        "chunk index " + std::to_string(root_index - 2) + " is invalid");
                }
                if (depth == 1 && event == Json::parse_event_t::value) {
                    if (root_index == 0) header = parsed;
                    else if (root_index == 1) raw_palette = parsed;
                    else {
                        throw std::runtime_error(
                            "chunk index " + std::to_string(root_index - 2) + " is invalid");
                    }
                    ++root_index;
                    return false;
                }
                if (inside_chunk && depth == 2 && event == Json::parse_event_t::key) {
                    chunk_key = parsed.get<std::string>();
                    return true;
                }
                if (inside_data && depth == 3 &&
                    (event == Json::parse_event_t::value ||
                        event == Json::parse_event_t::object_start ||
                        event == Json::parse_event_t::array_start ||
                        event == Json::parse_event_t::object_end ||
                        event == Json::parse_event_t::array_end)) {
                    return false;
                }
                if (inside_data && depth == 2 && event == Json::parse_event_t::array_end) {
                    inside_data = false;
                    return false;
                }
                if (inside_chunk && depth == 2 &&
                    (event == Json::parse_event_t::value ||
                        event == Json::parse_event_t::object_start ||
                        event == Json::parse_event_t::array_start)) {
                    if (chunk_key == "grids") {
                        has_grids = true;
                        if (event == Json::parse_event_t::object_start) return true;
                        grids_raw = event == Json::parse_event_t::array_start
                            ? Json::array() : parsed;
                        return false;
                    }
                    if (chunk_key == "data") {
                        if (data_occurrences == std::numeric_limits<std::size_t>::max()) {
                            throw std::runtime_error("data occurrence count overflow");
                        }
                        ++data_occurrences;
                        final_data_is_array = event == Json::parse_event_t::array_start;
                        inside_data = final_data_is_array;
                        return inside_data;
                    }
                    return false;
                }
                if (inside_chunk && depth == 2 && event == Json::parse_event_t::object_end &&
                    chunk_key == "grids") {
                    grids_raw = parsed;
                    has_grids = true;
                    return false;
                }
                if (inside_chunk && depth == 2 &&
                    (event == Json::parse_event_t::object_end ||
                        event == Json::parse_event_t::array_end)) {
                    return false;
                }
                if (inside_chunk && depth == 1 && event == Json::parse_event_t::object_end) {
                    const auto chunk_index = root_index - 2;
                    if (!has_grids || !grids_raw.is_object() ||
                        data_occurrences == 0 || !final_data_is_array) {
                        throw std::runtime_error(
                            "chunk index " + std::to_string(chunk_index) + " is invalid");
                    }
                    chunk_metadata.push_back({
                        json_i32(grids_raw.at("x"), "grids.x"),
                        json_i32(grids_raw.at("z"), "grids.z"),
                        data_occurrences
                    });
                    inside_chunk = false;
                    ++root_index;
                    return false;
                }
                if (depth == 1 && (event == Json::parse_event_t::object_end ||
                        event == Json::parse_event_t::array_end)) {
                    if (root_index == 0) header = parsed;
                    else if (root_index == 1) raw_palette = parsed;
                    ++root_index;
                    return false;
                }
                return true;
            };
            parse_callback_pass(metadata_callback);
            if (!root_is_array || root_index < 3 || chunk_metadata.empty() ||
                !header || !header->is_object() || !raw_palette) {
                throw std::runtime_error("header/chunk array is incomplete");
            }

            std::vector<std::string> palette;
            BlockPos declared_origin{};
            Size declared_size{};
            if (mVersion == StructureId::GangBanV3) {
                if (!raw_palette->is_string()) throw std::runtime_error("V3 palette is not a string");
                palette = parse_v3_palette(raw_palette->get<std::string>());
                declared_origin = {
                    json_i32(header->at("x"), "header.x"),
                    json_i32(header->at("y"), "header.y"),
                    json_i32(header->at("z"), "header.z")
                };
                declared_size = {
                    json_i32(header->at("xcha"), "header.xcha"),
                    json_i32(header->at("ycha"), "header.ycha"),
                    json_i32(header->at("zcha"), "header.zcha")
                };
                if (declared_size.width <= 0 || declared_size.height <= 0 || declared_size.length <= 0) {
                    throw std::runtime_error("V3 dimensions must be positive");
                }
            } else {
                palette = string_palette(*raw_palette, "V4 palette");
            }
            header.reset();
            raw_palette.reset();

            Bounds bounds;
            std::set<BlockPos, std::less<>> first_entity_positions;
            auto stream_entries = [&](const bool materialize) -> std::size_t {
                bool pass_root_is_array = false;
                std::size_t pass_root_index = 0;
                std::size_t pass_chunk_index = 0;
                bool pass_inside_chunk = false;
                bool pass_inside_data = false;
                bool active_data = false;
                std::string pass_chunk_key;
                std::size_t pass_data_occurrence = 0;
                std::size_t block_index = 0;

                auto consume_entry = [&](const Json& entry) {
                    if (pass_chunk_index >= chunk_metadata.size()) {
                        throw std::runtime_error("GangBan chunk stream changed while parsing");
                    }
                    try {
                        if (!entry.is_array() || entry.size() < 5) throw std::runtime_error("entry has fewer than five fields");
                        const auto palette_index = json_i32(entry[0], "palette index");
                        validate_palette_index(palette_index, palette.size(), block_index);
                        const auto aux = json_u16(entry[1], "aux");
                        const auto relative_x = json_i32(entry[2], "local x");
                        const auto world_y = json_i32(entry[3], "y");
                        const auto relative_z = json_i32(entry[4], "local z");
                        const auto x64 = static_cast<std::int64_t>(
                            chunk_metadata[pass_chunk_index].x) + relative_x;
                        const auto z64 = static_cast<std::int64_t>(
                            chunk_metadata[pass_chunk_index].z) + relative_z;
                        if (x64 < std::numeric_limits<std::int32_t>::min() || x64 > std::numeric_limits<std::int32_t>::max() ||
                            z64 < std::numeric_limits<std::int32_t>::min() || z64 > std::numeric_limits<std::int32_t>::max()) {
                            throw std::runtime_error("world coordinate exceeds int32");
                        }
                        const BlockPos world{ static_cast<std::int32_t>(x64), world_y, static_cast<std::int32_t>(z64) };
                        if (!materialize) {
                            bounds.add(world);
                            ++block_index;
                            return;
                        }
                        auto current_nbt = snbt_payload(entry);
                        const auto runtime = runtime_id(
                            palette[static_cast<std::size_t>(palette_index)], aux);
                        const auto local = mVersion == StructureId::GangBanV3
                            ? subtract(world, declared_origin)
                            : subtract(world, bounds.minimum);
                        mStore.put(local, runtime);
                        if (current_nbt) {
                            if (mVersion == StructureId::GangBanV4 ||
                                first_entity_positions.insert(local).second) {
                                mStore.put_entity(local, std::move(*current_nbt));
                            }
                        }
                    } catch (const std::exception& error) {
                        throw std::runtime_error("block index " + std::to_string(block_index) + ": " + error.what());
                    }
                    ++block_index;
                };

                const auto callback = [&](int depth, Json::parse_event_t event,
                                          Json& parsed) -> bool {
                    if (depth == 0 && event == Json::parse_event_t::array_start) {
                        pass_root_is_array = true;
                        return true;
                    }
                    if (pass_root_index < 2 && depth == 1 &&
                        (event == Json::parse_event_t::value ||
                            event == Json::parse_event_t::object_end ||
                            event == Json::parse_event_t::array_end)) {
                        ++pass_root_index;
                        return false;
                    }
                    if (pass_root_index < 2 && depth == 1 &&
                        (event == Json::parse_event_t::object_start ||
                            event == Json::parse_event_t::array_start)) {
                        // Keep the root container open so its depth-1 end
                        // event advances past the header/palette entry. If
                        // this is skipped wholesale, nlohmann-json does not
                        // emit that end event and the first chunk is never
                        // visited on the second streaming pass.
                        return true;
                    }
                    if (pass_root_index < 2 && depth > 1) return false;
                    if (depth == 1 && event == Json::parse_event_t::object_start) {
                        if (pass_chunk_index >= chunk_metadata.size()) {
                            throw std::runtime_error("GangBan chunk stream changed while parsing");
                        }
                        pass_inside_chunk = true;
                        pass_inside_data = false;
                        active_data = false;
                        pass_chunk_key.clear();
                        pass_data_occurrence = 0;
                        return true;
                    }
                    if (pass_inside_chunk && depth == 2 && event == Json::parse_event_t::key) {
                        pass_chunk_key = parsed.get<std::string>();
                        return true;
                    }
                    if (pass_inside_chunk && depth == 2 && pass_chunk_key == "data" &&
                        (event == Json::parse_event_t::value ||
                            event == Json::parse_event_t::object_start ||
                            event == Json::parse_event_t::array_start)) {
                        ++pass_data_occurrence;
                        pass_inside_data = event == Json::parse_event_t::array_start;
                        active_data = pass_inside_data && pass_data_occurrence ==
                            chunk_metadata[pass_chunk_index].data_occurrences;
                        return pass_inside_data;
                    }
                    if (pass_inside_data && depth == 3) {
                        if (event == Json::parse_event_t::object_start ||
                            event == Json::parse_event_t::array_start) {
                            return active_data;
                        }
                        if (event == Json::parse_event_t::value ||
                            event == Json::parse_event_t::object_end ||
                            event == Json::parse_event_t::array_end) {
                            if (active_data) consume_entry(parsed);
                            return false;
                        }
                    }
                    if (pass_inside_data && depth == 2 && event == Json::parse_event_t::array_end) {
                        pass_inside_data = false;
                        active_data = false;
                        return false;
                    }
                    if (pass_inside_chunk && depth == 2 &&
                        (event == Json::parse_event_t::value ||
                            event == Json::parse_event_t::object_start ||
                            event == Json::parse_event_t::array_start ||
                            event == Json::parse_event_t::object_end ||
                            event == Json::parse_event_t::array_end)) {
                        return false;
                    }
                    if (pass_inside_chunk && depth == 1 && event == Json::parse_event_t::object_end) {
                        pass_inside_chunk = false;
                        ++pass_chunk_index;
                        ++pass_root_index;
                        return false;
                    }
                    return true;
                };
                parse_callback_pass(callback);
                if (!pass_root_is_array || pass_chunk_index != chunk_metadata.size()) {
                    throw std::runtime_error("GangBan chunk stream changed while parsing");
                }
                return block_index;
            };

            const auto validated_blocks = stream_entries(false);
            if (validated_blocks == 0) throw std::runtime_error("structure has no blocks");
            if (mVersion == StructureId::GangBanV3) {
                mStore.set_size(declared_size);
                // Fatalder keeps V3 records that lie one cell beyond the
                // declared xcha/ycha/zcha bounds. Preserve those records in
                // chunk materialization without changing the reported size.
                mStore.set_include_out_of_bounds(true);
            } else {
                mStore.set_size(bounds.size());
            }
            const auto materialized_blocks = stream_entries(true);
            if (materialized_blocks != validated_blocks) {
                throw std::runtime_error("GangBan chunk stream changed while parsing");
            }
            mNonAirBlocks = mStore.count_non_air();
            return Result<void>::success();
        }

        if (mVersion == StructureId::GangBanV5 || mVersion == StructureId::GangBanV6 ||
            mVersion == StructureId::GangBanV7) {
            std::size_t document_size = 0;
            std::optional<Json> penultimate;
            std::optional<Json> last;
            for_each_root_entry(path, compressed, [&](std::size_t index, Json&& entry) {
                document_size = index + 1;
                penultimate = std::move(last);
                last = std::move(entry);
            });
            if (document_size == 0 || !last) throw std::runtime_error("root array is empty");
            const auto palette = string_palette(*last, "palette");
            const auto v5 = mVersion == StructureId::GangBanV5;
            const auto stream_end = document_size - (v5 ? 2 : 1);
            if (v5) {
                if (document_size < 2 || !penultimate || !penultimate->is_object() ||
                    !penultimate->contains("ep") || !(*penultimate)["ep"].is_array() ||
                    (*penultimate)["ep"].size() != 3) {
                    throw std::runtime_error("V5 area ep is invalid");
                }
                for (std::size_t index = 0; index < 3; ++index) {
                    (void)json_integer((*penultimate)["ep"][index], "area.ep");
                }
            }
            penultimate.reset();
            last.reset();

            Bounds bounds;
            std::set<BlockPos, std::less<>> first_entity_positions;
            auto process_stream = [&](const bool materialize) -> std::size_t {
                auto place = [&](BlockPos world, std::int32_t primary,
                                 std::int32_t secondary, const Json* payload,
                                 std::size_t block_index) {
                    std::uint32_t runtime = 0;
                    std::optional<NbtPayload> nbt;
                    if (payload != nullptr && payload->is_object()) {
                        runtime = runtime_id(command_block_name(secondary),
                            static_cast<std::uint16_t>(primary));
                        nbt = command_nbt_v5(*payload, secondary);
                    } else {
                        validate_palette_index(primary, palette.size(), block_index);
                        runtime = runtime_id(palette[static_cast<std::size_t>(primary)],
                            static_cast<std::uint16_t>(secondary));
                        if (payload != nullptr && payload->is_array()) {
                            nbt = container_nbt(
                                palette[static_cast<std::size_t>(primary)], *payload);
                        }
                    }
                    if (!materialize) {
                        bounds.add(world);
                        return;
                    }
                    const auto local = subtract(world, bounds.minimum);
                    mStore.put(local, runtime);
                    if (nbt && first_entity_positions.insert(local).second) {
                        mStore.put_entity(local, std::move(*nbt));
                    }
                };

                if (v5) {
                    std::array<std::int32_t, 6> base{};
                    std::size_t field_count = 0;
                    std::size_t block_index = 0;
                    auto finish_block = [&](const Json* payload) {
                        try {
                            place({ base[1], base[2], base[3] }, base[4], base[5],
                                payload, block_index);
                        } catch (const std::exception& error) {
                            throw std::runtime_error("block index " +
                                std::to_string(block_index) + ": " + error.what());
                        }
                        ++block_index;
                        field_count = 0;
                    };

                    for_each_root_entry(path, compressed, [&](std::size_t index, Json&& entry) {
                        if (index >= stream_end) return;
                        if (field_count == base.size()) {
                            if (entry.is_array() || entry.is_object()) {
                                finish_block(&entry);
                                return;
                            }
                            finish_block(nullptr);
                        }
                        if (field_count == 0 && stream_end - index < base.size()) {
                            throw std::runtime_error("block index " +
                                std::to_string(block_index) +
                                " is truncated at stream index " + std::to_string(index));
                        }
                        try {
                            base[field_count++] = json_i32(entry, "V5 block field");
                        } catch (const std::exception& error) {
                            throw std::runtime_error("block index " +
                                std::to_string(block_index) + ": " + error.what());
                        }
                    });
                    if (field_count == base.size()) finish_block(nullptr);
                    return block_index;
                }

                std::int64_t x = 0, y = 0, z = 0;
                std::size_t block_index = 0;
                for_each_root_entry(path, compressed, [&](std::size_t index, Json&& entry) {
                    if (index >= stream_end) return;
                    try {
                        if (!entry.is_array()) {
                            throw std::runtime_error("stream entry is not an array");
                        }
                        if (entry.size() >= 5 && entry[3].is_string() &&
                            entry[4].is_string()) {
                            return;
                        }
                        if (entry.size() < 5) {
                            throw std::runtime_error(
                                "stream entry has fewer than five fields");
                        }
                        x += json_i32(entry[0], "dx");
                        y += json_i32(entry[1], "dy");
                        z += json_i32(entry[2], "dz");
                        if (x < std::numeric_limits<std::int32_t>::min() ||
                            x > std::numeric_limits<std::int32_t>::max() ||
                            y < std::numeric_limits<std::int32_t>::min() ||
                            y > std::numeric_limits<std::int32_t>::max() ||
                            z < std::numeric_limits<std::int32_t>::min() ||
                            z > std::numeric_limits<std::int32_t>::max()) {
                            throw std::runtime_error("delta cursor exceeds int32");
                        }
                        const auto primary = json_i32(entry[3], "primary");
                        const auto secondary = json_i32(entry[4], "secondary");
                        const auto* payload = entry.size() >= 6 ? &entry[5] : nullptr;
                        place({ static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y),
                                  static_cast<std::int32_t>(z) },
                            primary, secondary, payload, block_index);
                    } catch (const std::exception& error) {
                        throw std::runtime_error("block index " +
                            std::to_string(block_index) + " stream index " +
                            std::to_string(index) + ": " + error.what());
                    }
                    ++block_index;
                });
                return block_index;
            };

            const auto validated_blocks = process_stream(false);
            if (validated_blocks == 0) throw std::runtime_error("structure has no blocks");
            mStore.set_size(bounds.size());
            const auto materialized_blocks = process_stream(true);
            if (materialized_blocks != validated_blocks) {
                throw std::runtime_error("GangBan command stream changed while parsing");
            }
            mNonAirBlocks = mStore.count_non_air();
            return Result<void>::success();
        }

        return Result<void>::failure("GangBan reader received an invalid version");
    } catch (const std::exception& error) {
        return Result<void>::failure("parse " + std::string(name()) + " failed: " + error.what());
    }
}

Result<void> GangBanStructure::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> GangBanStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure(std::string(name()) + " has no Go FromMCWorld capability");
}

} // namespace water_structure
