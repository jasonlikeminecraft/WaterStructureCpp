#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/world.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using water_structure::Result;
using water_structure::StructureId;

constexpr int kUsageError = 1;
constexpr int kInputError = 2;
constexpr int kConversionError = 3;

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string utf8(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const auto bytes = path.u8string();
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
#else
    return path.string();
#endif
}

std::filesystem::path path_from_utf8(std::string_view value)
{
#if defined(_WIN32)
    if (value.empty()) return {};
    const auto required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (required <= 0) return std::filesystem::path(value);
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        wide.data(), required);
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(value);
#endif
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

void print_usage(std::ostream& output = std::cout)
{
    output <<
        "WaterStructureCpp CLI\n\n"
        "Usage:\n"
        "  water_structure_cli inspect <input> [--assets <dir>]\n"
        "  water_structure_cli formats [--writers-only]\n"
        "  water_structure_cli convert <input> <output> [--format <name>] [--threads <n>]\n"
        "  water_structure_cli to-world <input> <world-or-mcworld> [--start <x,y,z>]\n\n"
        "Compatibility syntax:\n"
        "  water_structure_cli convert <input> --format <name> --output <output>\n\n"
        "Options:\n"
        "  -f, --format <name>  Target registry name; inferred from output extension when unique\n"
        "  -o, --output <path>  Output path for compatibility syntax\n"
        "  -j, --threads <n>    Writer worker count (0 = conservative automatic default)\n"
        "      --start <x,y,z>  Target subchunk position; default 0,-4,0\n"
        "      --assets <dir>   Runtime asset directory\n"
        "      --profile        Print internal conversion stage timing\n"
        "  -q, --quiet          Disable progress output\n"
        "  -h, --help           Show this help\n";
}

std::optional<StructureId> parse_format(std::string_view requested)
{
    const auto wanted = lower(requested);
    for (const auto& format : water_structure::FormatRegistry::formats()) {
        if (lower(format.name) == wanted) return format.id;
        for (const auto& extension : format.extensions) {
            auto candidate = extension;
            if (!candidate.empty() && candidate.front() == '.') candidate.erase(candidate.begin());
            if (lower(candidate) == wanted) return format.id;
        }
    }
    return std::nullopt;
}

Result<StructureId> infer_output_format(const std::filesystem::path& output)
{
    const auto extension = lower(output.extension().string());
    std::vector<StructureId> matches;
    for (const auto& format : water_structure::FormatRegistry::formats()) {
        if (!format.writer_implemented) continue;
        if (std::ranges::find(format.extensions, extension) != format.extensions.end()) {
            matches.push_back(format.id);
        }
    }
    if (matches.size() == 1) return Result<StructureId>::success(matches.front());
    if (matches.empty()) {
        return Result<StructureId>::failure(
            "无法从输出扩展名推断目标格式，请使用 --format <name>");
    }
    return Result<StructureId>::failure(
        "输出扩展名对应多个格式，请使用 --format 指定 SchemV1/SchemV2 等具体版本");
}

std::optional<std::size_t> parse_size(std::string_view value)
{
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return result;
}

std::optional<water_structure::SubChunkPos> parse_start(std::string_view value)
{
    water_structure::SubChunkPos result{};
    std::int32_t* fields[] = { &result.x, &result.y, &result.z };
    std::size_t begin = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto end = index == 2 ? value.size() : value.find(',', begin);
        if (end == std::string_view::npos || end == begin) return std::nullopt;
        const auto token = value.substr(begin, end - begin);
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), *fields[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return std::nullopt;
        begin = end + 1;
    }
    return begin == value.size() + 1 ? std::optional(result) : std::nullopt;
}

struct CommonOptions {
    std::filesystem::path assets;
    bool profile = false;
    bool quiet = false;
};

bool take_option_value(int argc, char** argv, int& index, std::string_view& value)
{
    if (index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

Result<std::filesystem::path> locate_assets(
    const std::filesystem::path& executable,
    const std::filesystem::path& configured)
{
    std::vector<std::filesystem::path> candidates;
    if (!configured.empty()) candidates.push_back(configured);
    candidates.push_back(std::filesystem::current_path() / "assets");
    auto parent = std::filesystem::absolute(executable).parent_path();
    for (int depth = 0; depth < 8 && !parent.empty(); ++depth) {
        candidates.push_back(parent / "assets");
        parent = parent.parent_path();
    }
    for (auto candidate : candidates) {
        if (candidate.filename() == "block_mappings_v1.json") candidate = candidate.parent_path();
        if (std::filesystem::is_regular_file(candidate / "block_mappings_v1.json")) {
            return Result<std::filesystem::path>::success(std::move(candidate));
        }
    }
    return Result<std::filesystem::path>::failure(
        "找不到 assets/block_mappings_v1.json，请使用 --assets <dir>");
}

Result<void> initialize_registry(
    water_structure::RuntimeRegistry& registry,
    const std::filesystem::path& executable,
    const std::filesystem::path& configured)
{
    auto assets = locate_assets(executable, configured);
    if (!assets) return Result<void>::failure(assets.error());
    auto loaded = registry.load_block_mappings(assets.value() / "block_mappings_v1.json");
    if (!loaded) return loaded;
    const auto legacy = assets.value() / "bdx_runtimeIds_117.json";
    if (std::filesystem::is_regular_file(legacy)) {
        loaded = registry.load_legacy_pool(legacy, 117);
        if (!loaded) return loaded;
    }
    registry.install_as_bwo_resolver();
    return Result<void>::success();
}

class Progress {
public:
    explicit Progress(bool quiet) : mQuiet(quiet) {}

    ~Progress() { end_busy(); }

    void start(std::size_t total)
    {
        end_busy();
        std::lock_guard lock(mMutex);
        mTotal = total;
        mDone = 0;
        mLastPercent = -1;
        mBegin = Clock::now();
        mIndeterminate = false;
        render(true);
    }

    void stage(std::string_view label, bool indeterminate = false)
    {
        std::lock_guard lock(mMutex);
        mLabel.assign(label);
        mIndeterminate = indeterminate;
        render(true);
    }

    void advance()
    {
        std::lock_guard lock(mMutex);
        ++mDone;
        render(false);
    }

    // Keep the terminal alive while a writer is doing a long blocking pass.
    // Writers which do not expose fine-grained callbacks still get an
    // updating elapsed/ETA line instead of a frozen prompt.
    void begin_busy()
    {
        if (mQuiet || mBusy.exchange(true)) return;
        mStopBusy = false;
        mBusyThread = std::thread([this] {
            while (!mStopBusy.load()) {
                {
                    std::lock_guard lock(mMutex);
                    render(true);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
    }

    void end_busy()
    {
        if (!mBusy.exchange(false)) return;
        mStopBusy = true;
        if (mBusyThread.joinable()) mBusyThread.join();
    }

    void finish()
    {
        end_busy();
        std::lock_guard lock(mMutex);
        if (mQuiet || mTotal == 0) return;
        mIndeterminate = false;
        mDone = mTotal;
        render(true);
        std::cout << '\n';
    }

private:
    void render(bool force)
    {
        if (mQuiet || mTotal == 0) return;
        if (mIndeterminate) {
            static constexpr std::string_view spinner = "-\\|/";
            const auto elapsed = std::chrono::duration<double>(Clock::now() - mBegin).count();
            const auto frame = static_cast<std::size_t>(elapsed * 4.0) % spinner.size();
            std::cout << "\r[" << spinner[frame] << "] " << mLabel
                      << " elapsed " << std::fixed << std::setprecision(1) << elapsed
                      << "s ETA n/a" << std::flush;
            return;
        }
        const auto percent = static_cast<int>(mDone * 100 / mTotal);
        if (!force && percent == mLastPercent) return;
        mLastPercent = percent;
        constexpr int width = 36;
        const auto filled = percent * width / 100;
        std::cout << "\r[";
        for (int i = 0; i < width; ++i) std::cout << (i < filled ? '#' : '-');
        const auto elapsed = std::chrono::duration<double>(Clock::now() - mBegin).count();
        const auto eta = (mDone > 0 && mDone < mTotal)
            ? elapsed * static_cast<double>(mTotal - mDone) / static_cast<double>(mDone)
            : 0.0;
        std::cout << "] " << std::setw(3) << percent << "% " << mDone << '/' << mTotal
                  << " " << mLabel
                  << " elapsed " << std::fixed << std::setprecision(1) << elapsed << "s";
        if (mDone > 0 && mDone < mTotal) {
            std::cout << " ETA " << std::fixed << std::setprecision(1) << eta << "s";
        } else if (mDone == 0) {
            std::cout << " ETA calculating...";
        }
        std::cout << std::flush;
    }

    bool mQuiet = false;
    std::size_t mTotal = 0;
    std::size_t mDone = 0;
    int mLastPercent = -1;
    Clock::time_point mBegin = Clock::now();
    std::mutex mMutex;
    std::atomic_bool mBusy = false;
    std::atomic_bool mStopBusy = false;
    std::thread mBusyThread;
    std::string mLabel = "处理中";
    bool mIndeterminate = false;
};

int list_formats(bool writers_only)
{
    std::cout << std::left << std::setw(18) << "Format"
              << std::setw(9) << "Reader" << std::setw(9) << "Writer"
              << std::setw(14) << "To world" << "Extensions\n";
    for (const auto& format : water_structure::FormatRegistry::formats()) {
        if (writers_only && !format.writer_implemented) continue;
        std::cout << std::left << std::setw(18) << format.name
                  << std::setw(9) << (format.reader_implemented ? "yes" : "pending")
                  << std::setw(9) << (format.writer_implemented ? "yes" : "pending")
                  << std::setw(14) << (format.world_import_implemented ? "yes" : "pending");
        for (std::size_t i = 0; i < format.extensions.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << format.extensions[i];
        }
        std::cout << '\n';
    }
    return 0;
}

int inspect_file(
    const std::filesystem::path& input,
    const std::filesystem::path& executable,
    const CommonOptions& options)
{
    water_structure::RuntimeRegistry registry;
    if (auto initialized = initialize_registry(registry, executable, options.assets); !initialized) {
        std::cerr << "error: " << initialized.error() << '\n';
        return kInputError;
    }
    auto detected = water_structure::FormatRegistry::detect(input);
    if (!detected) { std::cerr << "error: " << detected.error() << '\n'; return kInputError; }
    const auto format = detected.value();
    std::cout << "format: " << format.name << '\n'
              << "path: " << utf8(input) << '\n'
              << "reader: " << (format.reader_implemented ? "yes" : "pending") << '\n'
              << "writer: " << (format.writer_implemented ? "yes" : "pending") << '\n'
              << "world import: " << (format.world_import_implemented ? "yes" : "pending") << '\n'
              << "world export: " << (format.world_export_implemented ? "yes" : "pending") << '\n';
    if (!format.reader_implemented) return 0;
    auto opened = water_structure::FormatRegistry::open(input, registry);
    if (!opened) { std::cerr << "error: " << opened.error() << '\n'; return kInputError; }
    const auto size = opened.value()->size();
    const auto offset = opened.value()->offset();
    auto non_air = opened.value()->count_non_air_blocks();
    if (!non_air) { std::cerr << "error: " << non_air.error() << '\n'; return kInputError; }
    std::size_t entity_count = 0;
    for (std::int32_t z = 0; z < size.chunk_z_count(); ++z) {
        std::vector<water_structure::ChunkPos> row;
        for (std::int32_t x = 0; x < size.chunk_x_count(); ++x) row.push_back({x, z});
        auto entities = opened.value()->get_chunk_nbt(row);
        if (!entities) { std::cerr << "error: " << entities.error() << '\n'; return kInputError; }
        for (const auto& [_, values] : entities.value()) entity_count += values.size();
    }
    std::cout << "size: " << size.width << 'x' << size.height << 'x' << size.length << '\n'
              << "offset: " << offset.x << ',' << offset.y << ',' << offset.z << '\n'
              << "non-air blocks: " << non_air.value() << '\n'
              << "chunks: " << static_cast<std::size_t>(size.chunk_x_count()) * size.chunk_z_count() << '\n'
              << "block entities: " << entity_count << '\n';
    return 0;
}

int convert_file(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    std::optional<StructureId> format,
    std::size_t threads,
    const std::filesystem::path& executable,
    const CommonOptions& common)
{
    if (!std::filesystem::exists(input)) {
        std::cerr << "error: input does not exist: " << utf8(input) << '\n';
        return kInputError;
    }
    if (!format) {
        auto inferred = infer_output_format(output);
        if (!inferred) { std::cerr << "error: " << inferred.error() << '\n'; return kUsageError; }
        format = inferred.value();
    }
    const auto info = std::ranges::find_if(
        water_structure::FormatRegistry::formats(),
        [format](const auto& value) { return value.id == *format; });
    if (info == water_structure::FormatRegistry::formats().end() || !info->writer_implemented) {
        std::cerr << "error: target format has no verified writer: "
                  << water_structure::to_string(*format) << '\n';
        return kUsageError;
    }
    std::error_code directory_error;
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path(), directory_error);
        if (directory_error) {
            std::cerr << "error: cannot create output directory: " << directory_error.message() << '\n';
            return kInputError;
        }
    }
    water_structure::RuntimeRegistry registry;
    if (auto initialized = initialize_registry(registry, executable, common.assets); !initialized) {
        std::cerr << "error: " << initialized.error() << '\n';
        return kInputError;
    }
    const auto begin = Clock::now();
    Progress progress(common.quiet);
    // Conversion writers currently expose no per-block callback.  Treat the
    // operation as two measurable stages (open and write); while the write
    // stage is active the background refresh keeps elapsed time visible and
    // estimates its remaining duration from the completed open stage.
    progress.start(2);
    progress.stage("读取");
    auto opened = water_structure::FormatRegistry::open(input, registry);
    if (!opened) {
        if (!common.quiet) std::cout << '\n';
        std::cerr << "error: " << opened.error() << '\n';
        return kInputError;
    }
    progress.advance();
    progress.stage("写入", true);
    progress.begin_busy();
    auto written = water_structure::FormatRegistry::write(
        *opened.value(), *format, output, registry, {threads, 0});
    progress.end_busy();
    if (!written) {
        if (!common.quiet) std::cout << '\n';
        std::cerr << "error: " << written.error() << '\n';
        return kConversionError;
    }
    progress.finish();
    const auto seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    if (!common.quiet) {
        std::cout << "converted " << opened.value()->name() << " -> "
                  << water_structure::to_string(*format) << '\n'
                  << "output: " << utf8(output) << '\n'
                  << "elapsed: " << std::fixed << std::setprecision(3) << seconds << " s\n";
    }
    return 0;
}

int to_world(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    water_structure::SubChunkPos start,
    const std::filesystem::path& executable,
    const CommonOptions& common)
{
    water_structure::RuntimeRegistry registry;
    if (auto initialized = initialize_registry(registry, executable, common.assets); !initialized) {
        std::cerr << "error: " << initialized.error() << '\n';
        return kInputError;
    }
    auto opened = water_structure::FormatRegistry::open(
        input, registry, {.streaming_world_import = true});
    if (!opened) { std::cerr << "error: " << opened.error() << '\n'; return kInputError; }
    auto world = water_structure::BedrockWorldAdapter::open(output);
    if (!world) { std::cerr << "error: " << world.error() << '\n'; return kInputError; }
    Progress progress(common.quiet);
    progress.stage("写入世界");
    const auto begin = Clock::now();
    auto converted = opened.value()->write_to_world(
        world.value(), start,
        {[&progress](std::size_t total) { progress.start(total); },
         [&progress]() { progress.advance(); }});
    if (!converted) { std::cerr << "\nerror: " << converted.error() << '\n'; return kConversionError; }
    auto closed = world.value().close();
    if (!closed) { std::cerr << "\nerror: " << closed.error() << '\n'; return kConversionError; }
    progress.finish();
    if (!common.quiet) {
        std::cout << "converted " << opened.value()->name() << " -> MCWorld\n"
                  << "output: " << utf8(output) << '\n'
                  << "elapsed: " << std::fixed << std::setprecision(3)
                  << std::chrono::duration<double>(Clock::now() - begin).count() << " s\n";
    }
    return 0;
}

std::string prompt(std::string_view message, std::string_view default_value = {})
{
    std::cout << message;
    if (!default_value.empty()) std::cout << " [" << default_value << ']';
    std::cout << ": " << std::flush;
    std::string value;
    if (!std::getline(std::cin, value)) return {};
    value = trim(std::move(value));
    return value.empty() ? std::string(default_value) : value;
}

std::filesystem::path default_output_path(
    const std::filesystem::path& input,
    const water_structure::FormatInfo* format)
{
    auto result = input.parent_path() / input.stem();
    if (format == nullptr) {
        result += ".mcworld";
    } else if (!format->extensions.empty()) {
        result += format->extensions.front();
    } else {
        result += ".out";
    }
    return result;
}

int interactive(const std::filesystem::path& executable)
{
#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "========================================\n"
                 " WaterStructureCpp 结构转换器\n"
                 "========================================\n"
                 "输入 q 可随时退出。路径可以直接拖入窗口。\n\n";

    for (;;) {

    const auto source_text = prompt("请输入源结构文件路径");
    if (source_text.empty() || lower(source_text) == "q") return 0;
    const auto source = path_from_utf8(source_text);
    if (!std::filesystem::exists(source)) {
        std::cerr << "错误：源文件不存在：" << utf8(source) << '\n';
        return kInputError;
    }
    auto detected = water_structure::FormatRegistry::detect(source);
    if (!detected) {
        std::cerr << "错误：" << detected.error() << '\n';
        return kInputError;
    }
    std::cout << "检测到源格式：" << detected.value().name << "\n\n";

    std::vector<const water_structure::FormatInfo*> targets;
    std::cout << "请选择目标格式：\n"
                 "  0. MCWorld（写入世界目录或 .mcworld）\n";
    for (const auto& format : water_structure::FormatRegistry::formats()) {
        if (!format.writer_implemented) continue;
        targets.push_back(&format);
        std::cout << "  " << targets.size() << ". " << format.name;
        if (!format.extensions.empty()) {
            std::cout << " (";
            for (std::size_t index = 0; index < format.extensions.size(); ++index) {
                if (index) std::cout << ", ";
                std::cout << format.extensions[index];
            }
            std::cout << ')';
        }
        std::cout << '\n';
    }

    const auto target_text = prompt("输入序号或格式名称");
    if (target_text.empty() || lower(target_text) == "q") return 0;
    bool target_world = lower(target_text) == "mcworld" || target_text == "0";
    const water_structure::FormatInfo* target = nullptr;
    if (!target_world) {
        if (const auto number = parse_size(target_text);
            number && *number >= 1 && *number <= targets.size()) {
            target = targets[*number - 1];
        } else if (const auto id = parse_format(target_text)) {
            const auto found = std::ranges::find_if(targets, [id](const auto* value) {
                return value->id == *id;
            });
            if (found != targets.end()) target = *found;
        }
        if (target == nullptr) {
            std::cerr << "错误：不支持的目标格式：" << target_text << '\n';
            return kUsageError;
        }
    }

    const auto suggested = default_output_path(source, target);
    const auto output_text = prompt(
        target_world ? "请输入已有世界目录或 .mcworld 路径" : "请输入输出文件路径",
        utf8(suggested));
    if (output_text.empty() || lower(output_text) == "q") return 0;
    const auto output = path_from_utf8(output_text);

    CommonOptions common;
    if (target_world) {
        if (!std::filesystem::exists(output)) {
            std::cerr << "错误：目标世界必须已经存在。请提供包含 level.dat/db 的世界目录，"
                         "或一个可写回的 .mcworld 模板。\n";
            return kInputError;
        }
        auto start = water_structure::SubChunkPos{0, -4, 0};
        const auto start_text = prompt("目标 subchunk 坐标 x,y,z", "0,-4,0");
        if (lower(start_text) == "q") return 0;
        if (const auto parsed = parse_start(start_text)) start = *parsed;
        else {
            std::cerr << "错误：坐标格式无效，应为 x,y,z\n";
            return kUsageError;
        }
        std::cout << "\n正在写入世界……\n";
        const auto result = to_world(source, output, start, executable, common);
        if (result == 0) std::cout << "本次转换完成，可以继续处理其他文件。\n\n";
        else std::cerr << "本次转换失败，可以继续处理其他文件。\n\n";
        continue;
    }

    std::size_t threads = 0;
    const auto threads_text = prompt("编码线程数（0 为自动）", "0");
    if (lower(threads_text) == "q") return 0;
    if (const auto parsed = parse_size(threads_text)) threads = *parsed;
    else {
        std::cerr << "错误：线程数无效\n";
        return kUsageError;
    }
    std::cout << "\n正在转换 " << detected.value().name << " -> " << target->name << "……\n";
    const auto result = convert_file(source, output, target->id, threads, executable, common);
    if (result == 0) std::cout << "本次转换完成，可以继续处理其他文件。\n\n";
    else std::cerr << "本次转换失败，可以继续处理其他文件。\n\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2) return interactive(argv[0]);
        const std::string command = lower(argv[1]);
        if (command == "help" || command == "--help" || command == "-h") {
            print_usage();
            return 0;
        }
        if (command == "formats") {
            bool writers_only = false;
            for (int i = 2; i < argc; ++i) {
                if (std::string_view(argv[i]) == "--writers-only") writers_only = true;
                else { std::cerr << "error: unknown option: " << argv[i] << '\n'; return kUsageError; }
            }
            return list_formats(writers_only);
        }

        CommonOptions common;
        std::vector<std::filesystem::path> positional;
        std::optional<StructureId> format;
        std::filesystem::path explicit_output;
        std::size_t threads = 0;
        water_structure::SubChunkPos start{0, -4, 0};
        for (int i = 2; i < argc; ++i) {
            const std::string_view argument = argv[i];
            std::string_view value;
            if (argument == "--assets") {
                if (!take_option_value(argc, argv, i, value)) { std::cerr << "error: --assets needs a value\n"; return kUsageError; }
                common.assets = value;
            } else if (argument == "--profile") {
                common.profile = true;
            } else if (argument == "--quiet" || argument == "-q") {
                common.quiet = true;
            } else if (argument == "--format" || argument == "-f") {
                if (!take_option_value(argc, argv, i, value)) { std::cerr << "error: --format needs a value\n"; return kUsageError; }
                format = parse_format(value);
                if (!format) { std::cerr << "error: unknown format: " << value << '\n'; return kUsageError; }
            } else if (argument == "--output" || argument == "-o") {
                if (!take_option_value(argc, argv, i, value)) { std::cerr << "error: --output needs a value\n"; return kUsageError; }
                explicit_output = value;
            } else if (argument == "--threads" || argument == "-j") {
                if (!take_option_value(argc, argv, i, value)) { std::cerr << "error: --threads needs a value\n"; return kUsageError; }
                const auto parsed = parse_size(value);
                if (!parsed) { std::cerr << "error: invalid thread count: " << value << '\n'; return kUsageError; }
                threads = *parsed;
            } else if (argument == "--start") {
                if (!take_option_value(argc, argv, i, value)) { std::cerr << "error: --start needs x,y,z\n"; return kUsageError; }
                const auto parsed = parse_start(value);
                if (!parsed) { std::cerr << "error: invalid start position: " << value << '\n'; return kUsageError; }
                start = *parsed;
            } else if (argument == "--help" || argument == "-h") {
                print_usage();
                return 0;
            } else if (!argument.empty() && argument.front() == '-') {
                std::cerr << "error: unknown option: " << argument << '\n';
                return kUsageError;
            } else {
                positional.emplace_back(argument);
            }
        }

        if (common.profile) {
#if defined(_WIN32)
            _putenv_s("WATER_STRUCTURE_PROFILE", "1");
#else
            setenv("WATER_STRUCTURE_PROFILE", "1", 1);
#endif
        }
        const std::filesystem::path executable = argv[0];
        if (command == "inspect" && positional.size() == 1) {
            return inspect_file(positional[0], executable, common);
        }
        if (command == "convert" && (positional.size() == 1 || positional.size() == 2)) {
            const auto output = positional.size() == 2 ? positional[1] : explicit_output;
            if (output.empty()) { std::cerr << "error: output path is required\n"; return kUsageError; }
            return convert_file(positional[0], output, format, threads, executable, common);
        }
        if (command == "to-world" && positional.size() == 2) {
            return to_world(positional[0], positional[1], start, executable, common);
        }
        print_usage(std::cerr);
        return kUsageError;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return kConversionError;
    }
}
