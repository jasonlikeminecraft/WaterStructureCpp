#include "covstructure.hpp"

#include <WaterStructure/world.hpp>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxBlockIndexNestingDepth = 128;
constexpr std::size_t kMaxParserNestingDepth = 1024;

enum class JsonValueKind : std::uint8_t {
    Null,
    Boolean,
    Signed,
    Unsigned,
    Float,
    String,
    Binary,
    Array,
    Object
};

struct ScalarValue {
    JsonValueKind kind = JsonValueKind::Null;
    std::int64_t signed_value = 0;
    std::uint64_t unsigned_value = 0;
    double float_value = 0;
    std::string_view text;
};

struct PaletteEntry {
    std::string name;
    std::optional<std::int64_t> data;
};

std::string trim_name(std::string_view value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
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

std::optional<std::int64_t> integer(const ScalarValue& value)
{
    switch (value.kind) {
    case JsonValueKind::Signed:
        return value.signed_value;
    case JsonValueKind::Unsigned:
        if (value.unsigned_value <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(value.unsigned_value);
        }
        return std::nullopt;
    case JsonValueKind::Float:
        // 2^63 is exactly representable as a double, whereas INT64_MAX is not.
        // Use an exclusive upper bound to avoid an out-of-range float-to-int cast.
        if (std::isfinite(value.float_value) &&
            value.float_value >= -9223372036854775808.0 &&
            value.float_value < 9223372036854775808.0) {
            return static_cast<std::int64_t>(value.float_value);
        }
        return std::nullopt;
    case JsonValueKind::String: {
        const auto first = value.text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return std::nullopt;
        const auto last = value.text.find_last_not_of(" \t\r\n");
        const auto trimmed = value.text.substr(first, last - first + 1);
        std::int64_t result = 0;
        const auto parsed = std::from_chars(
            trimmed.data(), trimmed.data() + trimmed.size(), result);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == trimmed.data() + trimmed.size()) return result;
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

bool palette_shape_key(std::string_view key) noexcept
{
    return key == "name" || key == "states" || key == "properties" ||
        key == "id" || key == "block" || key == "data" || key == "meta";
}

struct PaletteEntryBuilder {
    bool has_palette_shape = false;
    std::optional<std::string> name;
    std::optional<std::string> block;
    std::optional<std::string> id;
    std::optional<std::int64_t> val;
    std::optional<std::int64_t> data;
    std::optional<std::int64_t> meta;
    std::optional<std::int64_t> damage;
    std::optional<std::int64_t> value;

    void consume(std::string_view key, const ScalarValue* scalar)
    {
        if (palette_shape_key(key)) has_palette_shape = true;
        auto consume_string = [&](std::optional<std::string>& destination) {
            destination.reset();
            if (scalar && scalar->kind == JsonValueKind::String) {
                destination.emplace(scalar->text);
            }
        };
        auto consume_integer = [&](std::optional<std::int64_t>& destination) {
            destination.reset();
            if (scalar) destination = integer(*scalar);
        };
        if (key == "name") consume_string(name);
        else if (key == "block") consume_string(block);
        else if (key == "id") consume_string(id);
        else if (key == "val") consume_integer(val);
        else if (key == "data") consume_integer(data);
        else if (key == "meta") consume_integer(meta);
        else if (key == "damage") consume_integer(damage);
        else if (key == "value") consume_integer(value);
    }

    PaletteEntry decode() const
    {
        PaletteEntry result{ "minecraft:air", std::nullopt };
        for (const auto* candidate : { &name, &block, &id }) {
            if (*candidate && !(*candidate)->empty()) {
                result.name = **candidate;
                break;
            }
        }
        for (const auto* candidate : { &data, &meta, &damage, &value }) {
            if (*candidate) {
                result.data = **candidate;
                break;
            }
        }
        return result;
    }
};

std::uint32_t runtime_id(RuntimeRegistry& registry, const PaletteEntry& entry)
{
    const auto name = trim_name(entry.name);
    if (entry.data) {
        if (const auto runtime = registry.legacy_runtime_id(
                name, static_cast<std::uint16_t>(*entry.data))) return *runtime;
    }
    if (const auto runtime = registry.find(name)) return *runtime;
    if (const auto runtime = registry.java_runtime_id(name)) return *runtime;
    if (const auto unknown = registry.find("minecraft:unknown")) return *unknown;
    return registry.register_state({ "minecraft:unknown", {}, 0 });
}

struct PaletteChildLayout {
    bool is_object = false;
    bool has_palette_shape = false;
    std::optional<std::size_t> block_palette_array_serial;
};

struct PaletteCandidateLayout {
    JsonValueKind kind = JsonValueKind::Null;
    std::map<std::string, PaletteChildLayout, std::less<>> children;
};

enum class PaletteMode : std::uint8_t {
    Empty,
    ArrayEntries,
    ObjectEntries
};

struct SelectedPaletteLayout {
    PaletteMode mode = PaletteMode::Empty;
    std::size_t palette_serial = 0;
    std::size_t entries_array_serial = 0;
};

struct DiscoveryResult {
    bool root_is_object = false;
    bool size_present = false;
    bool dimensions_present = false;
    std::vector<std::int32_t> size_values;
    std::vector<std::int32_t> dimension_values;
    bool root_palette_present = false;
    std::size_t root_palette_serial = 0;

    struct StructureSelection {
        bool is_object = false;
        bool palette_present = false;
        std::size_t palette_serial = 0;
        bool block_indices_present = false;
        std::size_t block_indices_serial = 0;
        bool blocks_present = false;
        std::size_t blocks_serial = 0;
    } structure;

    std::unordered_map<std::size_t, PaletteCandidateLayout> palette_layouts;

    std::optional<std::size_t> selected_block_serial() const noexcept
    {
        if (!structure.is_object) return std::nullopt;
        if (structure.block_indices_present) return structure.block_indices_serial;
        if (structure.blocks_present) return structure.blocks_serial;
        return std::nullopt;
    }

    std::optional<std::size_t> selected_palette_serial() const noexcept
    {
        if (structure.is_object && structure.palette_present) {
            return structure.palette_serial;
        }
        if (root_palette_present) return root_palette_serial;
        return std::nullopt;
    }

    SelectedPaletteLayout selected_palette_layout() const
    {
        SelectedPaletteLayout selected;
        const auto serial = selected_palette_serial();
        if (!serial) return selected;
        selected.palette_serial = *serial;
        const auto found = palette_layouts.find(*serial);
        if (found == palette_layouts.end()) return selected;
        if (found->second.kind == JsonValueKind::Array) {
            selected.mode = PaletteMode::ArrayEntries;
            selected.entries_array_serial = *serial;
            return selected;
        }
        if (found->second.kind != JsonValueKind::Object) return selected;
        for (const auto& [_, child] : found->second.children) {
            if (child.is_object && child.block_palette_array_serial) {
                selected.mode = PaletteMode::ArrayEntries;
                selected.entries_array_serial = *child.block_palette_array_serial;
                return selected;
            }
        }
        selected.mode = PaletteMode::ObjectEntries;
        return selected;
    }
};

class DiscoverySax final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return scalar({ JsonValueKind::Null }); }
    bool boolean(bool) override { return scalar({ JsonValueKind::Boolean }); }
    bool number_integer(number_integer_t value) override
    {
        ScalarValue scalar_value{ JsonValueKind::Signed };
        scalar_value.signed_value = value;
        return scalar(scalar_value);
    }
    bool number_unsigned(number_unsigned_t value) override
    {
        ScalarValue scalar_value{ JsonValueKind::Unsigned };
        scalar_value.unsigned_value = value;
        return scalar(scalar_value);
    }
    bool number_float(number_float_t value, const string_t&) override
    {
        ScalarValue scalar_value{ JsonValueKind::Float };
        scalar_value.float_value = value;
        return scalar(scalar_value);
    }
    bool string(string_t& value) override
    {
        ScalarValue scalar_value{ JsonValueKind::String };
        scalar_value.text = value;
        return scalar(scalar_value);
    }
    bool binary(binary_t&) override { return scalar({ JsonValueKind::Binary }); }

    bool start_object(std::size_t) override
    {
        return start_container(JsonValueKind::Object);
    }
    bool end_object() override { return end_container(JsonValueKind::Object); }
    bool start_array(std::size_t) override
    {
        return start_container(JsonValueKind::Array);
    }
    bool end_array() override { return end_container(JsonValueKind::Array); }

    bool key(string_t& value) override
    {
        if (mFrames.empty() || mFrames.back().kind != JsonValueKind::Object) {
            return fail("object key outside an object");
        }
        mFrames.back().key = value;
        return true;
    }

    bool parse_error(
        std::size_t position,
        const std::string&,
        const nlohmann::detail::exception& error) override
    {
        mError = "JSON error at byte " + std::to_string(position) + ": " + error.what();
        return false;
    }

    const DiscoveryResult& result() const noexcept { return mResult; }
    const std::string& error() const noexcept { return mError; }

private:
    enum class FrameRole : std::uint8_t {
        Normal,
        Root,
        Structure,
        SizeArray,
        DimensionsArray,
        PaletteRaw,
        PaletteChild
    };

    struct Frame {
        JsonValueKind kind = JsonValueKind::Null;
        FrameRole role = FrameRole::Normal;
        std::string key;
        std::size_t serial = 0;
        std::size_t palette_serial = 0;
        std::string palette_child_key;
    };

    bool fail(std::string message)
    {
        if (mError.empty()) mError = std::move(message);
        return false;
    }

    std::size_t next_serial()
    {
        if (mSerial == std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("JSON value count exceeds addressable range");
        }
        return ++mSerial;
    }

    bool scalar(const ScalarValue& value)
    {
        try {
            const auto serial = next_serial();
            begin_value(value.kind, serial, &value, nullptr);
            return true;
        } catch (const std::exception& error) {
            return fail(error.what());
        }
    }

    bool start_container(JsonValueKind kind)
    {
        if (mFrames.size() >= kMaxParserNestingDepth) {
            return fail("JSON nesting exceeds limit");
        }
        try {
            Frame frame;
            frame.kind = kind;
            frame.serial = next_serial();
            begin_value(kind, frame.serial, nullptr, &frame);
            mFrames.push_back(std::move(frame));
            return true;
        } catch (const std::exception& error) {
            return fail(error.what());
        }
    }

    bool end_container(JsonValueKind kind)
    {
        if (mFrames.empty() || mFrames.back().kind != kind) {
            return fail("mismatched JSON container");
        }
        mFrames.pop_back();
        return true;
    }

    void record_palette_candidate(
        std::size_t serial, JsonValueKind kind, Frame* child)
    {
        mResult.palette_layouts[serial] = PaletteCandidateLayout{ kind, {} };
        if (kind == JsonValueKind::Object && child) {
            child->role = FrameRole::PaletteRaw;
            child->palette_serial = serial;
        }
    }

    void capture_dimension(
        std::vector<std::int32_t>& destination, const ScalarValue* scalar_value)
    {
        if (!scalar_value) return;
        const auto parsed = integer(*scalar_value);
        if (parsed && *parsed >= std::numeric_limits<std::int32_t>::min() &&
            *parsed <= std::numeric_limits<std::int32_t>::max()) {
            destination.push_back(static_cast<std::int32_t>(*parsed));
        }
    }

    void begin_value(
        JsonValueKind kind,
        std::size_t serial,
        const ScalarValue* scalar_value,
        Frame* child)
    {
        if (mFrames.empty()) {
            mResult.root_is_object = kind == JsonValueKind::Object;
            if (child && kind == JsonValueKind::Object) child->role = FrameRole::Root;
            return;
        }

        auto& parent = mFrames.back();
        if (parent.role == FrameRole::Root) {
            if (parent.key == "size") {
                mResult.size_present = true;
                mResult.size_values.clear();
                if (kind == JsonValueKind::Array && child) {
                    child->role = FrameRole::SizeArray;
                }
            } else if (parent.key == "dimensions") {
                mResult.dimensions_present = true;
                mResult.dimension_values.clear();
                if (kind == JsonValueKind::Array && child) {
                    child->role = FrameRole::DimensionsArray;
                }
            } else if (parent.key == "structure") {
                mResult.structure = {};
                mResult.structure.is_object = kind == JsonValueKind::Object;
                if (kind == JsonValueKind::Object && child) {
                    child->role = FrameRole::Structure;
                }
            } else if (parent.key == "palette") {
                mResult.root_palette_present = true;
                mResult.root_palette_serial = serial;
                record_palette_candidate(serial, kind, child);
            }
            return;
        }

        if (parent.role == FrameRole::Structure) {
            if (parent.key == "palette") {
                mResult.structure.palette_present = true;
                mResult.structure.palette_serial = serial;
                record_palette_candidate(serial, kind, child);
            } else if (parent.key == "block_indices") {
                mResult.structure.block_indices_present = true;
                mResult.structure.block_indices_serial = serial;
            } else if (parent.key == "blocks") {
                mResult.structure.blocks_present = true;
                mResult.structure.blocks_serial = serial;
            }
            return;
        }

        if (parent.role == FrameRole::SizeArray) {
            capture_dimension(mResult.size_values, scalar_value);
            return;
        }
        if (parent.role == FrameRole::DimensionsArray) {
            capture_dimension(mResult.dimension_values, scalar_value);
            return;
        }

        if (parent.role == FrameRole::PaletteRaw) {
            auto& layout = mResult.palette_layouts.at(parent.palette_serial);
            auto& entry = layout.children[parent.key];
            entry = {};
            entry.is_object = kind == JsonValueKind::Object;
            if (kind == JsonValueKind::Object && child) {
                child->role = FrameRole::PaletteChild;
                child->palette_serial = parent.palette_serial;
                child->palette_child_key = parent.key;
            }
            return;
        }

        if (parent.role == FrameRole::PaletteChild) {
            auto& entry = mResult.palette_layouts.at(parent.palette_serial)
                              .children[parent.palette_child_key];
            if (palette_shape_key(parent.key)) entry.has_palette_shape = true;
            if (parent.key == "block_palette") {
                entry.block_palette_array_serial.reset();
                if (kind == JsonValueKind::Array) {
                    entry.block_palette_array_serial = serial;
                }
            }
        }
    }

    DiscoveryResult mResult;
    std::vector<Frame> mFrames;
    std::size_t mSerial = 0;
    std::string mError;
};

class PaletteSax final : public nlohmann::json_sax<Json> {
public:
    explicit PaletteSax(SelectedPaletteLayout layout) : mLayout(layout) {}

    bool null() override { return scalar({ JsonValueKind::Null }); }
    bool boolean(bool) override { return scalar({ JsonValueKind::Boolean }); }
    bool number_integer(number_integer_t value) override
    {
        ScalarValue scalar_value{ JsonValueKind::Signed };
        scalar_value.signed_value = value;
        return scalar(scalar_value);
    }
    bool number_unsigned(number_unsigned_t value) override
    {
        ScalarValue scalar_value{ JsonValueKind::Unsigned };
        scalar_value.unsigned_value = value;
        return scalar(scalar_value);
    }
    bool number_float(number_float_t value, const string_t&) override
    {
        ScalarValue scalar_value{ JsonValueKind::Float };
        scalar_value.float_value = value;
        return scalar(scalar_value);
    }
    bool string(string_t& value) override
    {
        ScalarValue scalar_value{ JsonValueKind::String };
        scalar_value.text = value;
        return scalar(scalar_value);
    }
    bool binary(binary_t&) override { return scalar({ JsonValueKind::Binary }); }

    bool start_object(std::size_t) override
    {
        return start_container(JsonValueKind::Object);
    }
    bool end_object() override { return end_container(JsonValueKind::Object); }
    bool start_array(std::size_t) override
    {
        return start_container(JsonValueKind::Array);
    }
    bool end_array() override { return end_container(JsonValueKind::Array); }

    bool key(string_t& value) override
    {
        if (mFrames.empty() || mFrames.back().kind != JsonValueKind::Object) {
            return fail("object key outside an object");
        }
        mFrames.back().key = value;
        return true;
    }

    bool parse_error(
        std::size_t position,
        const std::string&,
        const nlohmann::detail::exception& error) override
    {
        mError = "JSON error at byte " + std::to_string(position) + ": " + error.what();
        return false;
    }

    std::unordered_map<std::int64_t, PaletteEntry> take_palette()
    {
        if (mLayout.mode == PaletteMode::ObjectEntries) {
            for (auto& [_, entry] : mDirectEntries) {
                if (entry) append_entry(*entry);
            }
        }
        return std::move(mPalette);
    }

    const std::string& error() const noexcept { return mError; }

private:
    enum class FrameRole : std::uint8_t {
        Normal,
        PaletteRaw,
        EntriesArray,
        Entry
    };

    struct Frame {
        JsonValueKind kind = JsonValueKind::Null;
        FrameRole role = FrameRole::Normal;
        std::string key;
        std::size_t serial = 0;
        bool direct_entry = false;
        std::string direct_key;
        PaletteEntryBuilder entry;
    };

    bool fail(std::string message)
    {
        if (mError.empty()) mError = std::move(message);
        return false;
    }

    std::size_t next_serial()
    {
        if (mSerial == std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("JSON value count exceeds addressable range");
        }
        return ++mSerial;
    }

    bool scalar(const ScalarValue& value)
    {
        try {
            const auto serial = next_serial();
            begin_value(value.kind, serial, &value, nullptr);
            return true;
        } catch (const std::exception& error) {
            return fail(error.what());
        }
    }

    bool start_container(JsonValueKind kind)
    {
        if (mFrames.size() >= kMaxParserNestingDepth) {
            return fail("JSON nesting exceeds limit");
        }
        try {
            Frame frame;
            frame.kind = kind;
            frame.serial = next_serial();
            begin_value(kind, frame.serial, nullptr, &frame);
            mFrames.push_back(std::move(frame));
            return true;
        } catch (const std::exception& error) {
            return fail(error.what());
        }
    }

    bool end_container(JsonValueKind kind)
    {
        if (mFrames.empty() || mFrames.back().kind != kind) {
            return fail("mismatched JSON container");
        }
        auto frame = std::move(mFrames.back());
        mFrames.pop_back();
        if (frame.role == FrameRole::Entry && kind == JsonValueKind::Object) {
            if (frame.direct_entry) {
                if (frame.entry.has_palette_shape) {
                    mDirectEntries[frame.direct_key] = std::move(frame.entry);
                } else {
                    mDirectEntries[frame.direct_key].reset();
                }
            } else {
                append_entry(frame.entry);
            }
        }
        return true;
    }

    void append_entry(const PaletteEntryBuilder& builder)
    {
        const auto id = builder.val.value_or(static_cast<std::int64_t>(mEntryIndex));
        if (mEntryIndex != std::numeric_limits<std::size_t>::max()) ++mEntryIndex;
        mPalette[id] = builder.decode();
    }

    void begin_value(
        JsonValueKind kind,
        std::size_t serial,
        const ScalarValue* scalar_value,
        Frame* child)
    {
        if (mLayout.mode == PaletteMode::ObjectEntries &&
            serial == mLayout.palette_serial && kind == JsonValueKind::Object && child) {
            child->role = FrameRole::PaletteRaw;
            return;
        }
        if (mLayout.mode == PaletteMode::ArrayEntries &&
            serial == mLayout.entries_array_serial && kind == JsonValueKind::Array && child) {
            child->role = FrameRole::EntriesArray;
            return;
        }
        if (mFrames.empty()) return;

        auto& parent = mFrames.back();
        if (parent.role == FrameRole::EntriesArray) {
            if (kind == JsonValueKind::Object && child) {
                child->role = FrameRole::Entry;
            }
            return;
        }
        if (parent.role == FrameRole::PaletteRaw) {
            mDirectEntries[parent.key].reset();
            if (kind == JsonValueKind::Object && child) {
                child->role = FrameRole::Entry;
                child->direct_entry = true;
                child->direct_key = parent.key;
            }
            return;
        }
        if (parent.role == FrameRole::Entry) {
            parent.entry.consume(parent.key, scalar_value);
        }
    }

    SelectedPaletteLayout mLayout;
    std::vector<Frame> mFrames;
    std::map<std::string, std::optional<PaletteEntryBuilder>, std::less<>> mDirectEntries;
    std::unordered_map<std::int64_t, PaletteEntry> mPalette;
    std::size_t mSerial = 0;
    std::size_t mEntryIndex = 0;
    std::string mError;
};

struct ResolvedPaletteEntry {
    PaletteEntry entry;
    bool non_air = false;
    std::optional<std::uint32_t> runtime;
};

class BlockIndicesSax final : public nlohmann::json_sax<Json> {
public:
    BlockIndicesSax(
        std::size_t selected_serial,
        Size size,
        std::size_t volume,
        RuntimeRegistry& registry,
        SparseBlockStore& store,
        std::unordered_map<std::int64_t, ResolvedPaletteEntry>& palette)
        : mSelectedSerial(selected_serial),
          mSize(size),
          mVolume(volume),
          mRegistry(registry),
          mStore(store),
          mPalette(palette)
    {
    }

    bool null() override { return scalar({ JsonValueKind::Null }); }
    bool boolean(bool) override { return scalar({ JsonValueKind::Boolean }); }
    bool number_integer(number_integer_t value) override
    {
        ScalarValue scalar_value{ JsonValueKind::Signed };
        scalar_value.signed_value = value;
        return scalar(scalar_value);
    }
    bool number_unsigned(number_unsigned_t value) override
    {
        ScalarValue scalar_value{ JsonValueKind::Unsigned };
        scalar_value.unsigned_value = value;
        return scalar(scalar_value);
    }
    bool number_float(number_float_t value, const string_t&) override
    {
        ScalarValue scalar_value{ JsonValueKind::Float };
        scalar_value.float_value = value;
        return scalar(scalar_value);
    }
    bool string(string_t& value) override
    {
        ScalarValue scalar_value{ JsonValueKind::String };
        scalar_value.text = value;
        return scalar(scalar_value);
    }
    bool binary(binary_t&) override { return scalar({ JsonValueKind::Binary }); }

    bool start_object(std::size_t) override
    {
        return start_container(JsonValueKind::Object);
    }
    bool end_object() override { return end_container(JsonValueKind::Object); }
    bool start_array(std::size_t) override
    {
        return start_container(JsonValueKind::Array);
    }
    bool end_array() override { return end_container(JsonValueKind::Array); }

    bool key(string_t& value) override
    {
        if (mFrames.empty() || mFrames.back().kind != JsonValueKind::Object) {
            return fail("object key outside an object");
        }
        mFrames.back().key = value;
        return true;
    }

    bool parse_error(
        std::size_t position,
        const std::string&,
        const nlohmann::detail::exception& error) override
    {
        mError = "JSON error at byte " + std::to_string(position) + ": " + error.what();
        return false;
    }

    std::size_t non_air_blocks() const noexcept { return mNonAirBlocks; }
    const std::string& error() const noexcept { return mError; }

private:
    enum class FrameRole : std::uint8_t {
        Normal,
        TargetArray,
        Entry
    };

    struct Frame {
        JsonValueKind kind = JsonValueKind::Null;
        FrameRole role = FrameRole::Normal;
        std::string key;
        std::size_t serial = 0;
        std::size_t flatten_depth = 0;
        PaletteEntryBuilder entry;
    };

    bool fail(std::string message)
    {
        if (mError.empty()) mError = std::move(message);
        return false;
    }

    std::size_t next_serial()
    {
        if (mSerial == std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("JSON value count exceeds addressable range");
        }
        return ++mSerial;
    }

    bool scalar(const ScalarValue& value)
    {
        try {
            const auto serial = next_serial();
            begin_value(value.kind, serial, &value, nullptr);
            return true;
        } catch (const std::exception& error) {
            return fail(error.what());
        }
    }

    bool start_container(JsonValueKind kind)
    {
        if (mFrames.size() >= kMaxParserNestingDepth) {
            return fail("JSON nesting exceeds limit");
        }
        try {
            Frame frame;
            frame.kind = kind;
            frame.serial = next_serial();
            begin_value(kind, frame.serial, nullptr, &frame);
            mFrames.push_back(std::move(frame));
            return true;
        } catch (const std::exception& error) {
            return fail(error.what());
        }
    }

    bool end_container(JsonValueKind kind)
    {
        if (mFrames.empty() || mFrames.back().kind != kind) {
            return fail("mismatched JSON container");
        }
        auto frame = std::move(mFrames.back());
        mFrames.pop_back();
        if (frame.role == FrameRole::Entry && kind == JsonValueKind::Object) {
            emit_entry(frame.entry);
        }
        return true;
    }

    void advance_index() noexcept
    {
        if (mIndex != std::numeric_limits<std::size_t>::max()) ++mIndex;
    }

    void count_non_air() noexcept
    {
        if (mNonAirBlocks != std::numeric_limits<std::size_t>::max()) {
            ++mNonAirBlocks;
        }
    }

    void place(std::uint32_t runtime)
    {
        if (mIndex >= mVolume) return;
        const auto width = static_cast<std::size_t>(mSize.width);
        const auto length = static_cast<std::size_t>(mSize.length);
        const auto x = static_cast<std::int32_t>(mIndex % width);
        const auto z = static_cast<std::int32_t>((mIndex / width) % length);
        const auto y = static_cast<std::int32_t>(mIndex / (width * length));
        mStore.put({ x, y, z }, runtime);
    }

    void emit_scalar(const ScalarValue& value)
    {
        const auto palette_id = integer(value);
        auto found = palette_id ? mPalette.find(*palette_id) : mPalette.end();
        if (found != mPalette.end() && found->second.non_air) {
            count_non_air();
            if (value.kind != JsonValueKind::Null &&
                (!palette_id || *palette_id != -1) && mIndex < mVolume) {
                if (!found->second.runtime) {
                    found->second.runtime = runtime_id(mRegistry, found->second.entry);
                }
                place(*found->second.runtime);
            }
        }
        advance_index();
    }

    void emit_entry(const PaletteEntryBuilder& builder)
    {
        const auto entry = builder.decode();
        const auto name = trim_name(entry.name);
        if (!name.empty() && !equal_fold(name, "minecraft:air")) {
            count_non_air();
            if (mIndex < mVolume) place(runtime_id(mRegistry, entry));
        }
        advance_index();
    }

    void begin_value(
        JsonValueKind kind,
        std::size_t serial,
        const ScalarValue* scalar_value,
        Frame* child)
    {
        if (serial == mSelectedSerial) {
            if (kind == JsonValueKind::Array && child) {
                child->role = FrameRole::TargetArray;
                child->flatten_depth = 0;
            } else if (kind == JsonValueKind::Object && child) {
                child->role = FrameRole::Entry;
            } else if (scalar_value) {
                emit_scalar(*scalar_value);
            }
            return;
        }
        if (mFrames.empty()) return;

        auto& parent = mFrames.back();
        if (parent.role == FrameRole::TargetArray) {
            const auto depth = parent.flatten_depth + 1;
            if (depth > kMaxBlockIndexNestingDepth) {
                throw std::runtime_error("block_indices nesting exceeds limit");
            }
            if (kind == JsonValueKind::Array && child) {
                child->role = FrameRole::TargetArray;
                child->flatten_depth = depth;
            } else if (kind == JsonValueKind::Object && child) {
                child->role = FrameRole::Entry;
                child->flatten_depth = depth;
            } else if (scalar_value) {
                emit_scalar(*scalar_value);
            }
            return;
        }
        if (parent.role == FrameRole::Entry) {
            parent.entry.consume(parent.key, scalar_value);
        }
    }

    std::size_t mSelectedSerial = 0;
    Size mSize{};
    std::size_t mVolume = 0;
    RuntimeRegistry& mRegistry;
    SparseBlockStore& mStore;
    std::unordered_map<std::int64_t, ResolvedPaletteEntry>& mPalette;
    std::vector<Frame> mFrames;
    std::size_t mSerial = 0;
    std::size_t mIndex = 0;
    std::size_t mNonAirBlocks = 0;
    std::string mError;
};

template <typename Handler>
void parse_sax(const std::filesystem::path& path, Handler& handler)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open CovStructure file: " + path.string());
    }
    if (!Json::sax_parse(input, &handler)) {
        if (!handler.error().empty()) throw std::runtime_error(handler.error());
        throw std::runtime_error("invalid CovStructure JSON");
    }
}

} // namespace

Result<void> CovStructureReader::read(const std::filesystem::path& path)
{
    mStore.clear();
    mNonAirBlocks = 0;
    try {
        // Pass 1 retains only dimensions and palette/index locations. Palette
        // and block arrays may appear in either order, so resolving them in a
        // single pass would require buffering the complete block_indices value.
        DiscoverySax discovery;
        parse_sax(path, discovery);
        const auto& metadata = discovery.result();
        if (!metadata.root_is_object) throw std::runtime_error("root is not an object");

        const auto& dims = metadata.size_present
            ? metadata.size_values
            : metadata.dimension_values;
        if ((!metadata.size_present && !metadata.dimensions_present) ||
            dims.size() < 3 || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
            throw std::runtime_error(metadata.size_present || metadata.dimensions_present
                ? "size is invalid" : "size is missing");
        }

        const Size size{ dims[0], dims[1], dims[2] };
        const auto width = static_cast<std::uint64_t>(size.width);
        const auto height = static_cast<std::uint64_t>(size.height);
        const auto length = static_cast<std::uint64_t>(size.length);
        if (width > std::numeric_limits<std::uint64_t>::max() / height ||
            width * height > std::numeric_limits<std::uint64_t>::max() / length ||
            width * height * length > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("size volume exceeds addressable range");
        }
        const auto volume = static_cast<std::size_t>(width * height * length);
        mStore.set_size(size);

        // Pass 2 decodes only the selected palette. Object-form palettes keep
        // one compact entry per state; no unrelated JSON subtree is retained.
        std::unordered_map<std::int64_t, PaletteEntry> palette;
        const auto palette_layout = metadata.selected_palette_layout();
        if (palette_layout.mode != PaletteMode::Empty) {
            PaletteSax palette_parser(palette_layout);
            parse_sax(path, palette_parser);
            palette = palette_parser.take_palette();
        }

        std::unordered_map<std::int64_t, ResolvedPaletteEntry> resolved_palette;
        resolved_palette.reserve(palette.size());
        for (const auto& [id, entry] : palette) {
            const auto name = trim_name(entry.name);
            const bool non_air = !name.empty() && !equal_fold(name, "minecraft:air");
            resolved_palette.emplace(id, ResolvedPaletteEntry{
                entry, non_air, std::nullopt });
        }

        // Pass 3 flattens block_indices directly from SAX events and writes
        // sparse placements as they arrive. It never constructs the root DOM
        // or a volume-sized flattened/index array.
        if (const auto block_serial = metadata.selected_block_serial()) {
            BlockIndicesSax block_parser(
                *block_serial, size, volume, mRegistry, mStore, resolved_palette);
            parse_sax(path, block_parser);
            mNonAirBlocks = block_parser.non_air_blocks();
        }
        return Result<void>::success();
    } catch (const std::exception& error) {
        mStore.clear();
        mNonAirBlocks = 0;
        return Result<void>::failure(
            "parse CovStructure failed: " + std::string(error.what()));
    }
}

Result<void> CovStructureReader::write_to_world(
    WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const
{
    return convert_to_world(*this, world, start, std::move(callbacks));
}

Result<void> CovStructureReader::read_from_world(
    WorldSource&, BlockBox, ConversionCallbacks)
{
    return Result<void>::failure("CovStructure has no Go FromMCWorld capability");
}

} // namespace water_structure
