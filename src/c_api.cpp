#include <WaterStructure/c_api.h>

#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/world.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace water_structure {
namespace {

std::filesystem::path utf8_path(const char* value)
{
    if (value == nullptr) return {};
#if defined(_WIN32)
    const auto input = std::string(value);
    if (input.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0);
    if (required <= 0) return std::filesystem::path(input);
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
            wide.data(), required) <= 0) {
        return std::filesystem::path(input);
    }
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(value);
#endif
}

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

void load_assets(RuntimeRegistry& registry, const char* assets_directory_utf8)
{
    std::filesystem::path assets = assets_directory_utf8 == nullptr
        ? std::filesystem::current_path() / "assets"
        : utf8_path(assets_directory_utf8);
    if (assets.filename() != "assets") assets /= "assets";
    const auto mapping = assets / "block_mappings_v1.json";
    if (std::filesystem::is_regular_file(mapping)) {
        auto loaded = registry.load_block_mappings(mapping);
        if (!loaded) throw std::runtime_error(loaded.error());
    }
}

StructureId format_id(std::string_view requested)
{
    const auto wanted = lower(requested);
    for (const auto& info : FormatRegistry::formats()) {
        if (lower(info.name) == wanted) return info.id;
    }
    return StructureId::Unknown;
}

class ProgressBridge {
public:
    ProgressBridge(ws_progress_callback callback, void* user_data)
        : mCallback(callback), mUserData(user_data) {}

    void start(std::uint8_t stage, std::size_t total)
    {
        mCompleted = 0;
        mTotal = total;
        emit(stage, 0, total, true);
    }

    void advance(std::uint8_t stage)
    {
        const auto completed = ++mCompleted;
        emit(stage, completed, mTotal, completed >= mTotal);
    }

    void finish(std::uint8_t stage)
    {
        emit(stage, 1, 1, true);
    }

private:
    void emit(std::uint8_t stage, std::uint64_t completed, std::uint64_t total, bool force)
    {
        if (mCallback == nullptr) return;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mMutex);
        if (!force && now - mLastEmit < std::chrono::milliseconds(100)) return;
        mLastEmit = now;
        mCallback(mUserData, stage, completed, total);
    }

    ws_progress_callback mCallback = nullptr;
    void* mUserData = nullptr;
    std::mutex mMutex;
    std::chrono::steady_clock::time_point mLastEmit{};
    std::atomic<std::uint64_t> mCompleted{ 0 };
    std::size_t mTotal = 0;
};

} // namespace
} // namespace water_structure

struct ws_context {
    water_structure::RuntimeRegistry registry;
    std::string error;
};

struct ws_reader {
    ws_context* context = nullptr;
    std::unique_ptr<water_structure::IStructure> structure;
    std::string format;
};

namespace {

void set_error(ws_context* context, std::string value)
{
    if (context != nullptr) context->error = std::move(value);
}

} // namespace

extern "C" {

WATER_STRUCTURE_API const char* ws_version(void)
{
    return "0.1.7";
}

WATER_STRUCTURE_API uint32_t ws_abi_version(void)
{
    return 1;
}

WATER_STRUCTURE_API ws_context* ws_context_create(const char* assets_directory_utf8)
{
    try {
        auto context = std::make_unique<ws_context>();
        try {
            water_structure::load_assets(context->registry, assets_directory_utf8);
        } catch (const std::exception& error) {
            context->error = error.what();
        }
        context->registry.install_as_bwo_resolver();
        return context.release();
    } catch (const std::exception& error) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

WATER_STRUCTURE_API void ws_context_destroy(ws_context* context)
{
    delete context;
}

WATER_STRUCTURE_API const char* ws_last_error(const ws_context* context)
{
    return context == nullptr ? "invalid context" : context->error.c_str();
}

WATER_STRUCTURE_API ws_reader* ws_reader_open(
    ws_context* context,
    const char* input_path_utf8,
    int streaming_world_import)
{
    if (context == nullptr || input_path_utf8 == nullptr) return nullptr;
    try {
        auto opened = water_structure::FormatRegistry::open(
            water_structure::utf8_path(input_path_utf8), context->registry,
            { streaming_world_import != 0 });
        if (!opened) {
            set_error(context, opened.error());
            return nullptr;
        }
        auto* reader = new ws_reader;
        reader->context = context;
        reader->structure = std::move(opened).value();
        reader->format = std::string(reader->structure->name());
        context->error.clear();
        return reader;
    } catch (const std::exception& error) {
        set_error(context, error.what());
        return nullptr;
    } catch (...) {
        set_error(context, "unknown WaterStructure error");
        return nullptr;
    }
}

WATER_STRUCTURE_API void ws_reader_close(ws_reader* reader)
{
    delete reader;
}

WATER_STRUCTURE_API int ws_reader_info(ws_reader* reader, ws_structure_info* output)
{
    if (reader == nullptr || output == nullptr || !reader->structure) return 0;
    try {
        const auto size = reader->structure->size();
        const auto offset = reader->structure->offset();
        const auto non_air = reader->structure->count_non_air_blocks();
        if (!non_air) {
            set_error(reader->context, non_air.error());
            return 0;
        }
        *output = {
            static_cast<uint8_t>(reader->structure->id()),
            size.width, size.height, size.length,
            offset.x, offset.y, offset.z,
            static_cast<uint64_t>(non_air.value())
        };
        set_error(reader->context, {});
        return 1;
    } catch (const std::exception& error) {
        set_error(reader->context, error.what());
        return 0;
    } catch (...) {
        set_error(reader->context, "unknown WaterStructure error");
        return 0;
    }
}

WATER_STRUCTURE_API const char* ws_reader_format(const ws_reader* reader)
{
    return reader == nullptr ? "" : reader->format.c_str();
}

static int ws_convert_impl(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    int chunk_partition,
    ws_progress_callback callback,
    void* user_data)
{
    if (context == nullptr || input_path_utf8 == nullptr || target_format == nullptr ||
        output_path_utf8 == nullptr) return 0;
    try {
        water_structure::ProgressBridge progress(callback, user_data);
        progress.start(WS_PROGRESS_OPEN, 1);
        const auto target = water_structure::format_id(target_format);
        if (target == water_structure::StructureId::Unknown) {
            set_error(context, "unknown target format: " + std::string(target_format));
            return 0;
        }
        auto opened = water_structure::FormatRegistry::open(
            water_structure::utf8_path(input_path_utf8), context->registry);
        if (!opened) {
            set_error(context, opened.error());
            return 0;
        }
        progress.finish(WS_PROGRESS_OPEN);
        progress.start(WS_PROGRESS_READ, 1);
        progress.finish(WS_PROGRESS_READ);
        water_structure::ConversionOptions options;
        options.thread_count = static_cast<std::size_t>(thread_count);
        options.clear_air = clear_air != 0;
        options.mcfunction_chunk_partition = chunk_partition != 0;
        if (callback != nullptr) {
            options.callbacks.start = [&progress](std::size_t total) {
                progress.start(WS_PROGRESS_ENCODE, total);
            };
            options.callbacks.progress = [&progress]() {
                progress.advance(WS_PROGRESS_ENCODE);
            };
        }
        auto written = water_structure::FormatRegistry::write(
            *opened.value(), target, water_structure::utf8_path(output_path_utf8),
            context->registry, options);
        if (!written) {
            set_error(context, written.error());
            return 0;
        }
        progress.finish(WS_PROGRESS_FINALIZE);
        context->error.clear();
        return 1;
    } catch (const std::exception& error) {
        set_error(context, error.what());
        return 0;
    } catch (...) {
        set_error(context, "unknown WaterStructure error");
        return 0;
    }
}

WATER_STRUCTURE_API int ws_convert(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count)
{
    return ws_convert_impl(
        context, input_path_utf8, target_format, output_path_utf8,
        thread_count, 1, 0, nullptr, nullptr);
}

WATER_STRUCTURE_API int ws_convert_ex(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air)
{
    return ws_convert_impl(
        context, input_path_utf8, target_format, output_path_utf8,
        thread_count, clear_air, 0, nullptr, nullptr);
}

WATER_STRUCTURE_API int ws_convert_ex2(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    int chunk_partition)
{
    return ws_convert_impl(
        context, input_path_utf8, target_format, output_path_utf8,
        thread_count, clear_air, chunk_partition, nullptr, nullptr);
}

WATER_STRUCTURE_API int ws_convert_with_progress(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    ws_progress_callback callback,
    void* user_data)
{
    return ws_convert_impl(
        context, input_path_utf8, target_format, output_path_utf8,
        thread_count, 1, 0, callback, user_data);
}

WATER_STRUCTURE_API int ws_convert_with_progress_ex(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    ws_progress_callback callback,
    void* user_data)
{
    return ws_convert_impl(
        context, input_path_utf8, target_format, output_path_utf8,
        thread_count, clear_air, 0, callback, user_data);
}

WATER_STRUCTURE_API int ws_convert_with_progress_ex2(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    int chunk_partition,
    ws_progress_callback callback,
    void* user_data)
{
    return ws_convert_impl(
        context, input_path_utf8, target_format, output_path_utf8,
        thread_count, clear_air, chunk_partition, callback, user_data);
}

static int ws_to_world_impl(
    ws_context* context,
    const char* input_path_utf8,
    const char* world_path_utf8,
    int32_t start_x,
    int32_t start_y,
    int32_t start_z,
    ws_progress_callback callback,
    void* user_data)
{
    if (context == nullptr || input_path_utf8 == nullptr || world_path_utf8 == nullptr) return 0;
    try {
        water_structure::ProgressBridge progress(callback, user_data);
        progress.start(WS_PROGRESS_OPEN, 1);
        auto opened = water_structure::FormatRegistry::open(
            water_structure::utf8_path(input_path_utf8), context->registry,
            {
                .streaming_world_import = true,
                .direct_schem_world_stream = true
            });
        if (!opened) {
            set_error(context, opened.error());
            return 0;
        }
        progress.finish(WS_PROGRESS_OPEN);
        progress.start(WS_PROGRESS_READ, 1);
        progress.finish(WS_PROGRESS_READ);
        auto world = water_structure::BedrockWorldAdapter::open(
            water_structure::utf8_path(world_path_utf8));
        if (!world) {
            set_error(context, world.error());
            return 0;
        }
        water_structure::ConversionCallbacks callbacks;
        if (callback != nullptr) {
            callbacks.start = [&progress](std::size_t total) {
                progress.start(WS_PROGRESS_WRITE, total);
            };
            callbacks.progress = [&progress]() {
                progress.advance(WS_PROGRESS_WRITE);
            };
        }
        auto converted = opened.value()->write_to_world(
            world.value(), { start_x, start_y, start_z }, std::move(callbacks));
        if (!converted) {
            set_error(context, converted.error());
            return 0;
        }
        auto closed = world.value().close();
        if (!closed) {
            set_error(context, closed.error());
            return 0;
        }
        progress.finish(WS_PROGRESS_FINALIZE);
        context->error.clear();
        return 1;
    } catch (const std::exception& error) {
        set_error(context, error.what());
        return 0;
    } catch (...) {
        set_error(context, "unknown WaterStructure error");
        return 0;
    }
}

WATER_STRUCTURE_API int ws_to_world(
    ws_context* context,
    const char* input_path_utf8,
    const char* world_path_utf8,
    int32_t start_x,
    int32_t start_y,
    int32_t start_z)
{
    return ws_to_world_impl(
        context, input_path_utf8, world_path_utf8,
        start_x, start_y, start_z, nullptr, nullptr);
}

WATER_STRUCTURE_API int ws_to_world_with_progress(
    ws_context* context,
    const char* input_path_utf8,
    const char* world_path_utf8,
    int32_t start_x,
    int32_t start_y,
    int32_t start_z,
    ws_progress_callback callback,
    void* user_data)
{
    return ws_to_world_impl(
        context, input_path_utf8, world_path_utf8,
        start_x, start_y, start_z, callback, user_data);
}

} // extern "C"
