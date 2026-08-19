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

#ifdef __cplusplus
}
#endif
