#pragma once

#include <stdint.h>

#if defined(_WIN32) && defined(WATER_STRUCTURE_BUILD_SHARED)
#  define WATER_STRUCTURE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(WATER_STRUCTURE_USE_SHARED)
#  define WATER_STRUCTURE_API __declspec(dllimport)
#elif defined(WATER_STRUCTURE_BUILD_SHARED) && (defined(__GNUC__) || defined(__clang__))
#  define WATER_STRUCTURE_API __attribute__((visibility("default")))
#else
#  define WATER_STRUCTURE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_context ws_context;
typedef struct ws_reader ws_reader;

/* Progress stages reported by the optional conversion callback. */
#define WS_PROGRESS_OPEN 0
#define WS_PROGRESS_READ 1
#define WS_PROGRESS_ENCODE 2
#define WS_PROGRESS_WRITE 3
#define WS_PROGRESS_FINALIZE 4

#define WS_CAP_FILE_READER          (1u << 0)
#define WS_CAP_FILE_WRITER          (1u << 1)
#define WS_CAP_STRUCTURE_TO_WORLD   (1u << 2)
#define WS_CAP_WORLD_TO_STRUCTURE   (1u << 3)
#define WS_CAP_STREAMING_READER     (1u << 4)
#define WS_CAP_STREAMING_WRITER     (1u << 5)
#define WS_CAP_LOSSY_ROUND_TRIP     (1u << 6)

typedef void (*ws_progress_callback)(
    void* user_data,
    uint8_t stage,
    uint64_t completed,
    uint64_t total);

typedef struct ws_structure_info {
    uint8_t format_id;
    int32_t width;
    int32_t height;
    int32_t length;
    int32_t offset_x;
    int32_t offset_y;
    int32_t offset_z;
    uint64_t non_air_blocks;
} ws_structure_info;

/* The returned pointer remains valid until the next call on the same context. */
WATER_STRUCTURE_API const char* ws_version(void);
WATER_STRUCTURE_API uint32_t ws_abi_version(void);
WATER_STRUCTURE_API uint32_t ws_format_count(void);
WATER_STRUCTURE_API const char* ws_format_name(uint8_t format_id);
WATER_STRUCTURE_API uint32_t ws_format_capabilities(uint8_t format_id);
WATER_STRUCTURE_API ws_context* ws_context_create(const char* assets_directory_utf8);
WATER_STRUCTURE_API void ws_context_destroy(ws_context* context);
WATER_STRUCTURE_API const char* ws_last_error(const ws_context* context);

WATER_STRUCTURE_API ws_reader* ws_reader_open(
    ws_context* context,
    const char* input_path_utf8,
    int streaming_world_import);
WATER_STRUCTURE_API void ws_reader_close(ws_reader* reader);
WATER_STRUCTURE_API int ws_reader_info(
    ws_reader* reader,
    ws_structure_info* output);
WATER_STRUCTURE_API const char* ws_reader_format(const ws_reader* reader);

/* target_format is the registry name, e.g. "SchemV1", "BDX", or "MCFunction". */
WATER_STRUCTURE_API int ws_convert(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count);

/* Extended conversion variant. clear_air is non-zero for the default
 * bounding-box clear; pass zero to emit only non-air placement commands. */
WATER_STRUCTURE_API int ws_convert_ex(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air);

/* Adds MCFunction chunk-partitioned optimization without changing the ABI of
 * ws_convert_ex. chunk_partition is non-zero to keep every emitted placement
 * command inside one 16x16 chunk. */
WATER_STRUCTURE_API int ws_convert_ex2(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    int chunk_partition);

/* Streaming-budget extension. A zero soft_memory_budget_bytes selects the
 * library default (currently 450 MiB); zero queue limits select bounded
 * hardware-derived defaults. This adds a symbol without changing prior ABI. */
WATER_STRUCTURE_API int ws_convert_ex3(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    int chunk_partition,
    uint64_t soft_memory_budget_bytes,
    uint64_t max_in_flight_tasks,
    uint64_t max_in_flight_chunks,
    int allow_temporary_spool);

/* Optional progress-enabled variant; ws_convert remains ABI-compatible. */
WATER_STRUCTURE_API int ws_convert_with_progress(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    ws_progress_callback callback,
    void* user_data);

WATER_STRUCTURE_API int ws_convert_with_progress_ex(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    ws_progress_callback callback,
    void* user_data);

WATER_STRUCTURE_API int ws_convert_with_progress_ex2(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    int chunk_partition,
    ws_progress_callback callback,
    void* user_data);

WATER_STRUCTURE_API int ws_convert_with_progress_ex3(
    ws_context* context,
    const char* input_path_utf8,
    const char* target_format,
    const char* output_path_utf8,
    uint64_t thread_count,
    int clear_air,
    int chunk_partition,
    uint64_t soft_memory_budget_bytes,
    uint64_t max_in_flight_tasks,
    uint64_t max_in_flight_chunks,
    int allow_temporary_spool,
    ws_progress_callback callback,
    void* user_data);

WATER_STRUCTURE_API int ws_to_world(
    ws_context* context,
    const char* input_path_utf8,
    const char* world_path_utf8,
    int32_t start_x,
    int32_t start_y,
    int32_t start_z);

WATER_STRUCTURE_API int ws_to_world_with_progress(
    ws_context* context,
    const char* input_path_utf8,
    const char* world_path_utf8,
    int32_t start_x,
    int32_t start_y,
    int32_t start_z,
    ws_progress_callback callback,
    void* user_data);

/* Versioned world conversion entry point. Existing ws_to_world symbols keep
 * their ABI and use the conservative defaults. */
WATER_STRUCTURE_API int ws_to_world_ex3(
    ws_context* context,
    const char* input_path_utf8,
    const char* world_path_utf8,
    int32_t start_x,
    int32_t start_y,
    int32_t start_z,
    uint64_t worker_count,
    uint64_t soft_memory_budget_bytes,
    uint64_t max_in_flight_chunks,
    int allow_temporary_spool);

WATER_STRUCTURE_API int ws_to_world_with_progress_ex3(
    ws_context* context,
    const char* input_path_utf8,
    const char* world_path_utf8,
    int32_t start_x,
    int32_t start_y,
    int32_t start_z,
    uint64_t worker_count,
    uint64_t soft_memory_budget_bytes,
    uint64_t max_in_flight_chunks,
    int allow_temporary_spool,
    ws_progress_callback callback,
    void* user_data);

#ifdef __cplusplus
}
#endif
