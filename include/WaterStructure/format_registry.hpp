#pragma once

#include "result.hpp"
#include "runtime_registry.hpp"
#include "structure.hpp"

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace water_structure {

struct FormatInfo {
    StructureId id = StructureId::Unknown;
    std::string name;
    std::vector<std::string> extensions;
    bool reader_implemented = false;
    bool writer_implemented = false;
    bool world_import_implemented = false;
    bool world_export_implemented = false;
    std::vector<std::string> magic_signatures;

    // Explicit direction names. The legacy world_import/world_export fields
    // remain for source compatibility, while these fields describe the
    // actual callable capability audited by the registry.
    bool world_to_structure_implemented = false;
    bool structure_to_world_implemented = false;
    bool streaming_reader_implemented = false;
    bool streaming_writer_implemented = false;
    bool lossy_round_trip = false;
    // True only when detect() can distinguish this ID from every alias that
    // shares the same extension and wire layout. SchemV2 and MianYangV2 are
    // currently parser aliases and are selected by a verified trial parse.
    bool auto_detectable = true;
    // The high-level world-to-structure capability uses the MCWorld reader
    // followed by this format's writer. This separate flag describes the
    // legacy IStructure::read_from_world virtual, which is not implemented by
    // the current format classes.
    bool direct_world_import_implemented = false;
    bool read_projection_lossy = false;
    bool write_projection_lossy = false;

    bool can_convert_from_world() const noexcept { return world_to_structure_implemented; }
    bool can_convert_to_world() const noexcept { return structure_to_world_implemented; }
    bool can_read_world() const noexcept { return can_convert_from_world(); }
    bool can_write_world() const noexcept { return can_convert_to_world(); }
};

enum class ConversionDirection : std::uint8_t {
    FileToFile,
    StructureToWorld,
    WorldToStructure
};

struct ConversionCapability {
    StructureId source = StructureId::Unknown;
    StructureId target = StructureId::Unknown;
    ConversionDirection direction = ConversionDirection::FileToFile;
    bool supported = false;
    bool streaming = false;
    bool lossy = false;
    std::string reason;
    std::vector<std::string> loss_reasons;
};

struct ConversionOptions {
    // Keep the original fields and their order first.  Existing C++ callers
    // may use positional aggregate initialization; new knobs are appended
    // below so that source compatibility is retained.
    // Zero selects a conservative hardware-derived default. One disables
    // parallel writer stages and is useful for reproducible benchmarks.
    std::size_t thread_count = 0;
    // Zero keeps at most two tasks per worker in flight.
    std::size_t max_in_flight_tasks = 0;
    // Optional chunk-level callbacks. Writers report completed source chunks
    // through a bounded proxy so callers can display progress without adding
    // per-block overhead.
    ConversionCallbacks callbacks{};
    // MCFunction clears the destination bounding box before placing blocks by
    // default. Set false to emit only non-air placement commands and preserve
    // existing blocks outside the emitted structure.
    bool clear_air = true;
    // MCFunction's 3D optimizer normally merges runs across the requested
    // batch of adjacent chunks. When enabled, each 16x16 chunk is optimized
    // independently, so emitted block/fill commands never cross a chunk
    // boundary. Disabled by default to preserve the historical output shape.
    bool mcfunction_chunk_partition = false;
    // Zero selects a conservative hardware-derived default.  This is the
    // canonical spelling for new code; thread_count remains the legacy alias
    // and is used when worker_count is zero.
    std::size_t worker_count = 0;
    // Soft budget used by streaming implementations to size bounded windows;
    // hard process limits belong to the external test runner.
    std::size_t soft_memory_budget_bytes = 450u * 1024u * 1024u;
    // Maximum number of source chunks retained between pipeline stages.
    // Zero selects a conservative value from thread_count.
    std::size_t max_in_flight_chunks = 0;
    // Permit readers that require seekable compressed input to create a
    // bounded spool instead of silently allocating an unbounded buffer.
    bool allow_temporary_spool = true;
    // Optional directory for bounded spool files.  An empty path uses the
    // platform temporary directory.  Writers must treat this as a hint and
    // never remove files outside the directory they created.
    std::filesystem::path temporary_directory{};
    // Hard ceiling for one temporary spool file. Zero selects the format's
    // conservative protocol ceiling (TIBI currently caps decoded spools at
    // 2 GiB); it is deliberately independent from the RAM budget.
    std::size_t temporary_file_limit_bytes = 0;
    // Emit detailed stage timings to the configured statistics callback (or
    // the legacy stderr profiler when no callback is installed).
    bool profiling = false;
    // Collect stage counters/timestamps for diagnostics and benchmarks.
    bool collect_statistics = false;

    std::size_t resolved_worker_count() const noexcept
    {
        return worker_count != 0 ? worker_count : thread_count;
    }
};

struct OpenOptions {
    // Let readers with a dedicated world path defer expensive block and NBT
    // construction until write_to_world() consumes the source stream.
    bool streaming_world_import = false;
    // For direct to-world conversion, allow SchemV1/V2 to defer BlockData
    // inflation until write_to_world() can consume it. This removes the
    // full-size temporary BlockData write/read round-trip while keeping the
    // normal reader/inspect path fully materialized and random-access capable.
    bool direct_schem_world_stream = false;
    // Reader-side budget.  These fields are intentionally appended so the
    // historical aggregate initialization with one or two booleans remains
    // source compatible.
    std::size_t worker_count = 0;
    std::size_t max_in_flight_chunks = 0;
    std::size_t soft_memory_budget_bytes = 450u * 1024u * 1024u;
    bool allow_temporary_spool = true;
    std::filesystem::path temporary_directory{};
    std::size_t temporary_file_limit_bytes = 0;
    bool collect_statistics = false;
};

class FormatRegistry {
public:
    static const std::vector<FormatInfo>& formats();
    static Result<ConversionCapability> capability(
        StructureId source,
        StructureId target,
        ConversionDirection direction = ConversionDirection::FileToFile);
    static Result<FormatInfo> detect(const std::filesystem::path& path);
    static Result<std::unique_ptr<IStructure>> open(
        const std::filesystem::path& path,
        RuntimeRegistry& registry,
        const OpenOptions& options = {});
    static Result<std::unique_ptr<IStructure>> open_as(
        const std::filesystem::path& path,
        StructureId format,
        RuntimeRegistry& registry,
        const OpenOptions& options = {});
    static Result<void> write(
        const IStructure& structure,
        StructureId format,
        const std::filesystem::path& path,
        RuntimeRegistry& registry,
        const ConversionOptions& options = {});
};

std::string to_string(StructureId id);

} // namespace water_structure
