#include "mcfunction.hpp"
#include "../core/bounded_thread_pool.hpp"

#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/world.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace water_structure {
namespace {

// MCFunction keeps a compact command list so that fill cuboids can be
// replayed per requested chunk.  The list is deliberately bounded: without a
// limit an adversarial file containing millions of tiny commands could make
// the parser grow until the process is killed before the caller can report an
// error.  The limit leaves room for the command index and the bounded parser
// buffers under the 500 MiB test-process budget.
constexpr std::size_t kMaxCommands = 8'000'000;
constexpr std::size_t kMaxLineBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxStateProperties = 128;
// A dense CSR index is an acceleration structure, not semantic data. Keep its
// worst-case footprint bounded so an eight-million-command file remains below
// the 500 MiB child-process ceiling; commands outside this budget use the
// ordered broad fallback.
constexpr std::size_t kMaxIndexEntries = 8'000'000;
constexpr std::size_t kMaxRuntimeCacheEntries = 65'536;

std::string_view trim_view(std::string_view value) noexcept
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto l = static_cast<unsigned char>(left[index]);
        const auto r = static_cast<unsigned char>(right[index]);
        if (static_cast<unsigned char>(std::tolower(l)) !=
            static_cast<unsigned char>(std::tolower(r))) return false;
    }
    return true;
}

std::optional<std::string_view> next_field(
    std::string_view line,
    std::size_t& offset) noexcept
{
    while (offset < line.size() &&
           std::isspace(static_cast<unsigned char>(line[offset]))) ++offset;
    if (offset == line.size()) return std::nullopt;
    const auto begin = offset;
    while (offset < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[offset]))) ++offset;
    return line.substr(begin, offset - begin);
}

struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }
    std::size_t operator()(const std::string& value) const noexcept
    {
        return operator()(std::string_view(value));
    }
};

struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view left, std::string_view right) const noexcept
    {
        return left == right;
    }
};

std::optional<std::int32_t> coordinate(std::string_view token)
{
    if (token.empty()) return std::nullopt;
    if (token.front() == '~') token.remove_prefix(1);
    if (token.empty()) return 0;
    std::int32_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return std::nullopt;
    return value;
}

std::optional<std::vector<BlockStateProperty>> states(std::string_view text)
{
    if (text.empty()) return std::vector<BlockStateProperty>{};
    if (text.front() != '[' || text.back() != ']') return std::nullopt;
    text.remove_prefix(1); text.remove_suffix(1);
    std::vector<BlockStateProperty> result;
    std::size_t begin = 0;
    std::size_t property_count = 0;
    while (begin <= text.size()) {
        auto comma = std::string_view::npos;
        bool quoted = false;
        bool escaped = false;
        for (auto index = begin; index < text.size(); ++index) {
            if (quoted && escaped) {
                escaped = false;
                continue;
            }
            if (quoted && text[index] == '\\') {
                escaped = true;
                continue;
            }
            if (text[index] == '"') quoted = !quoted;
            if (text[index] == ',' && !quoted) {
                comma = index;
                break;
            }
        }
        if (quoted || escaped) return std::nullopt;
        const auto part = text.substr(
            begin,
            comma == std::string_view::npos ? text.size() - begin : comma - begin);
        if (!part.empty()) {
            if (++property_count > kMaxStateProperties) return std::nullopt;
            const auto equal = part.find('=');
            if (equal == std::string_view::npos || equal == 0) return std::nullopt;
            auto name = std::string(part.substr(0, equal));
            auto value = std::string(part.substr(equal + 1));
            auto trim = [](std::string& s) {
                const auto first = s.find_first_not_of(" \t\r\n\"");
                const auto last = s.find_last_not_of(" \t\r\n\"");
                s = first == std::string::npos ? std::string{} : s.substr(first, last - first + 1);
            };
            trim(name); trim(value);
            if (name.empty() || value.empty()) return std::nullopt;
            BlockStateProperty property;
            property.name = std::move(name);
            property.value = std::move(value);
            if (property.value == "true" || property.value == "false") {
                property.type = BlockStateValueType::Byte;
                property.value = property.value == "true" ? "1" : "0";
            }
            else {
                std::int32_t numeric = 0;
                const auto parsed = std::from_chars(property.value.data(), property.value.data() + property.value.size(), numeric);
                property.type = parsed.ec == std::errc{} && parsed.ptr == property.value.data() + property.value.size()
                    ? BlockStateValueType::Int : BlockStateValueType::String;
            }
            result.push_back(std::move(property));
        }
        if (comma == std::string_view::npos) break;
        begin = comma + 1;
    }
    return result;
}

} // namespace

void McFunctionStructure::set_offset(BlockPos offset) noexcept
{
    mOffset = offset;
    mCommandOffsets.clear();
    mCommandIndices.clear();
    mBroadCommands.clear();
    mCommandIndexReady = false;
    const auto expanded = [](std::int32_t base, std::int32_t delta) noexcept {
        const auto magnitude = delta < 0 ? -static_cast<std::int64_t>(delta)
                                         : static_cast<std::int64_t>(delta);
        const auto value = static_cast<std::int64_t>(base) + magnitude;
        return static_cast<std::int32_t>(std::min<std::int64_t>(
            value, std::numeric_limits<std::int32_t>::max()));
    };
    mSize = { expanded(mOriginalSize.width, offset.x),
        expanded(mOriginalSize.height, offset.y),
        expanded(mOriginalSize.length, offset.z) };
}

Result<void> McFunctionStructure::read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 MCFunction 文件: " + path.string());
    std::vector<Command> commands;
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (!size_error &&
        (file_size >= 32 * 1024 * 1024 ||
         std::getenv("WATER_STRUCTURE_MCFUNCTION_PARSE_THREADS") != nullptr)) {
        return read_parallel(path);
    }
    const auto estimated_commands = size_error
        ? static_cast<std::uintmax_t>(4096)
        : std::min<std::uintmax_t>(file_size / 64, 1024 * 1024);
    commands.reserve(static_cast<std::size_t>(
        std::max<std::uintmax_t>(4096, estimated_commands)));
    std::int32_t min_x = std::numeric_limits<std::int32_t>::max(), min_y = min_x, min_z = min_x;
    std::int32_t max_x = std::numeric_limits<std::int32_t>::min(), max_y = max_x, max_z = max_x;
    std::string line;
    std::size_t line_number = 0;
    std::unordered_map<std::string, std::uint32_t,
        TransparentStringHash, TransparentStringEqual> runtime_cache;
    runtime_cache.reserve(1024);
    auto resolve = [&](std::string_view name, std::string_view state_text) -> Result<std::uint32_t> {
        std::string owned_key;
        std::string_view lookup_key = name;
        if (!state_text.empty()) {
            if (name.data() + name.size() == state_text.data()) {
                lookup_key = std::string_view(
                    name.data(), name.size() + state_text.size());
            } else {
                owned_key.reserve(name.size() + state_text.size());
                owned_key.append(name);
                owned_key.append(state_text);
                lookup_key = owned_key;
            }
        }
        if (const auto cached = runtime_cache.find(lookup_key);
            cached != runtime_cache.end()) {
            return Result<std::uint32_t>::success(cached->second);
        }
        auto parsed_states = states(state_text);
        if (!parsed_states) return Result<std::uint32_t>::failure("第 " + std::to_string(line_number) + " 行: 状态格式无效");
        // MCFunction is Bedrock by default. Resolve the decoded Bedrock state
        // directly and only use Java compatibility as a legacy fallback.
        auto runtime = mRegistry.find(name, *parsed_states);
        if (!runtime && !parsed_states->empty()) {
            runtime = mRegistry.legacy_state_runtime_id(name, *parsed_states);
        }
        if (!runtime && parsed_states->empty()) {
            runtime = mRegistry.legacy_runtime_id(name, 0);
        }
        if (!runtime) runtime = mRegistry.compatible_java_runtime_id(lookup_key);
        const auto resolved = runtime.value_or(
            mRegistry.find("minecraft:unknown").value_or(mRegistry.air_runtime_id()));
        if (runtime_cache.size() < kMaxRuntimeCacheEntries) {
            runtime_cache.emplace(
                owned_key.empty() ? std::string(lookup_key) : std::move(owned_key),
                resolved);
        }
        return Result<std::uint32_t>::success(resolved);
    };
    auto add = [&](std::int32_t x1, std::int32_t y1, std::int32_t z1,
                   std::int32_t x2, std::int32_t y2, std::int32_t z2,
                   std::string_view name, std::string_view state_text) -> Result<void> {
        if (commands.size() >= kMaxCommands) {
            return Result<void>::failure(
                "MCFunction 命令数量超过限制 " + std::to_string(kMaxCommands));
        }
        auto runtime = resolve(name, state_text);
        if (!runtime) return Result<void>::failure(runtime.error());
        const auto command = Command{
            std::min(x1, x2), std::max(x1, x2),
            std::min(y1, y2), std::max(y1, y2),
            std::min(z1, z2), std::max(z1, z2), runtime.value()
        };
        commands.push_back(command);
        min_x = std::min(min_x, command.x1); min_y = std::min(min_y, command.y1); min_z = std::min(min_z, command.z1);
        max_x = std::max(max_x, command.x2); max_y = std::max(max_y, command.y2); max_z = std::max(max_z, command.z2);
        return Result<void>::success();
    };
    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > kMaxLineBytes) {
            return Result<void>::failure(
                "第 " + std::to_string(line_number) + " 行超过 " +
                std::to_string(kMaxLineBytes) + " bytes 限制");
        }
        const auto view = trim_view(line);
        if (view.empty() || view.front() == '#') continue;
        std::size_t offset = 0;
        const auto command = next_field(view, offset);
        if (!command) continue;
        const bool fill = ascii_iequals(*command, "fill");
        if (!fill && !ascii_iequals(*command, "setblock")) continue;

        std::array<std::optional<std::int32_t>, 6> coordinates{};
        const auto coordinate_count = fill ? std::size_t{6} : std::size_t{3};
        bool fields_complete = true;
        for (std::size_t index = 0; index < coordinate_count; ++index) {
            const auto token = next_field(view, offset);
            if (!token) {
                fields_complete = false;
                break;
            }
            coordinates[index] = coordinate(*token);
        }
        if (!fields_complete) {
            return Result<void>::failure(
                "第 " + std::to_string(line_number) + " 行: 命令参数不完整");
        }
        for (std::size_t index = 0; index < coordinate_count; ++index) {
            if (!coordinates[index]) {
                return Result<void>::failure(
                    "第 " + std::to_string(line_number) + " 行: 坐标无效");
            }
        }

        while (offset < view.size() &&
               std::isspace(static_cast<unsigned char>(view[offset]))) ++offset;
        if (offset == view.size()) {
            return Result<void>::failure(
                "第 " + std::to_string(line_number) + " 行: 缺少方块名");
        }
        const auto name_begin = offset;
        while (offset < view.size() && view[offset] != '[' &&
               !std::isspace(static_cast<unsigned char>(view[offset]))) ++offset;
        const auto block_name = view.substr(name_begin, offset - name_begin);
        if (block_name.empty()) {
            return Result<void>::failure(
                "第 " + std::to_string(line_number) + " 行: 缺少方块名");
        }
        while (offset < view.size() &&
               std::isspace(static_cast<unsigned char>(view[offset]))) ++offset;
        std::string_view state_text;
        if (offset < view.size() && view[offset] == '[') {
            const auto state_begin = offset;
            bool quoted = false;
            bool escaped = false;
            for (++offset; offset < view.size(); ++offset) {
                if (quoted && escaped) {
                    escaped = false;
                    continue;
                }
                if (quoted && view[offset] == '\\') {
                    escaped = true;
                    continue;
                }
                if (view[offset] == '"') quoted = !quoted;
                if (view[offset] == ']' && !quoted) {
                    ++offset;
                    state_text = view.substr(state_begin, offset - state_begin);
                    break;
                }
            }
            if (state_text.empty() || quoted || escaped) {
                return Result<void>::failure(
                    "第 " + std::to_string(line_number) + " 行: 状态格式无效");
            }
        }
        if (fill) {
            auto result = add(
                *coordinates[0], *coordinates[1], *coordinates[2],
                *coordinates[3], *coordinates[4], *coordinates[5],
                block_name, state_text);
            if (!result) return result;
        } else {
            auto result = add(
                *coordinates[0], *coordinates[1], *coordinates[2],
                *coordinates[0], *coordinates[1], *coordinates[2],
                block_name, state_text);
            if (!result) return result;
        }
    }
    if (commands.empty()) return Result<void>::failure("MCFunction 不包含 setblock 或 fill 命令");
    mCommands = std::move(commands);
    mCommandOffsets.clear();
    mCommandIndices.clear();
    mBroadCommands.clear();
    mCommandIndexReady = false;
    const auto dimension = [&](std::int32_t minimum, std::int32_t maximum,
                               std::string_view axis) -> Result<std::int32_t> {
        const auto span = static_cast<std::int64_t>(maximum) - minimum + 1;
        if (span <= 0 || span > std::numeric_limits<std::int32_t>::max()) {
            return Result<std::int32_t>::failure(
                "MCFunction " + std::string(axis) + " 尺寸超出 int32 范围");
        }
        return Result<std::int32_t>::success(static_cast<std::int32_t>(span));
    };
    const auto width = dimension(min_x, max_x, "X");
    const auto height = dimension(min_y, max_y, "Y");
    const auto length = dimension(min_z, max_z, "Z");
    if (!width || !height || !length) {
        return Result<void>::failure(
            !width ? width.error() : (!height ? height.error() : length.error()));
    }
    mOriginalSize = { width.value(), height.value(), length.value() };
    mNonAirBlocks.reset();
    // Normalize to the same local origin used by all structure readers.
    for (auto& command : mCommands) {
        command.x1 -= min_x; command.x2 -= min_x;
        command.y1 -= min_y; command.y2 -= min_y;
        command.z1 -= min_z; command.z2 -= min_z;
    }
    set_offset({});
    return Result<void>::success();
}

Result<void> McFunctionStructure::read_parallel(const std::filesystem::path& path)
{
    using RuntimeCache = std::unordered_map<std::string, std::uint32_t,
        TransparentStringHash, TransparentStringEqual>;
    struct Bounds {
        std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_y = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_z = std::numeric_limits<std::int32_t>::max();
        std::int32_t max_x = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_y = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_z = std::numeric_limits<std::int32_t>::min();

        void include(const Command& command) noexcept
        {
            min_x = std::min(min_x, command.x1);
            min_y = std::min(min_y, command.y1);
            min_z = std::min(min_z, command.z1);
            max_x = std::max(max_x, command.x2);
            max_y = std::max(max_y, command.y2);
            max_z = std::max(max_z, command.z2);
        }
        void include(const Bounds& other) noexcept
        {
            if (other.min_x == std::numeric_limits<std::int32_t>::max()) return;
            min_x = std::min(min_x, other.min_x);
            min_y = std::min(min_y, other.min_y);
            min_z = std::min(min_z, other.min_z);
            max_x = std::max(max_x, other.max_x);
            max_y = std::max(max_y, other.max_y);
            max_z = std::max(max_z, other.max_z);
        }
    };
    struct BatchResult {
        std::vector<Command> commands;
        Bounds bounds;
        std::string error;
        std::size_t source_bytes = 0;
    };

    const auto parse_line = [this](
        std::string_view raw_line,
        std::size_t line_number,
        RuntimeCache& runtime_cache,
        BatchResult& batch) -> Result<void> {
        if (raw_line.size() > kMaxLineBytes) {
            return Result<void>::failure(
                "第 " + std::to_string(line_number) + " 行超过 " +
                std::to_string(kMaxLineBytes) + " bytes 限制");
        }
        const auto view = trim_view(raw_line);
        if (view.empty() || view.front() == '#') return Result<void>::success();
        std::size_t offset = 0;
        const auto command_name = next_field(view, offset);
        if (!command_name) return Result<void>::success();
        const bool fill = ascii_iequals(*command_name, "fill");
        if (!fill && !ascii_iequals(*command_name, "setblock")) {
            return Result<void>::success();
        }

        std::array<std::optional<std::int32_t>, 6> coordinates{};
        const auto coordinate_count = fill ? std::size_t{6} : std::size_t{3};
        for (std::size_t index = 0; index < coordinate_count; ++index) {
            const auto token = next_field(view, offset);
            if (!token) {
                return Result<void>::failure(
                    "第 " + std::to_string(line_number) + " 行: 命令参数不完整");
            }
            coordinates[index] = coordinate(*token);
            if (!coordinates[index]) {
                return Result<void>::failure(
                    "第 " + std::to_string(line_number) + " 行: 坐标无效");
            }
        }
        while (offset < view.size() &&
               std::isspace(static_cast<unsigned char>(view[offset]))) ++offset;
        if (offset == view.size()) {
            return Result<void>::failure(
                "第 " + std::to_string(line_number) + " 行: 缺少方块名");
        }
        const auto name_begin = offset;
        while (offset < view.size() && view[offset] != '[' &&
               !std::isspace(static_cast<unsigned char>(view[offset]))) ++offset;
        const auto block_name = view.substr(name_begin, offset - name_begin);
        if (block_name.empty()) {
            return Result<void>::failure(
                "第 " + std::to_string(line_number) + " 行: 缺少方块名");
        }
        while (offset < view.size() &&
               std::isspace(static_cast<unsigned char>(view[offset]))) ++offset;
        std::string_view state_text;
        if (offset < view.size() && view[offset] == '[') {
            const auto state_begin = offset;
            bool quoted = false;
            bool escaped = false;
            for (++offset; offset < view.size(); ++offset) {
                if (quoted && escaped) {
                    escaped = false;
                    continue;
                }
                if (quoted && view[offset] == '\\') {
                    escaped = true;
                    continue;
                }
                if (view[offset] == '"') quoted = !quoted;
                if (view[offset] == ']' && !quoted) {
                    ++offset;
                    state_text = view.substr(state_begin, offset - state_begin);
                    break;
                }
            }
            if (state_text.empty() || quoted || escaped) {
                return Result<void>::failure(
                    "第 " + std::to_string(line_number) + " 行: 状态格式无效");
            }
        }

        std::string owned_key;
        std::string_view lookup_key = block_name;
        if (!state_text.empty()) {
            if (block_name.data() + block_name.size() == state_text.data()) {
                lookup_key = std::string_view(
                    block_name.data(), block_name.size() + state_text.size());
            } else {
                owned_key.reserve(block_name.size() + state_text.size());
                owned_key.append(block_name);
                owned_key.append(state_text);
                lookup_key = owned_key;
            }
        }
        std::uint32_t runtime_id = 0;
        if (const auto cached = runtime_cache.find(lookup_key);
            cached != runtime_cache.end()) {
            runtime_id = cached->second;
        } else {
            auto parsed_states = states(state_text);
            if (!parsed_states) {
                return Result<void>::failure(
                    "第 " + std::to_string(line_number) + " 行: 状态格式无效");
            }
            auto runtime = mRegistry.find(block_name, *parsed_states);
            if (!runtime && !parsed_states->empty()) {
                runtime = mRegistry.legacy_state_runtime_id(block_name, *parsed_states);
            }
            if (!runtime && parsed_states->empty()) {
                runtime = mRegistry.legacy_runtime_id(block_name, 0);
            }
            if (!runtime) runtime = mRegistry.compatible_java_runtime_id(lookup_key);
            runtime_id = runtime.value_or(
                mRegistry.find("minecraft:unknown").value_or(mRegistry.air_runtime_id()));
            if (runtime_cache.size() < kMaxRuntimeCacheEntries) {
                runtime_cache.emplace(
                    owned_key.empty() ? std::string(lookup_key) : std::move(owned_key),
                    runtime_id);
            }
        }

        Command command{};
        if (fill) {
            command = {
                std::min(*coordinates[0], *coordinates[3]),
                std::max(*coordinates[0], *coordinates[3]),
                std::min(*coordinates[1], *coordinates[4]),
                std::max(*coordinates[1], *coordinates[4]),
                std::min(*coordinates[2], *coordinates[5]),
                std::max(*coordinates[2], *coordinates[5]),
                runtime_id
            };
        } else {
            command = {
                *coordinates[0], *coordinates[0],
                *coordinates[1], *coordinates[1],
                *coordinates[2], *coordinates[2],
                runtime_id
            };
        }
        if (batch.commands.size() >= kMaxCommands) {
            return Result<void>::failure(
                "MCFunction 单批命令数量超过限制 " + std::to_string(kMaxCommands));
        }
        batch.bounds.include(command);
        batch.commands.push_back(command);
        return Result<void>::success();
    };

    const auto parse_batch = [&parse_line](
        std::string data,
        std::size_t first_line,
        RuntimeCache& runtime_cache) {
        BatchResult batch;
        batch.source_bytes = data.size();
        batch.commands.reserve(std::max<std::size_t>(data.size() / 64, 256));
        std::size_t offset = 0;
        std::size_t line_number = first_line;
        while (offset < data.size()) {
            const auto newline = data.find('\n', offset);
            const auto end = newline == std::string::npos ? data.size() : newline;
            const auto parsed = parse_line(
                std::string_view(data).substr(offset, end - offset),
                line_number,
                runtime_cache,
                batch);
            if (!parsed) {
                batch.error = parsed.error();
                break;
            }
            ++line_number;
            if (newline == std::string::npos) break;
            offset = newline + 1;
        }
        return batch;
    };

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return Result<void>::failure(
            "无法读取 MCFunction 文件大小: " + size_error.message());
    }
    auto worker_count = std::min<std::size_t>(
        std::max(1u, std::thread::hardware_concurrency()), 4);
    if (const auto* configured = std::getenv("WATER_STRUCTURE_MCFUNCTION_PARSE_THREADS")) {
        std::size_t requested = 0;
        const std::string_view text(configured);
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), requested);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
            requested != 0) {
            worker_count = std::clamp<std::size_t>(requested, 1, 32);
        }
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 MCFunction 文件: " + path.string());
    std::vector<RuntimeCache> runtime_caches(worker_count);
    for (auto& cache : runtime_caches) cache.reserve(1024);
    detail::BoundedThreadPool pool(worker_count, worker_count * 2);
    std::deque<std::future<BatchResult>> pending;
    Bounds bounds;
    mCommands.clear();
    bool reserved = false;

    const auto consume_front = [&]() -> Result<void> {
        auto batch = pending.front().get();
        pending.pop_front();
        if (!batch.error.empty()) return Result<void>::failure(std::move(batch.error));
        if (mCommands.size() > kMaxCommands - batch.commands.size()) {
            return Result<void>::failure(
                "MCFunction 命令数量超过限制 " + std::to_string(kMaxCommands));
        }
        if (!reserved && batch.source_bytes != 0 && !batch.commands.empty()) {
            const auto estimate = static_cast<std::uintmax_t>(
                static_cast<long double>(batch.commands.size()) *
                static_cast<long double>(file_size) /
                static_cast<long double>(batch.source_bytes) * 1.05L);
            const auto maximum = file_size / 16;
            mCommands.reserve(static_cast<std::size_t>(std::min<std::uintmax_t>(
                std::min(estimate, maximum), kMaxCommands)));
            reserved = true;
        }
        bounds.include(batch.bounds);
        mCommands.insert(
            mCommands.end(),
            std::make_move_iterator(batch.commands.begin()),
            std::make_move_iterator(batch.commands.end()));
        return Result<void>::success();
    };

    constexpr std::size_t kReadBlockSize = 16 * 1024 * 1024;
    std::vector<char> read_buffer(kReadBlockSize);
    std::string carry;
    std::size_t next_line = 1;
    const auto submit = [&](std::string data, std::size_t first_line) {
        pending.push_back(pool.submit_indexed(
            [&, data = std::move(data), first_line](std::size_t worker_index) mutable {
                return parse_batch(
                    std::move(data), first_line, runtime_caches[worker_index]);
            }));
    };

    while (input) {
        input.read(read_buffer.data(), static_cast<std::streamsize>(read_buffer.size()));
        const auto read_size = static_cast<std::size_t>(input.gcount());
        if (read_size == 0) break;
        std::string data;
        if (carry.size() > kMaxLineBytes) {
            return Result<void>::failure(
                "MCFunction 行缓冲超过 " + std::to_string(kMaxLineBytes) + " bytes 限制");
        }
        data.reserve(carry.size() + read_size);
        data.append(carry);
        data.append(read_buffer.data(), read_size);
        carry.clear();
        const auto last_newline = data.find_last_of('\n');
        if (last_newline == std::string::npos) {
            if (data.size() > kMaxLineBytes) {
                return Result<void>::failure(
                    "MCFunction 行缓冲超过 " + std::to_string(kMaxLineBytes) + " bytes 限制");
            }
            carry = std::move(data);
            continue;
        }
        if (data.size() - last_newline - 1 > kMaxLineBytes) {
            return Result<void>::failure(
                "MCFunction 行缓冲超过 " + std::to_string(kMaxLineBytes) + " bytes 限制");
        }
        carry.assign(data.data() + last_newline + 1, data.size() - last_newline - 1);
        data.resize(last_newline + 1);
        const auto line_count = static_cast<std::size_t>(
            std::count(data.begin(), data.end(), '\n'));
        submit(std::move(data), next_line);
        next_line += line_count;
        if (pending.size() >= worker_count * 2) {
            if (const auto consumed = consume_front(); !consumed) return consumed;
        }
    }
    if (input.bad()) {
        return Result<void>::failure("读取 MCFunction 文件失败: " + path.string());
    }
    if (!carry.empty()) submit(std::move(carry), next_line);
    while (!pending.empty()) {
        if (const auto consumed = consume_front(); !consumed) return consumed;
    }
    if (mCommands.empty()) {
        return Result<void>::failure("MCFunction 不包含 setblock 或 fill 命令");
    }

    if (mCommands.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Result<void>::failure("MCFunction 命令数量超过索引容量");
    }

    const auto dimension = [&](std::int32_t minimum, std::int32_t maximum,
                               std::string_view axis) -> Result<std::int32_t> {
        const auto span = static_cast<std::int64_t>(maximum) - minimum + 1;
        if (span <= 0 || span > std::numeric_limits<std::int32_t>::max()) {
            return Result<std::int32_t>::failure(
                "MCFunction " + std::string(axis) + " 尺寸超出 int32 范围");
        }
        return Result<std::int32_t>::success(static_cast<std::int32_t>(span));
    };
    const auto width = dimension(bounds.min_x, bounds.max_x, "X");
    const auto height = dimension(bounds.min_y, bounds.max_y, "Y");
    const auto length = dimension(bounds.min_z, bounds.max_z, "Z");
    if (!width || !height || !length) {
        return Result<void>::failure(
            !width ? width.error() : (!height ? height.error() : length.error()));
    }

    const auto normalize_workers = std::min(worker_count, mCommands.size());
    std::vector<std::future<void>> normalize_tasks;
    normalize_tasks.reserve(normalize_workers);
    for (std::size_t worker = 0; worker < normalize_workers; ++worker) {
        const auto begin = mCommands.size() * worker / normalize_workers;
        const auto end = mCommands.size() * (worker + 1) / normalize_workers;
        normalize_tasks.push_back(pool.submit([&, begin, end] {
            for (auto index = begin; index < end; ++index) {
                auto& command = mCommands[index];
                command.x1 -= bounds.min_x; command.x2 -= bounds.min_x;
                command.y1 -= bounds.min_y; command.y2 -= bounds.min_y;
                command.z1 -= bounds.min_z; command.z2 -= bounds.min_z;
            }
        }));
    }
    for (auto& task : normalize_tasks) task.get();

    mCommandOffsets.clear();
    mCommandIndices.clear();
    mBroadCommands.clear();
    mCommandIndexReady = false;
    mOriginalSize = { width.value(), height.value(), length.value() };
    mNonAirBlocks.reset();
    set_offset({});
    return Result<void>::success();
}

Result<ChunkMap> McFunctionStructure::get_chunks(std::span<const ChunkPos> positions) const
{
    ChunkMap result;
    const auto air = mRegistry.air_runtime_id();
    for (const auto pos : positions) result.emplace(pos, ChunkData{});
    if (!mCommandIndexReady) {
        const std::scoped_lock index_lock(mCommandIndexMutex);
        if (!mCommandIndexReady) {
        if (mCommands.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Result<ChunkMap>::failure("MCFunction 命令数量超过索引容量");
        }
        mCommandOffsets.clear();
        mCommandIndices.clear();
        mBroadCommands.clear();
        mIndexMinX = floor_div(mOffset.x, 16);
        mIndexMinZ = floor_div(mOffset.z, 16);
        const auto index_max_x = floor_div64(
            static_cast<std::int64_t>(mOffset.x) + std::max(mOriginalSize.width - 1, 0), 16);
        const auto index_max_z = floor_div64(
            static_cast<std::int64_t>(mOffset.z) + std::max(mOriginalSize.length - 1, 0), 16);
        const auto width64 = static_cast<std::int64_t>(index_max_x) -
            static_cast<std::int64_t>(mIndexMinX) + 1;
        const auto length64 = static_cast<std::int64_t>(index_max_z) -
            static_cast<std::int64_t>(mIndexMinZ) + 1;
        const bool range_valid = width64 > 0 && length64 > 0 &&
            width64 <= std::numeric_limits<std::int32_t>::max() &&
            length64 <= std::numeric_limits<std::int32_t>::max() &&
            index_max_x >= std::numeric_limits<std::int32_t>::min() &&
            index_max_x <= std::numeric_limits<std::int32_t>::max() &&
            index_max_z >= std::numeric_limits<std::int32_t>::min() &&
            index_max_z <= std::numeric_limits<std::int32_t>::max();
        mIndexWidth = range_valid ? static_cast<std::int32_t>(width64) : 0;
        mIndexLength = range_valid ? static_cast<std::int32_t>(length64) : 0;
        const auto cell_count64 = range_valid
            ? static_cast<std::uint64_t>(width64) * static_cast<std::uint64_t>(length64)
            : std::numeric_limits<std::uint64_t>::max();
        // Avoid turning a sparse, adversarial coordinate range into a huge
        // dense index. Such commands stay in the bounded broad list fallback.
        const bool dense_index = range_valid && cell_count64 <= 1 * 1024 * 1024;
        const auto cell_count = dense_index
            ? static_cast<std::size_t>(cell_count64)
            : std::size_t{0};
        std::vector<std::size_t> counts(cell_count, 0);
        const auto cell_for = [&](std::int32_t x, std::int32_t z) {
            return static_cast<std::size_t>(z - mIndexMinZ) *
                static_cast<std::size_t>(mIndexWidth) +
                static_cast<std::size_t>(x - mIndexMinX);
        };
        // Mark indexed commands explicitly.  A collection of individually
        // small fills can still produce an enormous command-to-cell list;
        // spill those commands into the bounded broad fallback once the
        // entry budget is reached.
        std::vector<std::uint8_t> indexed_commands(mCommands.size(), 0);
        std::size_t index_entries = 0;
        for (std::size_t index = 0; index < mCommands.size(); ++index) {
            const auto& command = mCommands[index];
            const auto x1 = static_cast<std::int64_t>(command.x1) + mOffset.x;
            const auto x2 = static_cast<std::int64_t>(command.x2) + mOffset.x;
            const auto z1 = static_cast<std::int64_t>(command.z1) + mOffset.z;
            const auto z2 = static_cast<std::int64_t>(command.z2) + mOffset.z;
            const auto chunk_x1 = floor_div64(x1, 16), chunk_x2 = floor_div64(x2, 16);
            const auto chunk_z1 = floor_div64(z1, 16), chunk_z2 = floor_div64(z2, 16);
            const auto span_x = chunk_x2 - chunk_x1 + 1;
            const auto span_z = chunk_z2 - chunk_z1 + 1;
            const auto covered = span_x <= 0 || span_z <= 0
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(span_x) >
                    std::numeric_limits<std::uint64_t>::max() /
                        static_cast<std::uint64_t>(span_z)
                    ? std::numeric_limits<std::uint64_t>::max()
                    : static_cast<std::uint64_t>(span_x) * static_cast<std::uint64_t>(span_z);
            // A command covering an enormous area must not turn the index
            // itself into an unbounded allocation. Such commands are rare and
            // cheap to test during a requested chunk replay.
            if (!dense_index || covered > 4096 ||
                covered > kMaxIndexEntries - index_entries) {
                mBroadCommands.push_back(static_cast<std::uint32_t>(index));
                continue;
            }
            indexed_commands[index] = 1;
            index_entries += static_cast<std::size_t>(covered);
            for (auto z = chunk_z1; z <= chunk_z2; ++z) {
                for (auto x = chunk_x1; x <= chunk_x2; ++x) {
                    ++counts[cell_for(static_cast<std::int32_t>(x),
                        static_cast<std::int32_t>(z))];
                    if (x == chunk_x2) break;
                }
                if (z == chunk_z2) break;
            }
        }
        if (dense_index) {
            mCommandOffsets.resize(cell_count + 1, 0);
            for (std::size_t cell = 0; cell < cell_count; ++cell) {
                mCommandOffsets[cell + 1] = mCommandOffsets[cell] + counts[cell];
            }
            mCommandIndices.resize(mCommandOffsets.back());
            auto cursors = mCommandOffsets;
            for (std::size_t index = 0; index < mCommands.size(); ++index) {
                const auto& command = mCommands[index];
                const auto chunk_x1 = floor_div64(
                    static_cast<std::int64_t>(command.x1) + mOffset.x, 16);
                const auto chunk_x2 = floor_div64(
                    static_cast<std::int64_t>(command.x2) + mOffset.x, 16);
                const auto chunk_z1 = floor_div64(
                    static_cast<std::int64_t>(command.z1) + mOffset.z, 16);
                const auto chunk_z2 = floor_div64(
                    static_cast<std::int64_t>(command.z2) + mOffset.z, 16);
                if (!indexed_commands[index]) continue;
                for (auto z = chunk_z1; z <= chunk_z2; ++z) {
                    for (auto x = chunk_x1; x <= chunk_x2; ++x) {
                        const auto cell = cell_for(static_cast<std::int32_t>(x),
                            static_cast<std::int32_t>(z));
                        mCommandIndices[cursors[cell]++] = static_cast<std::uint32_t>(index);
                        if (x == chunk_x2) break;
                    }
                    if (z == chunk_z2) break;
                }
            }
        }
            mCommandIndexReady = true;
        }
    }
    for (auto& [chunk_pos, chunk] : result) {
        const auto process = [&](std::uint32_t command_index) {
            const auto& command = mCommands[command_index];
            const auto chunk_x = static_cast<std::int64_t>(chunk_pos.x) * 16;
            const auto chunk_z = static_cast<std::int64_t>(chunk_pos.z) * 16;
            const auto x1 = std::max(static_cast<std::int64_t>(command.x1) + mOffset.x, chunk_x);
            const auto x2 = std::min(static_cast<std::int64_t>(command.x2) + mOffset.x, chunk_x + 15);
            const auto z1 = std::max(static_cast<std::int64_t>(command.z1) + mOffset.z, chunk_z);
            const auto z2 = std::min(static_cast<std::int64_t>(command.z2) + mOffset.z, chunk_z + 15);
            if (x1 > x2 || z1 > z2) return;

            const auto y1 = static_cast<std::int64_t>(command.y1) + mOffset.y;
            const auto y2 = static_cast<std::int64_t>(command.y2) + mOffset.y;
            const auto first_sub_y = floor_div64(y1 - 64, 16);
            const auto last_sub_y = floor_div64(y2 - 64, 16);
            const auto local_x1 = x1 - chunk_x;
            const auto local_x2 = x2 - chunk_x;
            const auto width = static_cast<std::size_t>(local_x2 - local_x1 + 1);
            for (auto sub_y64 = first_sub_y; sub_y64 <= last_sub_y; ++sub_y64) {
                if (sub_y64 < std::numeric_limits<std::int32_t>::min() ||
                    sub_y64 > std::numeric_limits<std::int32_t>::max()) return;
                const auto sub_y = static_cast<std::int32_t>(sub_y64);
                auto sub = chunk.sub_chunks.find(sub_y);
                if (sub == chunk.sub_chunks.end()) {
                    auto [inserted, created] = chunk.sub_chunks.try_emplace(sub_y);
                    sub = inserted;
                    if (created) {
                        sub->second.layer0.fill(air);
                        sub->second.layer1.fill(air);
                    }
                    // Keep the all-air subchunk so save_chunk() clears an
                    // existing destination subchunk, but avoid rewriting the
                    // 4096 entries that were just initialized to air.
                    if (command.runtime_id == air) continue;
                }
                const auto sub_base_y = sub_y64 * 16 + 64;
                const auto local_y1 = std::max(y1, sub_base_y) - sub_base_y;
                const auto local_y2 = std::min(y2, sub_base_y + 15) - sub_base_y;
                for (auto local_y = local_y1; local_y <= local_y2; ++local_y) {
                    for (auto z = z1; z <= z2; ++z) {
                        const auto local_z = z - chunk_z;
                        const auto begin = static_cast<std::size_t>(
                            (local_y * 16 + local_z) * 16 + local_x1);
                        std::fill_n(
                            sub->second.layer0.begin() + begin,
                            width,
                            command.runtime_id);
                        if (z == z2) break;
                    }
                    if (local_y == local_y2) break;
                }
                if (sub_y64 == last_sub_y) break;
            }
        };
        std::size_t indexed_begin = 0;
        std::size_t indexed_end = 0;
        if (!mCommandOffsets.empty() &&
            static_cast<std::int64_t>(chunk_pos.x) >= mIndexMinX &&
            static_cast<std::int64_t>(chunk_pos.z) >= mIndexMinZ &&
            static_cast<std::int64_t>(chunk_pos.x) <
                static_cast<std::int64_t>(mIndexMinX) + mIndexWidth &&
            static_cast<std::int64_t>(chunk_pos.z) <
                static_cast<std::int64_t>(mIndexMinZ) + mIndexLength) {
            const auto cell = static_cast<std::size_t>(chunk_pos.z - mIndexMinZ) *
                static_cast<std::size_t>(mIndexWidth) +
                static_cast<std::size_t>(chunk_pos.x - mIndexMinX);
            indexed_begin = mCommandOffsets[cell];
            indexed_end = mCommandOffsets[cell + 1];
        }
        // Both slices are sorted by source command index. Merge them so very
        // broad fill commands retain their exact ordering relative to local
        // commands instead of being replayed at the end.
        std::size_t broad_index = 0;
        while (indexed_begin < indexed_end || broad_index < mBroadCommands.size()) {
            if (broad_index == mBroadCommands.size() ||
                (indexed_begin < indexed_end &&
                 mCommandIndices[indexed_begin] < mBroadCommands[broad_index])) {
                process(mCommandIndices[indexed_begin++]);
            } else {
                process(mBroadCommands[broad_index++]);
            }
        }
    }
    return Result<ChunkMap>::success(std::move(result));
}

Result<void> McFunctionStructure::visit_chunks(
    std::span<const ChunkPos> positions,
    const ChunkVisitor& visitor) const
{
    if (!visitor) return Result<void>::failure("chunk visitor is empty");
    // Keep only one materialized chunk alive.  This is important for callers
    // that stream directly into a world or an encoder.
    for (const auto position : positions) {
        const std::array<ChunkPos, 1> request{{position}};
        auto chunks = get_chunks(request);
        if (!chunks) return Result<void>::failure(chunks.error());
        const auto found = chunks.value().find(position);
        if (found == chunks.value().end()) continue;
        auto visited = visitor(position, found->second);
        if (!visited) return visited;
    }
    return Result<void>::success();
}

Result<std::size_t> McFunctionStructure::count_non_air_blocks() const
{
    if (mNonAirBlocks) return Result<std::size_t>::success(*mNonAirBlocks);
    std::size_t count = 0;
    for (std::int32_t z = 0; z < mSize.chunk_z_count(); ++z) {
        for (std::int32_t x = 0; x < mSize.chunk_x_count(); ++x) {
            const std::array<ChunkPos, 1> position{{x, z}};
            auto chunks = get_chunks(position);
            if (!chunks) return Result<std::size_t>::failure(chunks.error());
            const auto& chunk = chunks.value().at({x, z});
            for (const auto& [_, sub] : chunk.sub_chunks)
                for (const auto runtime : sub.layer0) if (runtime != mRegistry.air_runtime_id()) ++count;
        }
    }
    mNonAirBlocks = count;
    return Result<std::size_t>::success(count);
}

Result<NbtChunkMap> McFunctionStructure::get_chunk_nbt(std::span<const ChunkPos> positions) const
{
    NbtChunkMap result; for (const auto pos : positions) result.emplace(pos, std::vector<BlockEntity>{});
    return Result<NbtChunkMap>::success(std::move(result));
}

Result<void> McFunctionStructure::write_to_world(WorldTarget& world, SubChunkPos start, ConversionCallbacks callbacks) const { return convert_to_world(*this, world, start, std::move(callbacks)); }
Result<void> McFunctionStructure::read_from_world(WorldSource&, BlockBox, ConversionCallbacks) { return Result<void>::failure("MCFunction 导出尚未迁移"); }

} // namespace water_structure
