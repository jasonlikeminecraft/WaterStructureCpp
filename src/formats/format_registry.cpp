#include <WaterStructure/format_registry.hpp>

#include "kbdx_structure.hpp"
#include "bdx.hpp"
#include "bdx_writer.hpp"
#include "bcf.hpp"
#include "axiom_bp.hpp"
#include "axiom_bp_writer.hpp"
#include "covstructure.hpp"
#include "construction.hpp"
#include "fuhong.hpp"
#include "fuhong_writer.hpp"
#include "gangban.hpp"
#include "ibimport.hpp"
#include "ibimport_writer.hpp"
#include "runaway.hpp"
#include "litematic.hpp"
#include "litematic_writer.hpp"
#include "mcfunction.hpp"
#include "mcfunction_writer.hpp"
#include "mcstructure.hpp"
#include "mcstructure_writer.hpp"
#include "mcworld.hpp"
#include "mianyang.hpp"
#include "msgpack_structure.hpp"
#include "qingxu.hpp"
#include "schematic.hpp"
#include "schematic_writer.hpp"
#include "schem.hpp"
#include "schem_writer.hpp"
#include "timebuilder.hpp"
#include "tibi.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <nlohmann/json.hpp>

namespace water_structure {

namespace {

class ProgressStructure final : public IStructure {
public:
    ProgressStructure(const IStructure& source, ConversionCallbacks callbacks)
        : mSource(source), mCallbacks(std::move(callbacks)) {}

    StructureId id() const noexcept override { return mSource.id(); }
    std::string_view name() const noexcept override { return mSource.name(); }
    Size size() const noexcept override { return mSource.size(); }
    BlockPos offset() const noexcept override { return mSource.offset(); }
    void set_offset(BlockPos value) noexcept override
    {
        const_cast<IStructure&>(mSource).set_offset(value);
    }

    Result<void> read(const std::filesystem::path& path) override
    {
        return const_cast<IStructure&>(mSource).read(path);
    }

    Result<ChunkMap> get_chunks(std::span<const ChunkPos> positions) const override
    {
        const auto started = std::chrono::steady_clock::now();
        auto result = mSource.get_chunks(positions);
        record_chunk_time(std::chrono::steady_clock::now() - started);
        if (result && mCallbacks.progress) advance(positions.size());
        return result;
    }

    Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const override
    {
        const auto started = std::chrono::steady_clock::now();
        auto result = mSource.get_chunks_layer0(positions);
        record_chunk_time(std::chrono::steady_clock::now() - started);
        if (result && mCallbacks.progress) advance(positions.size());
        return result;
    }

    Result<void> visit_chunks(
        std::span<const ChunkPos> positions,
        const ChunkVisitor& visitor) const override
    {
        std::atomic<std::size_t> visited{0};
        std::atomic<std::uint64_t> callback_nanoseconds{0};
        const auto started = std::chrono::steady_clock::now();
        auto result = mSource.visit_chunks(positions,
            [&](ChunkPos position, const ChunkData& chunk) -> Result<void> {
                const auto callback_started = std::chrono::steady_clock::now();
                auto result = visitor(position, chunk);
                if (result) {
                    visited.fetch_add(1, std::memory_order_relaxed);
                    if (mCallbacks.progress) advance();
                }
                callback_nanoseconds.fetch_add(nanoseconds(
                    std::chrono::steady_clock::now() - callback_started),
                    std::memory_order_relaxed);
                return result;
            });
        const auto total_nanoseconds = nanoseconds(
            std::chrono::steady_clock::now() - started);
        const auto callbacks = callback_nanoseconds.load(std::memory_order_relaxed);
        mChunkNanoseconds.fetch_add(
            total_nanoseconds > callbacks ? total_nanoseconds - callbacks : 0,
            std::memory_order_relaxed);
        const auto visited_count = visited.load(std::memory_order_relaxed);
        if (result && mCallbacks.progress && visited_count < positions.size()) {
            advance(positions.size() - visited_count);
        }
        return result;
    }

    Result<void> visit_chunk_nbt(
        std::span<const ChunkPos> positions,
        const ChunkNbtVisitor& visitor) const override
    {
        std::atomic<std::uint64_t> callback_nanoseconds{0};
        const auto started = std::chrono::steady_clock::now();
        auto result = mSource.visit_chunk_nbt(positions,
            [&](ChunkPos position, std::span<const BlockEntity> entities) {
                const auto callback_started = std::chrono::steady_clock::now();
                auto visited = visitor(position, entities);
                callback_nanoseconds.fetch_add(nanoseconds(
                    std::chrono::steady_clock::now() - callback_started),
                    std::memory_order_relaxed);
                return visited;
            });
        const auto total_nanoseconds = nanoseconds(
            std::chrono::steady_clock::now() - started);
        const auto callbacks = callback_nanoseconds.load(std::memory_order_relaxed);
        mNbtNanoseconds.fetch_add(
            total_nanoseconds > callbacks ? total_nanoseconds - callbacks : 0,
            std::memory_order_relaxed);
        return result;
    }

    void release_cached_chunks() const noexcept override
    {
        mSource.release_cached_chunks();
    }

    Result<void> visit_chunk_palettes(
        std::span<const ChunkPos> positions,
        const ChunkPaletteVisitor& visitor) const override
    {
        std::atomic<std::size_t> visited{0};
        std::atomic<std::uint64_t> callback_nanoseconds{0};
        const auto started = std::chrono::steady_clock::now();
        auto result = mSource.visit_chunk_palettes(positions,
            [&](ChunkPos position, std::span<const SubChunkPaletteData> palettes) -> Result<void> {
                const auto callback_started = std::chrono::steady_clock::now();
                auto result = visitor(position, palettes);
                if (result) {
                    visited.fetch_add(1, std::memory_order_relaxed);
                    if (mCallbacks.progress) advance();
                }
                callback_nanoseconds.fetch_add(nanoseconds(
                    std::chrono::steady_clock::now() - callback_started),
                    std::memory_order_relaxed);
                return result;
            });
        const auto total_nanoseconds = nanoseconds(
            std::chrono::steady_clock::now() - started);
        const auto callbacks = callback_nanoseconds.load(std::memory_order_relaxed);
        mChunkNanoseconds.fetch_add(
            total_nanoseconds > callbacks ? total_nanoseconds - callbacks : 0,
            std::memory_order_relaxed);
        const auto visited_count = visited.load(std::memory_order_relaxed);
        if (result && mCallbacks.progress && visited_count < positions.size()) {
            advance(positions.size() - visited_count);
        }
        return result;
    }

    std::size_t preferred_palette_batch_size() const noexcept override
    {
        return mSource.preferred_palette_batch_size();
    }

    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override
    {
        const auto started = std::chrono::steady_clock::now();
        auto result = mSource.get_chunk_nbt(positions);
        mNbtNanoseconds.fetch_add(nanoseconds(
            std::chrono::steady_clock::now() - started), std::memory_order_relaxed);
        return result;
    }

    Result<std::size_t> count_non_air_blocks() const override
    {
        return mSource.count_non_air_blocks();
    }

    Result<void> write_to_world(
        WorldTarget& world,
        SubChunkPos start,
        ConversionCallbacks callbacks) const override
    {
        return mSource.write_to_world(world, start, std::move(callbacks));
    }

    Result<void> read_from_world(
        WorldSource& world,
        BlockBox box,
        ConversionCallbacks callbacks) override
    {
        return const_cast<IStructure&>(mSource).read_from_world(
            world, box, std::move(callbacks));
    }

    void set_total(std::size_t total)
    {
        if (mCallbacks.start) mCallbacks.start(total);
    }

    std::uint64_t chunk_materialization_ms() const noexcept
    {
        return mChunkNanoseconds.load(std::memory_order_relaxed) / 1'000'000u;
    }

    std::uint64_t nbt_entity_decode_ms() const noexcept
    {
        return mNbtNanoseconds.load(std::memory_order_relaxed) / 1'000'000u;
    }

private:
    static std::uint64_t nanoseconds(std::chrono::steady_clock::duration value) noexcept
    {
        const auto measured = std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
        return measured > 0 ? static_cast<std::uint64_t>(measured) : 0;
    }

    void record_chunk_time(std::chrono::steady_clock::duration value) const noexcept
    {
        mChunkNanoseconds.fetch_add(nanoseconds(value), std::memory_order_relaxed);
    }

    void advance(std::size_t count) const
    {
        for (std::size_t index = 0; index < count; ++index) advance();
    }

    void advance() const
    {
        if (mCallbacks.progress) mCallbacks.progress();
    }

    const IStructure& mSource;
    ConversionCallbacks mCallbacks;
    mutable std::atomic<std::uint64_t> mChunkNanoseconds{0};
    mutable std::atomic<std::uint64_t> mNbtNanoseconds{0};
};

const std::vector<FormatInfo> kFormats = {
    { StructureId::Schematic, "Schematic", { ".schematic" }, true, true, true, true, { "gzip/NBT:Schematic" } },
    { StructureId::SchemV1, "SchemV1", { ".schem" }, true, true, true, true, { "gzip/NBT:Palette" } },
    { StructureId::SchemV2, "SchemV2", { ".schem" }, true, true, true, true, { "gzip/NBT:Schematic/Blocks" } },
    { StructureId::Litematic, "Litematic", { ".litematic" }, true, true, true, true, { "gzip/NBT:Regions" } },
    { StructureId::MCStructure, "MCStructure", { ".mcstructure" }, true, true, true, true, { "little-endian NBT:structure" } },
    { StructureId::MCWorld, "MCWorld", { ".mcworld", ".zip" }, true, false },
    { StructureId::BDX, "BDX", { ".bdx" }, true, true, true, true, { "BD@/Brotli/BDX" } },
    { StructureId::Construction, "Construction", { ".construction" }, true, false, true, false,
        { "constrct/version/section-index/NBT" } },
    { StructureId::AxiomBP, "AxiomBP", { ".bp" }, true, true, true, true,
        { "0x0AE5BB36/gzip NBT/BlockRegion" } },
    { StructureId::MCFunction, "MCFunction", { ".mcfunction", ".txt" }, true, true, true, true, { "setblock|fill" } },
    { StructureId::KBDX, "KBDX", { ".kbdx" }, true, false, true, false },
    { StructureId::IBImport, "IBImport", { ".ibi" }, true, true, true, true, { "IBImport " } },
    { StructureId::MianYangV1, "MianYangV1", { ".json" }, true, false, true, false,
        { "JSON:chunkedBlocks/namespaces" } },
    { StructureId::MianYangV2, "MianYangV2", { ".json" }, true, false, true, false,
        { "JSON:chunkedBlocks/namespaces (V1-compatible)" } },
    { StructureId::MianYangV3, "MianYangV3", { ".building" }, true, false, true, false,
        { "zlib/JSON:chunkedBlocks/namespaces" } },
    { StructureId::MianYangV4, "MianYangV4", { ".buildingx" }, true, false, true, false,
        { "gzip/BuildingX" } },
    { StructureId::GangBanV1, "GangBanV1", { ".json" }, true, false, true, false,
        { "JSON:blocks/range/list" } },
    { StructureId::GangBanV2, "GangBanV2", { ".json" }, true, false, true, false,
        { "JSON:blocks/list" } },
    { StructureId::GangBanV3, "GangBanV3", { ".json" }, true, false, true, false,
        { "JSON:xcha/string-palette/chunks" } },
    { StructureId::GangBanV4, "GangBanV4", { ".json" }, true, false, true, false,
        { "JSON:xcha/array-palette/chunks" } },
    { StructureId::GangBanV5, "GangBanV5", { ".json" }, true, false, true, false,
        { "JSON:flat-stream/ep/palette" } },
    { StructureId::GangBanV6, "GangBanV6", { ".json" }, true, false, true, false,
        { "JSON:delta-stream/palette" } },
    { StructureId::GangBanV7, "GangBanV7", { ".reb" }, true, false, true, false,
        { "zlib/JSON:delta-stream/palette" } },
    { StructureId::RunAway, "RunAway", { ".json" }, true, false, true, false },
    { StructureId::QingXuV1, "QingXuV1", { ".json" }, true, false, true, false,
        { "JSON:totalBlocks/string-chunks" } },
    { StructureId::TimeBuilderV1, "TimeBuilderV1", { ".json" }, true, false, true, false,
        { "JSON:version=TimeBuilder/block" } },
    { StructureId::FuHongV1, "FuHongV1", { ".json" }, true, false, true, false,
        { "JSON:block-array/name/aux/x/y/z" } },
    { StructureId::FuHongV2, "FuHongV2", { ".json" }, true, false, true, false,
        { "JSON:FuHongBuild_FinalFormat" } },
    { StructureId::FuHongV3, "FuHongV3", { ".json" }, true, false, true, false,
        { "JSON:FuHongBuild/BlocksList/BlockCalculationPos" } },
    { StructureId::FuHongV4, "FuHongV4", { ".json" }, true, true, true, true,
        { "JSON:FuHongBuild/BlocksList" } },
    { StructureId::FuHongV5, "FuHongV5", { ".fhbuild" }, true, true, true, true,
        { "zlib/FuHongBuild-rune-cipher/JSON" } },
    { StructureId::BDS, "BDS", { ".bds" }, true, false, true, false,
        { "MessagePack:[[name,x,y,z,data,air], ...]" } },
    { StructureId::SIBI, "SIBI", { ".sibi" }, false, false, false, false,
        { "H4/Go oracle unsupported" } },
    { StructureId::BCF, "BCF", { ".bcf" }, true, false, true, false,
        { "BCF/little-endian offset tables" } },
    { StructureId::TIBI, "TIBI", { ".tibi" }, true, false, true, false,
        { "raw-DEFLATE/15-byte-header/MD5-XOR/varint-command-stream" } },
    { StructureId::CovStructure, "CovStructure", { ".covstructure" }, true, false, true, false,
        { "JSON:size/structure.palette/block_indices" } },
    { StructureId::NexusNP, "NexusNP", { ".np" }, true, false, true, false,
        { "MessagePack:[[block_data,block_actor_data]]" } }
};

// Keep the legacy capability columns intact for old clients, but expose an
// audited, directionally explicit view through FormatRegistry::formats(). A
// world-to-structure conversion uses the verified MCWorld reader followed by
// the target's file writer; direct_world_import_implemented separately tracks
// the older per-reader read_from_world virtual.
const std::vector<FormatInfo>& format_table()
{
    static const auto audited = [] {
        auto values = kFormats;
        for (auto& value : values) {
            value.world_to_structure_implemented = value.writer_implemented;
            value.structure_to_world_implemented =
                value.reader_implemented &&
                (value.world_import_implemented || value.id == StructureId::MCWorld);
            value.direct_world_import_implemented = false;

            // A one-chunk visit_chunks fallback only bounds the adapter's
            // return value; it does not make a reader whose read() retains the
            // complete NBT/JSON/MessagePack tree a streaming reader. Advertise
            // only native, source-side bounded implementations here.
            value.streaming_reader_implemented =
                value.id == StructureId::MCWorld ||
                value.id == StructureId::SchemV1 ||
                value.id == StructureId::SchemV2 ||
                value.id == StructureId::BDX ||
                value.id == StructureId::Construction;
            value.streaming_writer_implemented =
                value.id == StructureId::SchemV1 ||
                value.id == StructureId::SchemV2 ||
                value.id == StructureId::BDX ||
                value.id == StructureId::IBImport ||
                value.id == StructureId::MCFunction;

            value.auto_detectable =
                value.id != StructureId::SchemV2 &&
                value.id != StructureId::MianYangV2;
            value.read_projection_lossy = false;
            value.write_projection_lossy = value.writer_implemented &&
                value.id != StructureId::MCStructure;
            value.lossy_round_trip = value.read_projection_lossy ||
                value.write_projection_lossy;
        }
        return values;
    }();
    return audited;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

enum class JsonProbeType : std::uint8_t {
    None,
    Null,
    Boolean,
    Integer,
    Float,
    String,
    Array,
    Object
};

constexpr bool json_probe_number(JsonProbeType type) noexcept
{
    return type == JsonProbeType::Integer || type == JsonProbeType::Float;
}

struct JsonRootElementProbe {
    JsonProbeType type = JsonProbeType::None;
    bool has_name = false;
    bool name_is_string = false;
    bool has_aux = false;
    bool has_x = false;
    bool x_is_integer = false;
    bool has_y = false;
    bool y_is_integer = false;
    bool has_z = false;
    bool z_is_integer = false;
    bool has_list = false;
    bool has_start = false;
    bool has_end = false;
    bool has_xcha = false;
    bool has_ep = false;

    void record(std::string_view key, JsonProbeType value_type) noexcept
    {
        if (key == "name") {
            has_name = true;
            name_is_string = value_type == JsonProbeType::String;
        } else if (key == "aux") {
            has_aux = true;
        } else if (key == "x") {
            has_x = true;
            x_is_integer = value_type == JsonProbeType::Integer;
        } else if (key == "y") {
            has_y = true;
            y_is_integer = value_type == JsonProbeType::Integer;
        } else if (key == "z") {
            has_z = true;
            z_is_integer = value_type == JsonProbeType::Integer;
        } else if (key == "list") {
            has_list = true;
        } else if (key == "start") {
            has_start = true;
        } else if (key == "end") {
            has_end = true;
        } else if (key == "xcha") {
            has_xcha = true;
        } else if (key == "ep") {
            has_ep = true;
        }
    }

    bool runaway_shape() const noexcept
    {
        return type == JsonProbeType::Object && has_name && name_is_string &&
            has_x && x_is_integer && has_y && y_is_integer &&
            has_z && z_is_integer;
    }
};

// Detection must not build a second full JSON DOM before the selected reader
// runs.  This SAX probe retains only the top-level keys and a rolling pair of
// root-array element shapes, which is sufficient to preserve the registry's
// Go-compatible trial order while keeping memory independent of block count.
class JsonFormatProbe final : public nlohmann::json_sax<nlohmann::json> {
public:
    bool null() override { return value(JsonProbeType::Null); }
    bool boolean(bool) override { return value(JsonProbeType::Boolean); }
    bool number_integer(number_integer_t) override { return value(JsonProbeType::Integer); }
    bool number_unsigned(number_unsigned_t) override { return value(JsonProbeType::Integer); }
    bool number_float(number_float_t, const string_t&) override
    {
        return value(JsonProbeType::Float);
    }
    bool string(string_t& input) override
    {
        return value(JsonProbeType::String, input);
    }
    bool binary(binary_t&) override { return value(JsonProbeType::String); }

    bool start_object(std::size_t) override
    {
        if (mFrames.size() >= 256) return false;
        if (!begin_container(JsonProbeType::Object)) return false;
        mFrames.push_back({ JsonProbeType::Object, {} });
        return true;
    }

    bool key(string_t& input) override
    {
        if (mFrames.empty() || mFrames.back().type != JsonProbeType::Object) return false;
        mFrames.back().key = input;
        return true;
    }

    bool end_object() override { return end_container(JsonProbeType::Object); }

    bool start_array(std::size_t) override
    {
        if (mFrames.size() >= 256) return false;
        if (!begin_container(JsonProbeType::Array)) return false;
        mFrames.push_back({ JsonProbeType::Array, {} });
        return true;
    }

    bool end_array() override { return end_container(JsonProbeType::Array); }

    bool parse_error(
        std::size_t,
        const std::string&,
        const nlohmann::detail::exception&) override
    {
        return false;
    }

    std::optional<StructureId> detected() const noexcept
    {
        if (mRootType == JsonProbeType::Object) {
            if (mVersionIsTimeBuilder) return StructureId::TimeBuilderV1;
            if (mHasFuHongFinal) return StructureId::FuHongV2;
            if (mHasFuHongBuild) {
                return mHasBlockCalculationPos ? StructureId::FuHongV3 : StructureId::FuHongV4;
            }
            if (mHasChunkedBlocks && mHasNamespaces) return StructureId::MianYangV1;
            if (mHasTotalBlocks) return StructureId::QingXuV1;
            if (mHasBlock && mHasVersion) return StructureId::TimeBuilderV1;
            if ((mHasSize || mHasDimensions) && mHasStructure) {
                return StructureId::CovStructure;
            }
            return std::nullopt;
        }
        if (mRootType != JsonProbeType::Array || mRootCount == 0) return std::nullopt;
        if (mFirst.type == JsonProbeType::Array && mLast.type == JsonProbeType::Array) {
            return StructureId::GangBanV6;
        }
        if (mRootCount >= 2 && json_probe_number(mFirst.type) &&
            mHasPenultimate && mPenultimate.type == JsonProbeType::Object &&
            mPenultimate.has_ep && mLast.type == JsonProbeType::Array) {
            return StructureId::GangBanV5;
        }
        if (mFirst.type == JsonProbeType::Object) {
            if (mAllRunAway) return StructureId::RunAway;
            if (mFirst.has_name && mFirst.has_aux && mFirst.has_x &&
                mFirst.has_y && mFirst.has_z) {
                return StructureId::FuHongV1;
            }
            if (mRootCount >= 2 && mLast.type == JsonProbeType::Object && mLast.has_list) {
                return mHasPenultimate && mPenultimate.type == JsonProbeType::Object &&
                    mPenultimate.has_start && mPenultimate.has_end
                    ? StructureId::GangBanV1 : StructureId::GangBanV2;
            }
            if (mRootCount >= 2 && mFirst.has_xcha) {
                if (mSecond.type == JsonProbeType::String) return StructureId::GangBanV3;
                if (mSecond.type == JsonProbeType::Array) return StructureId::GangBanV4;
            }
        }
        return std::nullopt;
    }

private:
    struct Frame {
        JsonProbeType type = JsonProbeType::None;
        std::string key;
    };

    bool begin_container(JsonProbeType type)
    {
        if (mFrames.empty()) {
            if (mRootType != JsonProbeType::None) return false;
            mRootType = type;
            return true;
        }
        return begin_value(type, {});
    }

    bool end_container(JsonProbeType type)
    {
        if (mFrames.empty() || mFrames.back().type != type) return false;
        const bool finishes_root_element =
            mRootType == JsonProbeType::Array && mFrames.size() == 2;
        mFrames.pop_back();
        if (finishes_root_element) finish_root_element();
        return true;
    }

    bool value(JsonProbeType type, std::string_view text = {})
    {
        if (mFrames.empty()) {
            if (mRootType != JsonProbeType::None) return false;
            mRootType = type;
            return true;
        }
        if (!begin_value(type, text)) return false;
        if (mRootType == JsonProbeType::Array && mFrames.size() == 1) {
            finish_root_element();
        }
        return true;
    }

    bool begin_value(JsonProbeType type, std::string_view text)
    {
        auto& parent = mFrames.back();
        if (mRootType == JsonProbeType::Array && mFrames.size() == 1 &&
            parent.type == JsonProbeType::Array) {
            if (mRootElementActive) return false;
            mCurrent = {};
            mCurrent.type = type;
            mRootElementActive = true;
            return true;
        }
        if (parent.type != JsonProbeType::Object) return true;
        const auto key = std::move(parent.key);
        parent.key.clear();
        if (mRootType == JsonProbeType::Object && mFrames.size() == 1) {
            record_root_key(key, type, text);
        } else if (mRootType == JsonProbeType::Array && mFrames.size() == 2 &&
                   mFrames.front().type == JsonProbeType::Array && mRootElementActive) {
            mCurrent.record(key, type);
        }
        return true;
    }

    void record_root_key(
        std::string_view key,
        JsonProbeType type,
        std::string_view text) noexcept
    {
        if (key == "version") {
            mHasVersion = true;
            mVersionIsTimeBuilder = type == JsonProbeType::String && text == "TimeBuilder";
        } else if (key == "FuHongBuild_FinalFormat") {
            mHasFuHongFinal = true;
        } else if (key == "FuHongBuild") {
            mHasFuHongBuild = true;
        } else if (key == "BlockCalculationPos") {
            mHasBlockCalculationPos = true;
        } else if (key == "chunkedBlocks") {
            mHasChunkedBlocks = true;
        } else if (key == "namespaces") {
            mHasNamespaces = true;
        } else if (key == "totalBlocks") {
            mHasTotalBlocks = true;
        } else if (key == "block") {
            mHasBlock = true;
        } else if (key == "size") {
            mHasSize = true;
        } else if (key == "dimensions") {
            mHasDimensions = true;
        } else if (key == "structure") {
            mHasStructure = true;
        }
    }

    void finish_root_element() noexcept
    {
        if (!mRootElementActive) return;
        if (mRootCount == 0) {
            mFirst = mCurrent;
            mAllRunAway = mCurrent.runaway_shape();
        } else {
            mAllRunAway = mAllRunAway && mCurrent.runaway_shape();
        }
        if (mRootCount == 1) mSecond = mCurrent;
        mPenultimate = mLast;
        mHasPenultimate = mRootCount != 0;
        mLast = mCurrent;
        if (mRootCount != std::numeric_limits<std::size_t>::max()) ++mRootCount;
        mCurrent = {};
        mRootElementActive = false;
    }

    std::vector<Frame> mFrames;
    JsonProbeType mRootType = JsonProbeType::None;
    JsonRootElementProbe mCurrent;
    JsonRootElementProbe mFirst;
    JsonRootElementProbe mSecond;
    JsonRootElementProbe mPenultimate;
    JsonRootElementProbe mLast;
    std::size_t mRootCount = 0;
    bool mRootElementActive = false;
    bool mHasPenultimate = false;
    bool mAllRunAway = false;
    bool mHasVersion = false;
    bool mVersionIsTimeBuilder = false;
    bool mHasFuHongFinal = false;
    bool mHasFuHongBuild = false;
    bool mHasBlockCalculationPos = false;
    bool mHasChunkedBlocks = false;
    bool mHasNamespaces = false;
    bool mHasTotalBlocks = false;
    bool mHasBlock = false;
    bool mHasSize = false;
    bool mHasDimensions = false;
    bool mHasStructure = false;
};

} // namespace

const std::vector<FormatInfo>& FormatRegistry::formats()
{
    return format_table();
}

Result<ConversionCapability> FormatRegistry::capability(
    StructureId source,
    StructureId target,
    ConversionDirection direction)
{
    const auto& formats = format_table();
    const auto source_it = std::find_if(formats.begin(), formats.end(),
        [source](const auto& value) { return value.id == source; });
    const auto target_it = std::find_if(formats.begin(), formats.end(),
        [target](const auto& value) { return value.id == target; });
    if (source_it == formats.end() || target_it == formats.end()) {
        return Result<ConversionCapability>::failure(
            "capability error: unknown source or target format");
    }

    ConversionCapability result;
    result.source = source;
    result.target = target;
    result.direction = direction;
    switch (direction) {
    case ConversionDirection::FileToFile:
        result.supported = source_it->reader_implemented && target_it->writer_implemented;
        if (!source_it->reader_implemented) {
            result.reason = "source format has no verified reader";
        } else if (!target_it->writer_implemented) {
            result.reason = "target format has no verified writer";
        }
        if (result.supported) {
            result.streaming = source_it->streaming_reader_implemented &&
                target_it->streaming_writer_implemented;
            if (source_it->read_projection_lossy) {
                result.loss_reasons.push_back("source reader uses a lossy projection");
            }
            if (target_it->write_projection_lossy) {
                result.loss_reasons.push_back("target writer omits unsupported states, actors, or metadata");
            }
        }
        break;
    case ConversionDirection::StructureToWorld:
        result.supported = source_it->structure_to_world_implemented &&
            target == StructureId::MCWorld;
        if (target != StructureId::MCWorld) {
            result.reason = "structure-to-world target must be MCWorld";
        } else if (!source_it->structure_to_world_implemented) {
            result.reason = "source format has no verified world writer";
        }
        if (result.supported) {
            result.streaming = source_it->streaming_reader_implemented;
            if (source_it->read_projection_lossy) {
                result.loss_reasons.push_back("source reader uses a lossy projection");
            }
        }
        break;
    case ConversionDirection::WorldToStructure:
        result.supported = source == StructureId::MCWorld &&
            target_it->world_to_structure_implemented;
        if (source != StructureId::MCWorld) {
            result.reason = "world-to-structure source must be MCWorld";
        } else if (!target_it->world_to_structure_implemented) {
            result.reason = "target format has no verified world-to-file writer";
        }
        if (result.supported) {
            result.streaming = source_it->streaming_reader_implemented &&
                target_it->streaming_writer_implemented;
            if (target_it->write_projection_lossy) {
                result.loss_reasons.push_back("target writer omits unsupported states, actors, or metadata");
            }
        }
        break;
    }
    result.lossy = !result.loss_reasons.empty();
    return Result<ConversionCapability>::success(std::move(result));
}

Result<FormatInfo> FormatRegistry::detect(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        return Result<FormatInfo>::failure("文件不存在: " + path.string());
    }

    std::array<char, 8> header{};
    std::ifstream input(path, std::ios::binary);
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    const auto ext = lower(path.extension().string());

    auto by_id = [](StructureId id) -> FormatInfo {
        return *std::find_if(format_table().begin(), format_table().end(), [id](const auto& value) {
            return value.id == id;
        });
    };

    if (std::filesystem::is_directory(path)) {
        if (std::filesystem::is_regular_file(path / "level.dat") &&
            std::filesystem::is_directory(path / "db")) {
            return Result<FormatInfo>::success(by_id(StructureId::MCWorld));
        }
        return Result<FormatInfo>::failure("目录不是有效的 Bedrock world: " + path.string());
    }

    if (std::string_view(header.data(), 2) == "BD") return Result<FormatInfo>::success(by_id(StructureId::BDX));
    if (std::string_view(header.data(), 2) == "IB") return Result<FormatInfo>::success(by_id(StructureId::IBImport));
    if (std::string_view(header.data(), 2) == "co") return Result<FormatInfo>::success(by_id(StructureId::Construction));
    if (std::string_view(header.data(), 2) == "H4") return Result<FormatInfo>::success(by_id(StructureId::SIBI));
    if (std::string_view(header.data(), 3) == "BCF") return Result<FormatInfo>::success(by_id(StructureId::BCF));
    if (std::string_view(header.data(), 2) == "PK") return Result<FormatInfo>::success(by_id(StructureId::MCWorld));
    if (static_cast<unsigned char>(header[0]) == 0x1f &&
        static_cast<unsigned char>(header[1]) == 0x8b && ext == ".buildingx") {
        return Result<FormatInfo>::success(by_id(StructureId::MianYangV4));
    }
    if ((static_cast<unsigned char>(header[0]) == 0x78 ||
         (static_cast<unsigned char>(header[0]) == 0x28 &&
          static_cast<unsigned char>(header[1]) == 0x15)) && ext == ".building") {
        return Result<FormatInfo>::success(by_id(StructureId::MianYangV3));
    }
    if (static_cast<unsigned char>(header[0]) == 0x78 && ext == ".reb") {
        return Result<FormatInfo>::success(by_id(StructureId::GangBanV7));
    }
    if (static_cast<unsigned char>(header[0]) == 0x78 && ext == ".fhbuild") {
        return Result<FormatInfo>::success(by_id(StructureId::FuHongV5));
    }
    // TIBI payloads commonly start with 0x0a (the same marker used by
    // MCStructure).  Give the explicit extension precedence so a valid .tibi
    // file is not misclassified before its reader gets a chance to validate it.
    if (ext == ".tibi") return Result<FormatInfo>::success(by_id(StructureId::TIBI));
    if (ext == ".np") return Result<FormatInfo>::success(by_id(StructureId::NexusNP));
    if (ext == ".bds") return Result<FormatInfo>::success(by_id(StructureId::BDS));
    if (static_cast<unsigned char>(header[0]) == 0x0a) {
        return Result<FormatInfo>::success(ext == ".bp" ? by_id(StructureId::AxiomBP) : by_id(StructureId::MCStructure));
    }

    // JSON formats share extensions, so use the same field-based order as Go's
    // registry before falling back to the extension table. The probe is SAX
    // based so detection does not retain a second complete copy of a large
    // vendor structure immediately before its reader is opened.
    if (ext == ".json" || ext == ".building" || ext == ".buildingx" ||
        ext == ".reb" || ext == ".covstructure") {
        std::ifstream json_input(path, std::ios::binary);
        JsonFormatProbe probe;
        if (json_input && nlohmann::json::sax_parse(json_input, &probe)) {
            if (const auto detected = probe.detected()) {
                return Result<FormatInfo>::success(by_id(*detected));
            }
        }
        // Extension fallback below retains the Go registry's eventual error.
    }

    for (const auto& format : format_table()) {
        if (std::find(format.extensions.begin(), format.extensions.end(), ext) != format.extensions.end()) {
            return Result<FormatInfo>::success(format);
        }
    }
    return Result<FormatInfo>::failure("无法识别结构文件格式: " + path.string());
}

Result<std::unique_ptr<IStructure>> FormatRegistry::open(
    const std::filesystem::path& path,
    RuntimeRegistry& registry,
    const OpenOptions& options)
{
    auto detected = detect(path);
    if (!detected) {
        return Result<std::unique_ptr<IStructure>>::failure(detected.error());
    }
    return open_as(path, detected.value().id, registry, options);
}

Result<std::unique_ptr<IStructure>> FormatRegistry::open_as(
    const std::filesystem::path& path,
    StructureId format,
    RuntimeRegistry& registry,
    const OpenOptions& options)
{
    if (format == StructureId::KBDX) {
        auto reader = std::make_unique<KbdxStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) {
            return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        }
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::BDX) {
        auto reader = std::make_unique<BdxStructure>(registry);
        reader->set_streaming_world_import(options.streaming_world_import);
        reader->set_streaming_options(
            options.allow_temporary_spool,
            options.temporary_directory,
            options.temporary_file_limit_bytes);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::IBImport) {
        auto reader = std::make_unique<IbImportStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::RunAway) {
        auto reader = std::make_unique<RunAwayStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::MianYangV1 ||
        format == StructureId::MianYangV2 ||
        format == StructureId::MianYangV3 ||
        format == StructureId::MianYangV4) {
        auto reader = std::make_unique<MianYangStructure>(registry, format);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::GangBanV1 ||
        format == StructureId::GangBanV2 ||
        format == StructureId::GangBanV3 ||
        format == StructureId::GangBanV4 ||
        format == StructureId::GangBanV5 ||
        format == StructureId::GangBanV6 ||
        format == StructureId::GangBanV7) {
        auto reader = std::make_unique<GangBanStructure>(registry, format);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::QingXuV1) {
        auto reader = std::make_unique<QingXuStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::TimeBuilderV1) {
        auto reader = std::make_unique<TimeBuilderStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::FuHongV1 ||
        format == StructureId::FuHongV2 ||
        format == StructureId::FuHongV3 ||
        format == StructureId::FuHongV4 ||
        format == StructureId::FuHongV5) {
        auto reader = std::make_unique<FuHongStructure>(registry, format);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::BDS || format == StructureId::NexusNP) {
        auto reader = std::make_unique<MsgpackStructure>(registry, format);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::BCF) {
        auto reader = std::make_unique<BcfStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::CovStructure) {
        auto reader = std::make_unique<CovStructureReader>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::Construction) {
        auto reader = std::make_unique<ConstructionReader>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::AxiomBP) {
        auto reader = std::make_unique<AxiomBpReader>(registry);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::MCStructure) {
        auto reader = std::make_unique<McStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) {
            return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        }
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::Schematic) {
        auto reader = std::make_unique<SchematicStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) {
            return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        }
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::SchemV1 || format == StructureId::SchemV2) {
        std::string first_error;
        const std::array attempts{
            format,
            format == StructureId::SchemV1 ? StructureId::SchemV2 : StructureId::SchemV1
        };
        for (const auto attempted_format : attempts) {
            auto reader = std::make_unique<SchemStructure>(registry, attempted_format);
            reader->set_streaming_world_import(options.streaming_world_import);
            reader->set_direct_world_stream(options.direct_schem_world_stream);
            auto parsed = reader->read(path);
            if (parsed) {
                return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
            }
            if (first_error.empty()) first_error = parsed.error();
        }
        return Result<std::unique_ptr<IStructure>>::failure(
            "SchemV1/V2 均解析失败；首个错误: " + first_error
        );
    }
    if (format == StructureId::Litematic) {
        auto reader = std::make_unique<LitematicStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) {
            return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        }
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::MCWorld) {
        auto reader = std::make_unique<McWorldStructure>(registry);
        reader->set_archive_options(
            options.allow_temporary_spool,
            options.temporary_directory,
            options.temporary_file_limit_bytes);
        auto parsed = reader->read(path);
        if (!parsed) {
            return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        }
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::MCFunction) {
        auto reader = std::make_unique<McFunctionStructure>(registry);
        auto parsed = reader->read(path);
        if (!parsed) {
            return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        }
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    if (format == StructureId::TIBI) {
        auto reader = std::make_unique<TibiReader>(registry);
        reader->set_streaming_options(
            options.soft_memory_budget_bytes,
            options.allow_temporary_spool,
            options.temporary_directory,
            options.temporary_file_limit_bytes);
        auto parsed = reader->read(path);
        if (!parsed) return Result<std::unique_ptr<IStructure>>::failure(parsed.error());
        return Result<std::unique_ptr<IStructure>>::success(std::move(reader));
    }
    return Result<std::unique_ptr<IStructure>>::failure(
        "已识别为 " + to_string(format) + "，该格式的 C++ reader 尚未迁移"
    );
}

Result<void> FormatRegistry::write(
    const IStructure& structure,
    StructureId format,
    const std::filesystem::path& path,
    RuntimeRegistry& registry,
    const ConversionOptions& options)
{
    const auto started = std::chrono::steady_clock::now();
    ConversionStats stats;
    stats.source_format = structure.id();
    stats.target_format = format;
    const auto source_size = structure.size();
    const auto source_chunk_x = static_cast<std::size_t>(
        std::max<std::int32_t>(0, source_size.chunk_x_count()));
    const auto source_chunk_z = static_cast<std::size_t>(
        std::max<std::int32_t>(0, source_size.chunk_z_count()));
    if (source_chunk_x != 0 &&
        source_chunk_z > std::numeric_limits<std::size_t>::max() / source_chunk_x) {
        return Result<void>::failure("source chunk count overflows size_t");
    }
    stats.source_chunks = source_chunk_x * source_chunk_z;
    const bool report_statistics = options.collect_statistics ||
        static_cast<bool>(options.callbacks.statistics);
    ProgressStructure* measured_input = nullptr;
    const auto finish = [&](Result<void> result) -> Result<void> {
        stats.success = result.ok();
        if (!result) {
            stats.error_stage = "encode/write";
            stats.error_location = result.error();
        }
        stats.completed_chunks = result.ok() ? stats.source_chunks : 0;
        if (report_statistics) {
            stats.elapsed_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count());
            if (measured_input != nullptr) {
                stats.chunk_materialization_ms +=
                    measured_input->chunk_materialization_ms();
                stats.nbt_entity_decode_ms += measured_input->nbt_entity_decode_ms();
                const auto source_ms = stats.chunk_materialization_ms >
                        std::numeric_limits<std::uint64_t>::max() - stats.nbt_entity_decode_ms
                    ? std::numeric_limits<std::uint64_t>::max()
                    : stats.chunk_materialization_ms + stats.nbt_entity_decode_ms;
                stats.encode_compress_ms += stats.elapsed_ms > source_ms
                    ? stats.elapsed_ms - source_ms : 0;
            }
            if (result.ok()) {
                // Counting is deliberately opt-in: several readers compute
                // this lazily and a benchmark should not pay for it twice.
                auto count = structure.count_non_air_blocks();
                if (count) stats.non_air_blocks = count.value();
            }
            if (const auto* bdx = dynamic_cast<const BdxStructure*>(&structure)) {
                stats.temporary_spool_bytes = static_cast<std::uint64_t>(
                    bdx->temporary_spool_bytes());
            }
            if (options.callbacks.statistics) {
                try {
                    options.callbacks.statistics(stats);
                } catch (...) {
                    // Telemetry is diagnostic and must not alter writer
                    // success/failure or unwind through the C API.
                }
            }
        }
        return result;
    };
    std::unique_ptr<ProgressStructure> progress_structure;
    const IStructure* input = &structure;
    if (report_statistics || options.callbacks.start || options.callbacks.progress) {
        progress_structure = std::make_unique<ProgressStructure>(structure, options.callbacks);
        progress_structure->set_total(stats.source_chunks);
        measured_input = progress_structure.get();
        input = progress_structure.get();
    }
    if (format == StructureId::MCStructure) {
        return finish(write_mcstructure(*input, registry, path, options));
    }
    if (format == StructureId::BDX) {
        return finish(write_bdx(*input, registry, path, options));
    }
    if (format == StructureId::AxiomBP) {
        return finish(write_axiom_bp(*input, registry, path, options));
    }
    if (format == StructureId::SchemV1 || format == StructureId::SchemV2) {
        return finish(write_schem(*input, registry, format, path, options));
    }
    if (format == StructureId::Litematic) {
        return finish(write_litematic(*input, registry, path, options));
    }
    if (format == StructureId::Schematic) {
        return finish(write_schematic(*input, registry, path, options));
    }
    if (format == StructureId::IBImport) {
        return finish(write_ibimport(*input, registry, path, options));
    }
    if (format == StructureId::FuHongV4 || format == StructureId::FuHongV5) {
        return finish(write_fuhong(*input, registry, format, path, options));
    }
    if (format == StructureId::MCFunction) {
        return finish(write_mcfunction(*input, registry, path, options));
    }
    return finish(Result<void>::failure(
        "capability error: 目标格式 " + to_string(format) + " 没有已验证的 writer"));
}

std::string to_string(StructureId id)
{
    const auto it = std::find_if(format_table().begin(), format_table().end(), [id](const auto& value) {
        return value.id == id;
    });
    return it == format_table().end() ? "Unknown" : it->name;
}

} // namespace water_structure
