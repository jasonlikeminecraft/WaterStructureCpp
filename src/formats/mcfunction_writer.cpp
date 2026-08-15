#include "mcfunction_writer.hpp"
#include "../core/bounded_thread_pool.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/format_registry.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace water_structure {
namespace {

constexpr std::int32_t kFillEdge = 32;
constexpr std::int32_t kChunkBatchSize = 32;
constexpr std::size_t kTaskMemoryLimit = 2 * 1024 * 1024;
constexpr std::size_t kStreamBufferSize = 256 * 1024;

struct TaskOutput {
    std::string memory;
    std::filesystem::path spill_path;
};

class TaskCommandBuffer final {
public:
    explicit TaskCommandBuffer(std::filesystem::path spill_path)
        : mSpillPath(std::move(spill_path)), mStaging(kStreamBufferSize)
    {
        mMemory.reserve(kStreamBufferSize);
    }

    explicit TaskCommandBuffer(std::ostream& direct_output)
        : mDirectOutput(&direct_output), mStaging(kStreamBufferSize)
    {
    }

    void append_text(std::string_view text)
    {
        while (!mFailed && !text.empty()) {
            if (mPosition == mStaging.size() && !flush_staging()) return;
            const auto count = std::min(text.size(), mStaging.size() - mPosition);
            std::memcpy(mStaging.data() + mPosition, text.data(), count);
            mPosition += count;
            text.remove_prefix(count);
        }
    }

    void append_character(char value)
    {
        if (mFailed) return;
        if (mPosition == mStaging.size() && !flush_staging()) return;
        mStaging[mPosition++] = value;
    }

    void append_integer(std::int32_t value)
    {
        if (mFailed) return;
        constexpr std::size_t kMaximumIntegerLength = 16;
        if (mStaging.size() - mPosition < kMaximumIntegerLength && !flush_staging()) {
            return;
        }
        const auto converted = std::to_chars(
            mStaging.data() + mPosition,
            mStaging.data() + mStaging.size(),
            value);
        if (converted.ec != std::errc{}) {
            mFailed = true;
            return;
        }
        mPosition = static_cast<std::size_t>(converted.ptr - mStaging.data());
    }

    bool good() const noexcept { return !mFailed; }

    Result<TaskOutput> finish()
    {
        if (!flush_staging()) {
            return Result<TaskOutput>::failure("MCFunction writer 写入任务缓冲失败");
        }
        if (mDirectOutput != nullptr) {
            if (!*mDirectOutput) {
                return Result<TaskOutput>::failure("MCFunction writer 写入输出文件失败");
            }
            return Result<TaskOutput>::success({});
        }
        if (mSpill.is_open()) {
            mSpill.close();
            if (!mSpill) {
                return Result<TaskOutput>::failure(
                    "MCFunction writer 关闭临时文件失败: " + mSpillPath.string());
            }
            return Result<TaskOutput>::success(TaskOutput{ {}, mSpillPath });
        }
        return Result<TaskOutput>::success(TaskOutput{ std::move(mMemory), {} });
    }

private:
    bool flush_staging()
    {
        if (mFailed) return false;
        if (mPosition != 0 && !write_bytes(mStaging.data(), mPosition)) {
            mFailed = true;
            return false;
        }
        mPosition = 0;
        return true;
    }

    bool open_spill()
    {
        if (mSpill.is_open()) return true;
        mSpillBuffer.resize(kStreamBufferSize);
        mSpill.rdbuf()->pubsetbuf(mSpillBuffer.data(),
                                  static_cast<std::streamsize>(mSpillBuffer.size()));
        mSpill.open(mSpillPath, std::ios::binary | std::ios::trunc);
        if (!mSpill) return false;
        if (!mMemory.empty()) {
            mSpill.write(mMemory.data(), static_cast<std::streamsize>(mMemory.size()));
            if (!mSpill) return false;
            std::string{}.swap(mMemory);
        }
        return true;
    }

    bool write_bytes(const char* data, std::size_t size)
    {
        if (mDirectOutput != nullptr) {
            mDirectOutput->write(data, static_cast<std::streamsize>(size));
            return static_cast<bool>(*mDirectOutput);
        }
        if (!mSpill.is_open() && mMemory.size() + size <= kTaskMemoryLimit) {
            mMemory.append(data, size);
            return true;
        }
        if (!open_spill()) return false;
        mSpill.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(mSpill);
    }

    std::ostream* mDirectOutput = nullptr;
    std::filesystem::path mSpillPath;
    std::vector<char> mStaging;
    std::size_t mPosition = 0;
    std::vector<char> mSpillBuffer;
    std::string mMemory;
    std::ofstream mSpill;
    bool mFailed = false;
};

struct BlockRun {
    std::uint32_t runtime = 0;
    std::int32_t x1 = 0;
    std::int32_t x2 = 0;
};

struct Rectangle {
    std::uint32_t runtime = 0;
    std::int32_t x1 = 0;
    std::int32_t x2 = 0;
    std::int32_t z1 = 0;
    std::int32_t z2 = 0;
};

struct Cuboid {
    std::uint32_t runtime = 0;
    std::int32_t x1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y1 = 0;
    std::int32_t y2 = 0;
    std::int32_t z1 = 0;
    std::int32_t z2 = 0;
};

struct CuboidKey {
    std::uint32_t runtime = 0;
    std::int32_t x1 = 0;
    std::int32_t x2 = 0;
    std::int32_t z1 = 0;
    std::int32_t z2 = 0;

    auto operator<=>(const CuboidKey&) const noexcept = default;
};

bool same_run(const BlockRun& left, const BlockRun& right) noexcept {
    return left.runtime == right.runtime && left.x1 == right.x1 && left.x2 == right.x2;
}

CuboidKey cuboid_key(const Rectangle& rectangle) noexcept {
    return {
        rectangle.runtime,
        rectangle.x1,
        rectangle.x2,
        rectangle.z1,
        rectangle.z2
    };
}

CuboidKey cuboid_key(const Cuboid& cuboid) noexcept {
    return {
        cuboid.runtime,
        cuboid.x1,
        cuboid.x2,
        cuboid.z1,
        cuboid.z2
    };
}

// Formats an internal BlockState into a Bedrock command state string. Cannot
// fail: names are prefixed and properties sorted deterministically.
std::string format_bedrock_state(const BlockState& state)
{
    auto name = state.name;
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    if (state.states.empty()) {
        return name;
    }

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
    return name;
}

Result<std::string> bedrock_state_string(
    const RuntimeRegistry& registry,
    std::uint32_t runtime_id,
    BlockPos position)
{
    const auto found = registry.resolve_state(runtime_id);
    if (!found) {
        return Result<std::string>::failure(
            "MCFunction writer: runtime ID " + std::to_string(runtime_id) +
            " 没有 Bedrock block-state 映射，坐标: (" + std::to_string(position.x) + "," +
            std::to_string(position.y) + "," + std::to_string(position.z) + ")");
    }
    return Result<std::string>::success(format_bedrock_state(*found));
}

// Canonical identity of a decoded state, used to cache upgrade + formatting
// once per distinct state across the whole conversion. Includes the version
// because the upgrade schemas are version-dependent. Decoded Bedrock states
// arrive with sorted properties (NBT compounds), so the sort is skipped in
// the common case.
std::string palette_state_key(const BlockState& state)
{
    const auto properties_less = [](const auto& left, const auto& right) {
        if (left.name != right.name) return left.name < right.name;
        if (left.type != right.type) return left.type < right.type;
        return left.value < right.value;
    };
    auto properties = state.states;
    if (!std::is_sorted(properties.begin(), properties.end(), properties_less)) {
        std::sort(properties.begin(), properties.end(), properties_less);
    }
    std::string key = state.name;
    key += '|';
    key += std::to_string(state.version);
    for (const auto& property : properties) {
        key += '|';
        key += property.name;
        key += ':';
        key += std::to_string(static_cast<int>(property.type));
        key += '=';
        key += property.value;
    }
    return key;
}

void write_fill(
    std::ostream& output,
    std::int32_t x1,
    std::int32_t y1,
    std::int32_t z1,
    std::int32_t x2,
    std::int32_t y2,
    std::int32_t z2,
    std::string_view state)
{
    output << "fill " << x1 << ' ' << y1 << ' ' << z1 << ' '
           << x2 << ' ' << y2 << ' ' << z2 << ' ' << state << '\n';
}

void write_task_fill(
    TaskCommandBuffer& output,
    std::int32_t x1,
    std::int32_t y1,
    std::int32_t z1,
    std::int32_t x2,
    std::int32_t y2,
    std::int32_t z2,
    std::string_view state)
{
    output.append_text("fill ");
    output.append_integer(x1);
    output.append_character(' ');
    output.append_integer(y1);
    output.append_character(' ');
    output.append_integer(z1);
    output.append_character(' ');
    output.append_integer(x2);
    output.append_character(' ');
    output.append_integer(y2);
    output.append_character(' ');
    output.append_integer(z2);
    output.append_character(' ');
    output.append_text(state);
    output.append_character('\n');
}

void write_task_setblock(
    TaskCommandBuffer& output,
    std::int32_t x,
    std::int32_t y,
    std::int32_t z,
    std::string_view state)
{
    output.append_text("setblock ");
    output.append_integer(x);
    output.append_character(' ');
    output.append_integer(y);
    output.append_character(' ');
    output.append_integer(z);
    output.append_character(' ');
    output.append_text(state);
    output.append_character('\n');
}

void write_cuboid(
    TaskCommandBuffer& output,
    const Cuboid& cuboid,
    std::string_view state)
{
    // Java's /fill command is limited to 32768 blocks. Splitting every axis
    // at 32 keeps each emitted command within that limit, including thin slabs.
    for (std::int32_t y1 = cuboid.y1; y1 <= cuboid.y2; y1 += kFillEdge) {
        const auto y2 = std::min(cuboid.y2, y1 + kFillEdge - 1);
        for (std::int32_t z1 = cuboid.z1; z1 <= cuboid.z2; z1 += kFillEdge) {
            const auto z2 = std::min(cuboid.z2, z1 + kFillEdge - 1);
            for (std::int32_t x1 = cuboid.x1; x1 <= cuboid.x2; x1 += kFillEdge) {
                const auto x2 = std::min(cuboid.x2, x1 + kFillEdge - 1);
                if (x1 == x2 && y1 == y2 && z1 == z2) {
                    write_task_setblock(output, x1, y1, z1, state);
                } else {
                    write_task_fill(output, x1, y1, z1, x2, y2, z2, state);
                }
            }
        }
    }
}

const BlockLayer* layer_at(
    const ChunkMap& chunks,
    ChunkPos position,
    std::int32_t sub_y)
{
    const auto chunk = chunks.find(position);
    if (chunk == chunks.end()) return nullptr;
    const auto sub_chunk = chunk->second.sub_chunks.find(sub_y);
    return sub_chunk == chunk->second.sub_chunks.end()
        ? nullptr
        : &sub_chunk->second.layer0;
}

// Uniform per-layer access for the run/rectangle/cuboid encoder.
// - `data` points to 4096 entries in either internal (y,z,x) or Bedrock native
//   (x,y,z) order (generic runtime/handle layers).
// - `palette_handles`/`indices` is the palette-preserving mode: `indices` holds
//   4096 native (x,y,z) entries and each maps into `palette_handles`.
// A null `data` and null `indices` means the layer is absent (air).
struct BlockLayerView {
    const std::uint32_t* data = nullptr;
    bool native_layout = false;
    const std::uint32_t* palette_handles = nullptr;
    const std::uint16_t* indices = nullptr;
};

struct BatchBounds {
    std::int32_t chunk_z = 0;
    std::int32_t batch_x = 0;
    std::int32_t batch_end = 0;
    std::int32_t x_begin = 0;
    std::int32_t x_end = 0;
    std::int32_t z_begin = 0;
    std::int32_t z_end = 0;
};

struct WorkerContext {
    std::unordered_map<std::uint32_t, std::string> state_cache;
    std::vector<BlockLayerView> layers;
    std::vector<Rectangle> open_rectangles;
    std::vector<Rectangle> next_rectangles;
    std::vector<Rectangle> rectangles;
    std::vector<BlockRun> runs;
    std::vector<Cuboid> active_cuboids;
    std::vector<Cuboid> next_cuboids;
};

struct TempDirectoryCleanup {
    std::filesystem::path path;

    ~TempDirectoryCleanup()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

Result<std::filesystem::path> create_temp_directory(
    const std::filesystem::path& output_path)
{
    std::error_code error;
    auto base = output_path.parent_path();
    if (base.empty() || !std::filesystem::is_directory(base, error)) {
        error.clear();
        base = std::filesystem::temp_directory_path(error);
        if (error) {
            return Result<std::filesystem::path>::failure(
                "MCFunction writer 无法定位临时目录: " + error.message());
        }
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
        auto candidate = base /
            (".water_structure_mcfunction_" + std::to_string(stamp) + "_" +
             std::to_string(attempt));
        error.clear();
        if (std::filesystem::create_directory(candidate, error)) {
            return Result<std::filesystem::path>::success(std::move(candidate));
        }
        if (error && error != std::errc::file_exists) {
            return Result<std::filesystem::path>::failure(
                "MCFunction writer 无法创建临时目录: " + error.message());
        }
    }
    return Result<std::filesystem::path>::failure(
        "MCFunction writer 无法创建唯一临时目录");
}

std::size_t selected_worker_count(
    const ConversionOptions& options,
    std::size_t task_count)
{
    auto workers = options.thread_count;
    if (workers == 0) {
        // Automatic selection: parallel encoding past 2 threads is
        // memory-bandwidth limited (utopia measurements: 1/2/3/4/8 threads =
        // 17.06/9.55/11.31/11.05/11.28 s), so the default is
        // min(CPU cores, 2); tiny inputs use a single worker to avoid the
        // thread-pool overhead entirely.
        const auto hardware = std::max(1u, std::thread::hardware_concurrency());
        workers = task_count <= 1
            ? 1
            : std::min<std::size_t>(hardware, 2);
    }
    return std::clamp<std::size_t>(workers, 1, std::max<std::size_t>(task_count, 1));
}

template <class LayerFor, class EmitState>
Result<void> encode_batch_impl(
    std::uint32_t air,
    Size size,
    const BatchBounds& bounds,
    WorkerContext& context,
    TaskCommandBuffer& output,
    LayerFor&& layer_for,
    EmitState&& emit_state)
{
    auto& layers = context.layers;
    auto& open_rectangles = context.open_rectangles;
    auto& next_rectangles = context.next_rectangles;
    auto& rectangles = context.rectangles;
    auto& runs = context.runs;
    auto& active_cuboids = context.active_cuboids;
    auto& next_cuboids = context.next_cuboids;

    layers.clear();
    layers.reserve(static_cast<std::size_t>(bounds.batch_end - bounds.batch_x));
    open_rectangles.clear();
    next_rectangles.clear();
    rectangles.clear();
    runs.clear();
    open_rectangles.reserve(static_cast<std::size_t>(bounds.x_end - bounds.x_begin));
    next_rectangles.reserve(static_cast<std::size_t>(bounds.x_end - bounds.x_begin));
    rectangles.reserve(static_cast<std::size_t>(bounds.x_end - bounds.x_begin));
    runs.reserve(static_cast<std::size_t>(bounds.x_end - bounds.x_begin));
    active_cuboids.clear();
    next_cuboids.clear();
    active_cuboids.reserve(static_cast<std::size_t>(bounds.x_end - bounds.x_begin));
    next_cuboids.reserve(static_cast<std::size_t>(bounds.x_end - bounds.x_begin));

    const auto emit_cuboid = [&](const Cuboid& cuboid) -> Result<void> {
        auto state = emit_state(
            cuboid.runtime, { cuboid.x1, cuboid.y1, cuboid.z1 });
        if (!state) return Result<void>::failure(state.error());
        write_cuboid(output, cuboid, state.value());
        if (!output.good()) {
            return Result<void>::failure("MCFunction writer 写入任务输出失败");
        }
        return Result<void>::success();
    };

    for (std::int32_t y = 0; y < size.height; ++y) {
        const auto sub_y = floor_div(y - 64, 16);
        const auto local_y = y - (sub_y * 16 + 64);
        layers.clear();
        for (std::size_t chunk_index = 0;
             chunk_index < static_cast<std::size_t>(bounds.batch_end - bounds.batch_x);
             ++chunk_index) {
            layers.push_back(layer_for(chunk_index, sub_y));
        }

        open_rectangles.clear();
        rectangles.clear();
        for (std::int32_t z = bounds.z_begin; z < bounds.z_end; ++z) {
            const auto local_z = floor_mod(z, 16);
            runs.clear();
            std::uint32_t run_runtime = air;
            std::int32_t run_begin = bounds.x_begin;

            for (std::int32_t x = bounds.x_begin; x < bounds.x_end; ++x) {
                const auto chunk_index =
                    static_cast<std::size_t>((x / 16) - bounds.batch_x);
                const auto& layer = layers[chunk_index];
                std::uint32_t runtime_id = air;
                if (layer.data != nullptr) {
                    const auto local_x = floor_mod(x, 16);
                    const auto index = layer.native_layout
                        ? static_cast<std::size_t>(local_x * 256 + local_y * 16 + local_z)
                        : static_cast<std::size_t>((local_y * 16 + local_z) * 16 + local_x);
                    runtime_id = layer.data[index];
                } else if (layer.indices != nullptr) {
                    const auto local_x = floor_mod(x, 16);
                    const auto native_index =
                        static_cast<std::size_t>(local_x * 256 + local_y * 16 + local_z);
                    runtime_id = layer.palette_handles[layer.indices[native_index]];
                }
                if (runtime_id == run_runtime) continue;
                if (run_runtime != air) {
                    runs.push_back({ run_runtime, run_begin, x - 1 });
                }
                run_runtime = runtime_id;
                run_begin = x;
            }
            if (run_runtime != air) {
                runs.push_back({ run_runtime, run_begin, bounds.x_end - 1 });
            }

            next_rectangles.clear();
            std::size_t open_index = 0;
            for (const auto& run : runs) {
                while (open_index < open_rectangles.size() &&
                       (open_rectangles[open_index].x1 < run.x1 ||
                        (open_rectangles[open_index].x1 == run.x1 &&
                         (open_rectangles[open_index].x2 < run.x2 ||
                          (open_rectangles[open_index].x2 == run.x2 &&
                           open_rectangles[open_index].runtime < run.runtime))))) {
                    rectangles.push_back(open_rectangles[open_index++]);
                }

                if (open_index < open_rectangles.size()) {
                    const auto& open = open_rectangles[open_index];
                    const BlockRun open_run{ open.runtime, open.x1, open.x2 };
                    const BlockRun current_run{ run.runtime, run.x1, run.x2 };
                    if (same_run(open_run, current_run)) {
                        auto extended = open;
                        extended.z2 = z;
                        next_rectangles.push_back(extended);
                        ++open_index;
                        continue;
                    }
                }
                next_rectangles.push_back({ run.runtime, run.x1, run.x2, z, z });
            }
            while (open_index < open_rectangles.size()) {
                rectangles.push_back(open_rectangles[open_index++]);
            }
            open_rectangles.swap(next_rectangles);
        }

        rectangles.insert(rectangles.end(), open_rectangles.begin(), open_rectangles.end());
        std::sort(rectangles.begin(), rectangles.end(), [](const auto& left, const auto& right) {
            return cuboid_key(left) < cuboid_key(right);
        });

        next_cuboids.clear();
        next_cuboids.reserve(active_cuboids.size() + rectangles.size());
        std::size_t active_index = 0;
        std::size_t rectangle_index = 0;
        while (active_index < active_cuboids.size() && rectangle_index < rectangles.size()) {
            const auto& active = active_cuboids[active_index];
            const auto& rectangle = rectangles[rectangle_index];
            const auto active_key = cuboid_key(active);
            const auto rectangle_key = cuboid_key(rectangle);
            if (active_key < rectangle_key) {
                if (const auto emitted = emit_cuboid(active); !emitted) return emitted;
                ++active_index;
            } else if (rectangle_key < active_key) {
                next_cuboids.push_back(Cuboid{
                    rectangle.runtime,
                    rectangle.x1,
                    rectangle.x2,
                    y,
                    y,
                    rectangle.z1,
                    rectangle.z2
                });
                ++rectangle_index;
            } else {
                auto extended = active;
                extended.y2 = y;
                next_cuboids.push_back(extended);
                ++active_index;
                ++rectangle_index;
            }
        }
        while (active_index < active_cuboids.size()) {
            if (const auto emitted = emit_cuboid(active_cuboids[active_index]); !emitted) {
                return emitted;
            }
            ++active_index;
        }
        while (rectangle_index < rectangles.size()) {
            const auto& rectangle = rectangles[rectangle_index++];
            next_cuboids.push_back(Cuboid{
                rectangle.runtime,
                rectangle.x1,
                rectangle.x2,
                y,
                y,
                rectangle.z1,
                rectangle.z2
            });
        }
        active_cuboids.swap(next_cuboids);
    }

    for (const auto& cuboid : active_cuboids) {
        if (const auto emitted = emit_cuboid(cuboid); !emitted) return emitted;
    }
    return Result<void>::success();
}

Result<void> encode_batch(
    const ChunkMap& chunks,
    const RuntimeRegistry& registry,
    std::uint32_t air,
    Size size,
    const BatchBounds& bounds,
    WorkerContext& context,
    TaskCommandBuffer& output)
{
    return encode_batch_impl(
        air, size, bounds, context, output,
        [&](std::size_t chunk_index, std::int32_t sub_y) -> BlockLayerView {
            const auto* layer = layer_at(
                chunks,
                { bounds.batch_x + static_cast<std::int32_t>(chunk_index), bounds.chunk_z },
                sub_y);
            return layer == nullptr
                ? BlockLayerView{}
                : BlockLayerView{ layer->data(), false };
        },
        [&](std::uint32_t runtime, BlockPos position) -> Result<std::string_view> {
            auto cached = context.state_cache.find(runtime);
            if (cached == context.state_cache.end()) {
                auto state = bedrock_state_string(registry, runtime, position);
                if (!state) return Result<std::string_view>::failure(state.error());
                cached = context.state_cache.emplace(
                    runtime, std::move(state.value())).first;
            }
            return Result<std::string_view>::success(
                std::string_view(cached->second));
        });
}

struct PaletteSubChunk {
    // Batch-local handles, one per palette entry; index 0 is "minecraft:air".
    std::vector<std::uint32_t> palette_handles;
    // 4096 indices in Bedrock native (x,y,z) order: index = x*256 + y*16 + z.
    std::vector<std::uint16_t> indices;
};

struct PaletteChunk {
    std::unordered_map<std::int32_t, PaletteSubChunk> sub_chunks;
};

// Palette-mode batch: `formatted` maps batch-local handles to pre-formatted
// Bedrock state strings (handle 0 = "minecraft:air"). Each subchunk keeps its
// palette handles and packed indices; the encoder resolves per block as
// palette_handles[indices[native]].
struct PaletteBatchData {
    std::vector<std::string> formatted;
    std::vector<PaletteChunk> chunks;
};

Result<void> encode_batch_palette(
    const PaletteBatchData& data,
    std::uint32_t air,
    Size size,
    const BatchBounds& bounds,
    WorkerContext& context,
    TaskCommandBuffer& output)
{
    return encode_batch_impl(
        air, size, bounds, context, output,
        [&](std::size_t chunk_index, std::int32_t sub_y) -> BlockLayerView {
            if (chunk_index >= data.chunks.size()) return BlockLayerView{};
            const auto it = data.chunks[chunk_index].sub_chunks.find(sub_y);
            if (it == data.chunks[chunk_index].sub_chunks.end()) return BlockLayerView{};
            return BlockLayerView{
                nullptr,
                false,
                it->second.palette_handles.data(),
                it->second.indices.data()
            };
        },
        [&](std::uint32_t handle, BlockPos) -> Result<std::string_view> {
            if (handle >= data.formatted.size()) {
                return Result<std::string_view>::failure(
                    "MCFunction writer: palette handle 越界");
            }
            return Result<std::string_view>::success(
                std::string_view(data.formatted[handle]));
        });
}

} // namespace

Result<void> write_mcfunction(
    const IStructure& structure,
    RuntimeRegistry& registry,
    const std::filesystem::path& output_path,
    const ConversionOptions& options)
{
    const auto size = structure.size();
    if (size.width <= 0 || size.height <= 0 || size.length <= 0) {
        return Result<void>::failure("MCFunction writer: 结构尺寸无效");
    }
    auto output_buffer = std::make_unique<char[]>(1024 * 1024);
    std::ofstream output;
    output.rdbuf()->pubsetbuf(output_buffer.get(), 1024 * 1024);
    output.open(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Result<void>::failure("无法创建 MCFunction 文件: " + output_path.string());
    }
    output << "# Generated by WaterStructureCpp\n";

    // Clearing the complete box preserves both air holes and trailing-air dimensions.
    for (std::int32_t y = 0; y < size.height; y += kFillEdge) {
        const auto y2 = std::min(size.height - 1, y + kFillEdge - 1);
        for (std::int32_t z = 0; z < size.length; z += kFillEdge) {
            const auto z2 = std::min(size.length - 1, z + kFillEdge - 1);
            for (std::int32_t x = 0; x < size.width; x += kFillEdge) {
                const auto x2 = std::min(size.width - 1, x + kFillEdge - 1);
                write_fill(output, x, y, z, x2, y2, z2, "minecraft:air");
            }
        }
    }

    std::vector<ChunkPos> positions;
    positions.reserve(kChunkBatchSize);
    const auto air = registry.air_runtime_id();
    const auto task_count =
        static_cast<std::size_t>(size.chunk_z_count()) *
        static_cast<std::size_t>(
            (size.chunk_x_count() + kChunkBatchSize - 1) / kChunkBatchSize);
    const auto worker_count = selected_worker_count(options, task_count);

    // Palette fast path: MCWorld feeds subchunk palettes + packed indices
    // directly, so each distinct state is formatted once instead of resolving
    // a runtime ID per block. Other structures report "unsupported" and the
    // writer falls back to the generic get_chunks_layer0() path. The probe
    // runs once on the first batch and the outcome is cached per conversion.
    // WATER_STRUCTURE_MCFUNCTION_NO_PALETTE=1 forces the generic path (used by
    // tests to verify both paths cover identical commands).
    const bool palette_allowed =
        std::getenv("WATER_STRUCTURE_MCFUNCTION_NO_PALETTE") == nullptr;
    bool palette_probed = false;
    bool use_palette = false;

    struct LoadedBatch {
        ChunkMap chunks;
        std::shared_ptr<const PaletteBatchData> palette;
        BatchBounds bounds;
    };

    // Conversion-lifetime cache of decoded state -> formatted Bedrock string.
    // Upgrade + formatting run once per distinct state (utopia-scale worlds
    // repeat a few hundred states across ~1M palette entries), keyed by the
    // raw state signature. Batches then only copy the strings they use.
    std::unordered_map<std::string, std::string> global_states;

    const auto load_palette_batch = [&](
        std::span<const ChunkPos> batch_positions) -> Result<std::shared_ptr<const PaletteBatchData>> {
        auto data = std::make_shared<PaletteBatchData>();
        std::unordered_map<std::string, std::uint32_t> by_formatted;
        data->formatted.push_back("minecraft:air"); // handle 0
        by_formatted.emplace("minecraft:air", 0);
        const bool profile = std::getenv("WATER_STRUCTURE_PROFILE") != nullptr;
        const auto format_start = profile ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
        auto visited = structure.visit_chunk_palettes(
            batch_positions,
            [&](ChunkPos, std::span<const SubChunkPaletteData> subchunks) -> Result<void> {
                PaletteChunk chunk;
                for (const auto& subchunk : subchunks) {
                    std::vector<std::uint32_t> palette_handles;
                    palette_handles.reserve(subchunk.palette.size());
                    for (const auto& state : subchunk.palette) {
                        const auto key = palette_state_key(state);
                        std::string formatted;
                        const auto global = global_states.find(key);
                        if (global == global_states.end()) {
                            auto upgraded = registry.upgrade_state(state);
                            formatted = format_bedrock_state(upgraded);
                            global_states.emplace(key, formatted);
                        } else {
                            formatted = global->second;
                        }
                        const auto found = by_formatted.find(formatted);
                        std::uint32_t handle = 0;
                        if (found == by_formatted.end()) {
                            handle = static_cast<std::uint32_t>(data->formatted.size());
                            by_formatted.emplace(formatted, handle);
                            data->formatted.push_back(std::move(formatted));
                        } else {
                            handle = found->second;
                        }
                        palette_handles.push_back(handle);
                    }
                    // Match generic materialization: subchunks whose palette is
                    // only air are skipped instead of scanned.
                    if (palette_handles.size() == 1 && palette_handles[0] == 0) {
                        continue;
                    }
                    PaletteSubChunk sub_chunk;
                    sub_chunk.palette_handles = std::move(palette_handles);
                    sub_chunk.indices = std::move(subchunk.indices);
                    chunk.sub_chunks.emplace(subchunk.sub_y, std::move(sub_chunk));
                }
                data->chunks.push_back(std::move(chunk));
                return Result<void>::success();
            });
        if (!visited) {
            return Result<std::shared_ptr<const PaletteBatchData>>::failure(visited.error());
        }
        if (profile) {
            const auto format_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - format_start).count();
            std::cerr << "mcfunction_palette_batch_profile format_seconds="
                      << format_seconds << " states=" << data->formatted.size()
                      << " chunks=" << data->chunks.size() << '\n';
        }
        return Result<std::shared_ptr<const PaletteBatchData>>::success(std::move(data));
    };

    const auto load_batch = [&](std::int32_t chunk_z, std::int32_t batch_x)
        -> Result<LoadedBatch> {
        const auto batch_end =
            std::min(size.chunk_x_count(), batch_x + kChunkBatchSize);
        positions.clear();
        for (auto chunk_x = batch_x; chunk_x < batch_end; ++chunk_x) {
            positions.push_back({ chunk_x, chunk_z });
        }
        const BatchBounds bounds{
            chunk_z,
            batch_x,
            batch_end,
            batch_x * 16,
            std::min(size.width, batch_end * 16),
            chunk_z * 16,
            std::min(size.length, chunk_z * 16 + 16)
        };

        if (palette_allowed && (!palette_probed || use_palette)) {
            auto palette = load_palette_batch(positions);
            if (palette) {
                palette_probed = true;
                use_palette = true;
                return Result<LoadedBatch>::success(LoadedBatch{
                    {}, std::move(palette.value()), bounds });
            }
            palette_probed = true;
            use_palette = false;
        }

        auto chunks = structure.get_chunks_layer0(positions);
        structure.release_cached_chunks();
        if (!chunks) {
            return Result<LoadedBatch>::failure(
                "MCFunction writer 获取 chunks 失败: " + chunks.error());
        }
        return Result<LoadedBatch>::success(LoadedBatch{
            std::move(chunks).value(), nullptr, bounds });
    };

    if (worker_count == 1) {
        WorkerContext context;
        TaskCommandBuffer command_output(output);
        for (std::int32_t chunk_z = 0; chunk_z < size.chunk_z_count(); ++chunk_z) {
            for (std::int32_t batch_x = 0;
                 batch_x < size.chunk_x_count();
                 batch_x += kChunkBatchSize) {
                auto batch = load_batch(chunk_z, batch_x);
                if (!batch) return Result<void>::failure(batch.error());
                const auto encoded = batch.value().palette
                    ? encode_batch_palette(
                        *batch.value().palette, 0, size, batch.value().bounds,
                        context, command_output)
                    : encode_batch(
                        batch.value().chunks, registry, air, size,
                        batch.value().bounds, context, command_output);
                if (!encoded) return encoded;
            }
        }
        if (const auto finished = command_output.finish(); !finished) {
            return Result<void>::failure(finished.error());
        }
    } else {
        auto temp_directory_result = create_temp_directory(output_path);
        if (!temp_directory_result) {
            return Result<void>::failure(temp_directory_result.error());
        }
        TempDirectoryCleanup temp_directory{
            std::move(temp_directory_result).value()
        };

        std::vector<std::unique_ptr<WorkerContext>> worker_contexts;
        worker_contexts.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            worker_contexts.push_back(std::make_unique<WorkerContext>());
        }
        detail::BoundedThreadPool pool(worker_count, worker_count);
        const auto max_in_flight = std::max<std::size_t>(
            1,
            options.max_in_flight_tasks == 0
                ? worker_count * 2
                : options.max_in_flight_tasks);

        struct PendingTask {
            std::size_t sequence = 0;
            std::future<Result<TaskOutput>> future;
        };
        std::deque<PendingTask> pending;
        auto copy_buffer = std::make_unique<char[]>(1024 * 1024);

        const auto drain_one = [&]() -> Result<void> {
            auto pending_task = std::move(pending.front());
            pending.pop_front();

            Result<TaskOutput> task_output;
            try {
                task_output = pending_task.future.get();
            } catch (const std::exception& exception) {
                return Result<void>::failure(
                    "MCFunction writer 并行任务 " +
                    std::to_string(pending_task.sequence) + " 异常: " +
                    exception.what());
            }
            if (!task_output) {
                return Result<void>::failure(task_output.error());
            }

            auto& result = task_output.value();
            if (!result.memory.empty()) {
                output.write(
                    result.memory.data(),
                    static_cast<std::streamsize>(result.memory.size()));
            } else if (!result.spill_path.empty()) {
                std::ifstream input;
                input.rdbuf()->pubsetbuf(copy_buffer.get(), 1024 * 1024);
                input.open(result.spill_path, std::ios::binary);
                if (!input) {
                    return Result<void>::failure(
                        "MCFunction writer 无法读取临时文件: " +
                        result.spill_path.string());
                }
                while (input) {
                    input.read(copy_buffer.get(), 1024 * 1024);
                    const auto count = input.gcount();
                    if (count > 0) output.write(copy_buffer.get(), count);
                }
                if (!input.eof()) {
                    return Result<void>::failure(
                        "MCFunction writer 读取临时文件失败: " +
                        result.spill_path.string());
                }
                input.close();
                std::error_code remove_error;
                std::filesystem::remove(result.spill_path, remove_error);
            }
            if (!output) {
                return Result<void>::failure(
                    "写入 MCFunction 文件失败: " + output_path.string());
            }
            return Result<void>::success();
        };

        std::size_t sequence = 0;
        const auto batches_per_z = static_cast<std::size_t>(
            (size.chunk_x_count() + kChunkBatchSize - 1) / kChunkBatchSize);
        const auto total_batches =
            static_cast<std::size_t>(size.chunk_z_count()) * batches_per_z;

        // Palette-mode pipeline: subchunk loads are independent read-only
        // LevelDB work, so a dedicated loader thread prefetches batches while
        // the encode pool works. That overlaps the single-threaded decode
        // cost with the parallel merge/format stage. The generic path keeps
        // loading on the main thread (readers may own mutable chunk caches).
        // The RAII guard cancels and joins the loader on every exit path.
        struct LoadedSlot {
            std::size_t index = 0;
            Result<LoadedBatch> batch;
        };
        struct LoaderPipeline {
            std::thread thread;
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<LoadedSlot> loaded;
            bool done = false;
            bool cancel = false;

            void request_cancel() noexcept {
                std::lock_guard lock(mutex);
                cancel = true;
                cv.notify_all();
            }

            ~LoaderPipeline() {
                if (!thread.joinable()) return;
                request_cancel();
                thread.join();
            }
        } pipeline;
        constexpr std::size_t kPrefetchDepth = 3;

        auto first = load_batch(0, 0);
        if (!first) return Result<void>::failure(first.error());
        const bool pipeline_enabled = first.value().palette != nullptr;
        if (pipeline_enabled) {
            pipeline.thread = std::thread([&]() {
                try {
                    for (std::size_t index = 1; index < total_batches; ++index) {
                        {
                            std::unique_lock lock(pipeline.mutex);
                            pipeline.cv.wait(lock, [&] {
                                return pipeline.loaded.size() < kPrefetchDepth ||
                                    pipeline.cancel;
                            });
                            if (pipeline.cancel) return;
                        }
                        const auto chunk_z =
                            static_cast<std::int32_t>(index / batches_per_z);
                        const auto batch_x = static_cast<std::int32_t>(
                            (index % batches_per_z) * kChunkBatchSize);
                        auto batch = load_batch(chunk_z, batch_x);
                        {
                            std::lock_guard lock(pipeline.mutex);
                            pipeline.loaded.push_back({ index, std::move(batch) });
                        }
                        pipeline.cv.notify_all();
                    }
                } catch (const std::exception& error) {
                    std::lock_guard lock(pipeline.mutex);
                    pipeline.loaded.push_back({
                        total_batches,
                        Result<LoadedBatch>::failure(
                            "MCFunction writer 加载线程异常: " +
                            std::string(error.what()))
                    });
                }
                {
                    std::lock_guard lock(pipeline.mutex);
                    pipeline.done = true;
                }
                pipeline.cv.notify_all();
            });
        }

        for (std::size_t index = 0; index < total_batches; ++index) {
            while (pending.size() >= max_in_flight) {
                if (const auto drained = drain_one(); !drained) return drained;
            }

            Result<LoadedBatch> batch;
            if (index == 0) {
                batch = std::move(first);
            } else if (pipeline_enabled) {
                std::unique_lock lock(pipeline.mutex);
                pipeline.cv.wait(lock, [&] {
                    return !pipeline.loaded.empty() || pipeline.done ||
                        pipeline.cancel;
                });
                if (pipeline.loaded.empty()) {
                    return Result<void>::failure(
                        "MCFunction writer 加载流水线提前结束");
                }
                auto slot = std::move(pipeline.loaded.front());
                pipeline.loaded.pop_front();
                pipeline.cv.notify_all();
                batch = std::move(slot.batch);
            } else {
                const auto chunk_z =
                    static_cast<std::int32_t>(index / batches_per_z);
                const auto batch_x = static_cast<std::int32_t>(
                    (index % batches_per_z) * kChunkBatchSize);
                batch = load_batch(chunk_z, batch_x);
            }
            if (!batch) return Result<void>::failure(batch.error());
            const auto spill_path =
                temp_directory.path /
                ("task_" + std::to_string(sequence) + ".tmp");
            const auto task_sequence = sequence++;
            pending.push_back({
                task_sequence,
                pool.submit_indexed(
                    [loaded = std::move(batch).value(),
                     spill_path,
                     &registry,
                     &worker_contexts,
                     air,
                     size,
                     task_sequence](std::size_t worker_index) mutable
                        -> Result<TaskOutput> {
                        TaskCommandBuffer task_output(spill_path);
                        const auto encoded = loaded.palette
                            ? encode_batch_palette(
                                *loaded.palette, 0, size, loaded.bounds,
                                *worker_contexts[worker_index], task_output)
                            : encode_batch(
                                loaded.chunks, registry, air, size,
                                loaded.bounds,
                                *worker_contexts[worker_index], task_output);
                        if (!encoded) {
                            return Result<TaskOutput>::failure(
                                "MCFunction writer 并行任务 " +
                                std::to_string(task_sequence) + " (" +
                                std::to_string(loaded.bounds.chunk_z) + "," +
                                std::to_string(loaded.bounds.batch_x) + ") 失败: " +
                                encoded.error());
                        }
                        return task_output.finish();
                    })
            });
        }
        while (!pending.empty()) {
            if (const auto drained = drain_one(); !drained) return drained;
        }
    }

    output.flush();
    if (!output) {
        return Result<void>::failure("写入 MCFunction 文件失败: " + output_path.string());
    }
    return Result<void>::success();
}

} // namespace water_structure
