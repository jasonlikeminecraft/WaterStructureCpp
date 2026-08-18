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
#include <cctype>
#include <fstream>
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
        auto result = mSource.get_chunks(positions);
        if (result && mCallbacks.progress) advance(positions.size());
        return result;
    }

    Result<ChunkMap> get_chunks_layer0(std::span<const ChunkPos> positions) const override
    {
        auto result = mSource.get_chunks_layer0(positions);
        if (result && mCallbacks.progress) advance(positions.size());
        return result;
    }

    Result<void> visit_chunks(
        std::span<const ChunkPos> positions,
        const ChunkVisitor& visitor) const override
    {
        std::size_t visited = 0;
        auto result = mSource.visit_chunks(positions,
            [&](ChunkPos position, const ChunkData& chunk) -> Result<void> {
                auto result = visitor(position, chunk);
                if (result) {
                    ++visited;
                    if (mCallbacks.progress) advance();
                }
                return result;
            });
        if (result && mCallbacks.progress && visited < positions.size()) {
            advance(positions.size() - visited);
        }
        return result;
    }

    Result<void> visit_chunk_nbt(
        std::span<const ChunkPos> positions,
        const ChunkNbtVisitor& visitor) const override
    {
        return mSource.visit_chunk_nbt(positions, visitor);
    }

    void release_cached_chunks() const noexcept override
    {
        mSource.release_cached_chunks();
    }

    Result<void> visit_chunk_palettes(
        std::span<const ChunkPos> positions,
        const ChunkPaletteVisitor& visitor) const override
    {
        std::size_t visited = 0;
        auto result = mSource.visit_chunk_palettes(positions,
            [&](ChunkPos position, std::span<const SubChunkPaletteData> palettes) -> Result<void> {
                auto result = visitor(position, palettes);
                if (result) {
                    ++visited;
                    if (mCallbacks.progress) advance();
                }
                return result;
            });
        if (result && mCallbacks.progress && visited < positions.size()) {
            advance(positions.size() - visited);
        }
        return result;
    }

    std::size_t preferred_palette_batch_size() const noexcept override
    {
        return mSource.preferred_palette_batch_size();
    }

    Result<NbtChunkMap> get_chunk_nbt(std::span<const ChunkPos> positions) const override
    {
        return mSource.get_chunk_nbt(positions);
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

private:
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

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

const std::vector<FormatInfo>& FormatRegistry::formats()
{
    return kFormats;
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
        return *std::find_if(kFormats.begin(), kFormats.end(), [id](const auto& value) {
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
    if (ext == ".np") return Result<FormatInfo>::success(by_id(StructureId::NexusNP));
    if (ext == ".bds") return Result<FormatInfo>::success(by_id(StructureId::BDS));
    if (static_cast<unsigned char>(header[0]) == 0x0a) {
        return Result<FormatInfo>::success(ext == ".bp" ? by_id(StructureId::AxiomBP) : by_id(StructureId::MCStructure));
    }

    // JSON formats share extensions, so use the same field-based order as Go's
    // registry before falling back to the extension table.
    if (ext == ".json" || ext == ".building" || ext == ".buildingx" ||
        ext == ".reb" || ext == ".covstructure") {
        std::ifstream json_input(path, std::ios::binary);
        nlohmann::json document;
        try {
            json_input >> document;
            if (document.is_object()) {
                if (document.value("version", std::string{}) == "TimeBuilder") return Result<FormatInfo>::success(by_id(StructureId::TimeBuilderV1));
                if (document.contains("FuHongBuild_FinalFormat")) return Result<FormatInfo>::success(by_id(StructureId::FuHongV2));
                if (document.contains("FuHongBuild")) {
                    return Result<FormatInfo>::success(by_id(document.contains("BlockCalculationPos") ? StructureId::FuHongV3 : StructureId::FuHongV4));
                }
                if (document.contains("chunkedBlocks") && document.contains("namespaces")) return Result<FormatInfo>::success(by_id(StructureId::MianYangV1));
                if (document.contains("totalBlocks")) return Result<FormatInfo>::success(by_id(StructureId::QingXuV1));
                if (document.contains("block") && document.contains("version")) return Result<FormatInfo>::success(by_id(StructureId::TimeBuilderV1));
                if ((document.contains("size") || document.contains("dimensions")) &&
                    document.contains("structure")) return Result<FormatInfo>::success(by_id(StructureId::CovStructure));
            } else if (document.is_array() && !document.empty()) {
                if (document.front().is_array() && document.back().is_array()) {
                    return Result<FormatInfo>::success(by_id(StructureId::GangBanV6));
                }
                if (document.size() >= 2 && document.front().is_number() &&
                    document[document.size() - 2].is_object() &&
                    document[document.size() - 2].contains("ep") && document.back().is_array()) {
                    return Result<FormatInfo>::success(by_id(StructureId::GangBanV5));
                }
                if (document.front().is_object()) {
                    if (std::ranges::all_of(document, [](const auto& entry) {
                        return entry.is_object() && entry.contains("name") &&
                            entry.contains("x") && entry.contains("y") && entry.contains("z") &&
                            entry["name"].is_string() && entry["x"].is_number_integer() &&
                            entry["y"].is_number_integer() && entry["z"].is_number_integer();
                    })) {
                        return Result<FormatInfo>::success(by_id(StructureId::RunAway));
                    }
                    if (document.front().contains("name") && document.front().contains("aux") &&
                        document.front().contains("x") && document.front().contains("y") &&
                        document.front().contains("z")) {
                        return Result<FormatInfo>::success(by_id(StructureId::FuHongV1));
                    }
                    if (document.size() >= 2 && document.back().is_object() && document.back().contains("list")) {
                        const auto& penultimate = document[document.size() - 2];
                        return Result<FormatInfo>::success(by_id(
                            penultimate.is_object() && penultimate.contains("start") && penultimate.contains("end")
                                ? StructureId::GangBanV1 : StructureId::GangBanV2));
                    }
                    if (document.size() >= 2 && document.front().contains("xcha")) {
                        if (document[1].is_string()) return Result<FormatInfo>::success(by_id(StructureId::GangBanV3));
                        if (document[1].is_array()) return Result<FormatInfo>::success(by_id(StructureId::GangBanV4));
                    }
                }
            }
        } catch (...) {
            // Extension fallback below retains the Go registry's eventual error.
        }
    }

    for (const auto& format : kFormats) {
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
        for (const auto format : { StructureId::SchemV1, StructureId::SchemV2 }) {
            auto reader = std::make_unique<SchemStructure>(registry, format);
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
    std::unique_ptr<ProgressStructure> progress_structure;
    const IStructure* input = &structure;
    if (options.callbacks.start || options.callbacks.progress) {
        progress_structure = std::make_unique<ProgressStructure>(structure, options.callbacks);
        const auto size = structure.size();
        const auto total = static_cast<std::size_t>(size.chunk_x_count()) *
            static_cast<std::size_t>(size.chunk_z_count());
        progress_structure->set_total(total);
        input = progress_structure.get();
    }
    if (format == StructureId::MCStructure) {
        return write_mcstructure(*input, registry, path);
    }
    if (format == StructureId::BDX) {
        return write_bdx(*input, registry, path);
    }
    if (format == StructureId::AxiomBP) {
        return write_axiom_bp(*input, registry, path);
    }
    if (format == StructureId::SchemV1 || format == StructureId::SchemV2) {
        return write_schem(*input, registry, format, path);
    }
    if (format == StructureId::Litematic) {
        return write_litematic(*input, registry, path);
    }
    if (format == StructureId::Schematic) {
        return write_schematic(*input, registry, path);
    }
    if (format == StructureId::IBImport) {
        return write_ibimport(*input, registry, path, options);
    }
    if (format == StructureId::FuHongV4 || format == StructureId::FuHongV5) {
        return write_fuhong(*input, registry, format, path);
    }
    if (format == StructureId::MCFunction) {
        return write_mcfunction(*input, registry, path, options);
    }
    return Result<void>::failure(
        "capability error: 目标格式 " + to_string(format) + " 没有已验证的 writer");
}

std::string to_string(StructureId id)
{
    const auto it = std::find_if(kFormats.begin(), kFormats.end(), [id](const auto& value) {
        return value.id == id;
    });
    return it == kFormats.end() ? "Unknown" : it->name;
}

} // namespace water_structure
