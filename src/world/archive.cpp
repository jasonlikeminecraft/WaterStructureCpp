#include "archive.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace water_structure::archive {

namespace {

std::uint16_t read_le16(const std::uint8_t* value)
{
    return static_cast<std::uint16_t>(value[0]) |
        (static_cast<std::uint16_t>(value[1]) << 8u);
}

std::uint32_t read_le32(const std::uint8_t* value)
{
    return static_cast<std::uint32_t>(value[0]) |
        (static_cast<std::uint32_t>(value[1]) << 8u) |
        (static_cast<std::uint32_t>(value[2]) << 16u) |
        (static_cast<std::uint32_t>(value[3]) << 24u);
}

void write_le16(std::ostream& output, std::uint16_t value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu)
    };
    output.write(bytes.data(), bytes.size());
}

void write_le32(std::ostream& output, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu)
    };
    output.write(bytes.data(), bytes.size());
}

bool safe_relative_path(std::string_view name)
{
    // ZIP names are conventionally slash separated, but accepting a
    // backslash as a separator is necessary for archives produced on
    // Windows.  Validate the byte string before constructing a platform
    // path so that a Windows archive cannot smuggle a drive/root path into a
    // POSIX build (and vice versa).
    if (name.empty() || name.find('\0') != std::string_view::npos) return false;
    if (name.front() == '/' || name.front() == '\\') return false;
    if (name.size() >= 2 && name[1] == ':') return false;
    std::size_t component_begin = 0;
    while (component_begin <= name.size()) {
        const auto separator = name.find_first_of("/\\", component_begin);
        const auto component_end = separator == std::string_view::npos ? name.size() : separator;
        const auto component = name.substr(component_begin, component_end - component_begin);
        if (component == "..") return false;
        if (separator == std::string_view::npos) break;
        component_begin = separator + 1;
    }
    return true;
}

bool read_exact(std::istream& input, void* destination, std::size_t size)
{
    if (size == 0) return true;
    input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

Result<void> copy_stored_entry(
    std::istream& input,
    std::ostream& output,
    std::uint32_t compressed_size,
    std::uint32_t expected_size,
    std::uint32_t expected_crc,
    std::string_view entry_name,
    std::string_view output_name,
    std::vector<std::uint8_t>& buffer)
{
    if (compressed_size != expected_size) {
        return Result<void>::failure("ZIP stored entry 长度不匹配");
    }
    std::uint64_t remaining = compressed_size;
    std::uint64_t written = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    while (remaining != 0) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        if (!read_exact(input, buffer.data(), count)) {
            return Result<void>::failure("读取 ZIP entry 数据失败");
        }
        crc = crc32(crc, buffer.data(), static_cast<uInt>(count));
        output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(count));
        if (!output) return Result<void>::failure("写入 ZIP entry 失败: " + std::string(output_name));
        remaining -= count;
        written += count;
    }
    if (written != expected_size) {
        return Result<void>::failure("ZIP stored entry 长度不匹配");
    }
    if (static_cast<std::uint32_t>(crc) != expected_crc) {
        return Result<void>::failure("ZIP entry CRC32 不匹配: " + std::string(entry_name));
    }
    return Result<void>::success();
}

Result<void> inflate_entry(
    std::istream& input,
    std::ostream& output,
    std::uint32_t compressed_size,
    std::uint32_t expected_size,
    std::uint32_t expected_crc,
    std::string_view entry_name,
    std::string_view output_name,
    std::vector<std::uint8_t>& input_buffer,
    std::vector<std::uint8_t>& output_buffer)
{
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return Result<void>::failure("初始化 raw deflate 解压失败");
    }

    std::uint64_t compressed_remaining = compressed_size;
    std::uint64_t output_size = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    bool stream_end = false;
    std::string error;

    // Feed zlib one bounded input window at a time.  No compressed or
    // decompressed entry-sized allocation is made here.
    while (compressed_remaining != 0 && !stream_end) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(compressed_remaining, input_buffer.size()));
        if (!read_exact(input, input_buffer.data(), count)) {
            error = "读取 ZIP entry 数据失败";
            break;
        }
        compressed_remaining -= count;
        stream.next_in = input_buffer.data();
        stream.avail_in = static_cast<uInt>(count);
        while (stream.avail_in != 0 && !stream_end) {
            stream.next_out = output_buffer.data();
            stream.avail_out = static_cast<uInt>(output_buffer.size());
            const auto before_in = stream.avail_in;
            const auto status = inflate(&stream, Z_NO_FLUSH);
            const auto produced = output_buffer.size() - stream.avail_out;
            if (produced != 0) {
                if (output_size + produced > expected_size) {
                    error = "ZIP entry 解压长度超过声明值: " + std::string(entry_name);
                    break;
                }
                crc = crc32(crc, output_buffer.data(), static_cast<uInt>(produced));
                output.write(reinterpret_cast<const char*>(output_buffer.data()),
                    static_cast<std::streamsize>(produced));
                if (!output) {
                    error = "写入 ZIP entry 失败: " + std::string(output_name);
                    break;
                }
                output_size += produced;
            }
            if (status == Z_STREAM_END) {
                stream_end = true;
                // Any bytes left in the declared compressed payload are
                // trailing garbage.  Reject them instead of silently
                // accepting a malformed entry.
                if (stream.avail_in != 0 || compressed_remaining != 0) {
                    error = "ZIP entry deflate 压缩长度不匹配: " + std::string(entry_name);
                }
                break;
            }
            if (status != Z_OK) {
                error = "ZIP entry deflate 数据无效: " + std::string(entry_name);
                break;
            }
            if (stream.avail_in == before_in && produced == 0) {
                error = "ZIP entry deflate 数据无进展: " + std::string(entry_name);
                break;
            }
        }
        if (!error.empty()) break;
    }

    // A valid raw stream can consume its final byte while requiring one more
    // call to report Z_STREAM_END (notably for empty streams).  Give zlib a
    // zero-input call, but never permit it to manufacture output beyond the
    // central-directory declaration.
    if (error.empty() && !stream_end && compressed_remaining == 0) {
        stream.next_in = nullptr;
        stream.avail_in = 0;
        stream.next_out = output_buffer.data();
        stream.avail_out = static_cast<uInt>(output_buffer.size());
        const auto status = inflate(&stream, Z_FINISH);
        const auto produced = output_buffer.size() - stream.avail_out;
        if (produced != 0) {
            if (output_size + produced > expected_size) {
                error = "ZIP entry 解压长度超过声明值: " + std::string(entry_name);
            } else {
                crc = crc32(crc, output_buffer.data(), static_cast<uInt>(produced));
                output.write(reinterpret_cast<const char*>(output_buffer.data()),
                    static_cast<std::streamsize>(produced));
                if (!output) {
                    error = "写入 ZIP entry 失败: " + std::string(output_name);
                } else {
                    output_size += produced;
                }
            }
        }
        if (error.empty() && status == Z_STREAM_END) stream_end = true;
        if (error.empty() && !stream_end) error = "ZIP entry deflate 数据截断: " + std::string(entry_name);
    }
    inflateEnd(&stream);

    if (!error.empty()) return Result<void>::failure(std::move(error));
    if (!stream_end) return Result<void>::failure("ZIP entry deflate 数据截断: " + std::string(entry_name));
    if (output_size != expected_size) {
        return Result<void>::failure("ZIP entry 解压长度不匹配: " + std::string(entry_name));
    }
    if (static_cast<std::uint32_t>(crc) != expected_crc) {
        return Result<void>::failure("ZIP entry CRC32 不匹配: " + std::string(entry_name));
    }
    return Result<void>::success();
}

Result<void> validate_data_descriptor(
    std::istream& input,
    std::uint64_t data_region_end,
    std::uint32_t expected_crc,
    std::uint32_t expected_compressed_size,
    std::uint32_t expected_uncompressed_size,
    std::string_view entry_name)
{
    std::array<std::uint8_t, 4> first{};
    if (!read_exact(input, first.data(), first.size())) {
        return Result<void>::failure("ZIP entry data descriptor 截断: " + std::string(entry_name));
    }
    const auto first_value = read_le32(first.data());
    std::uint32_t crc = first_value;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    if (first_value == 0x08074b50u) {
        std::array<std::uint8_t, 12> descriptor{};
        if (!read_exact(input, descriptor.data(), descriptor.size())) {
            return Result<void>::failure("ZIP entry data descriptor 截断: " + std::string(entry_name));
        }
        crc = read_le32(descriptor.data());
        compressed_size = read_le32(descriptor.data() + 4);
        uncompressed_size = read_le32(descriptor.data() + 8);
    } else {
        std::array<std::uint8_t, 8> descriptor{};
        if (!read_exact(input, descriptor.data(), descriptor.size())) {
            return Result<void>::failure("ZIP entry data descriptor 截断: " + std::string(entry_name));
        }
        compressed_size = read_le32(descriptor.data());
        uncompressed_size = read_le32(descriptor.data() + 4);
    }
    const auto position = input.tellg();
    if (position < 0 || static_cast<std::uint64_t>(position) > data_region_end) {
        return Result<void>::failure("ZIP entry data descriptor 越界: " + std::string(entry_name));
    }
    if (crc != expected_crc || compressed_size != expected_compressed_size ||
        uncompressed_size != expected_uncompressed_size) {
        return Result<void>::failure("ZIP entry data descriptor 不匹配: " + std::string(entry_name));
    }
    return Result<void>::success();
}

struct CentralEntry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t local_offset = 0;
};

} // namespace

Result<void> extract_zip(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    std::uint64_t maximum_uncompressed_bytes)
{
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) return Result<void>::failure("无法打开 ZIP: " + archive_path.string());
    input.seekg(0, std::ios::end);
    const auto file_size = input.tellg();
    if (file_size < 22) return Result<void>::failure("ZIP 文件过短");

    const auto tail_size = static_cast<std::size_t>(std::min<std::streamoff>(file_size, 0x10000 + 22));
    std::vector<std::uint8_t> tail(tail_size);
    input.seekg(file_size - static_cast<std::streamoff>(tail_size));
    input.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!input) return Result<void>::failure("读取 ZIP 尾部失败");

    std::size_t eocd_position = std::string::npos;
    for (std::size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (read_le32(tail.data() + i) == 0x06054b50u &&
            i + 22u + read_le16(tail.data() + i + 20) == tail.size()) {
            eocd_position = i;
            break;
        }
    }
    if (eocd_position == std::string::npos) return Result<void>::failure("ZIP 缺少 EOCD");
    const auto* eocd = tail.data() + eocd_position;
    const auto disk_number = read_le16(eocd + 4);
    const auto central_disk = read_le16(eocd + 6);
    const auto disk_entry_count = read_le16(eocd + 8);
    const auto entry_count = read_le16(eocd + 10);
    const auto central_size = read_le32(eocd + 12);
    const auto central_offset = read_le32(eocd + 16);
    const auto eocd_offset = static_cast<std::uint64_t>(file_size) - tail_size + eocd_position;
    if (disk_number != 0 || central_disk != 0 || disk_entry_count != entry_count) {
        return Result<void>::failure("不支持分卷 ZIP");
    }
    if (disk_entry_count == UINT16_MAX || entry_count == UINT16_MAX ||
        central_size == UINT32_MAX || central_offset == UINT32_MAX) {
        return Result<void>::failure("不支持 ZIP64");
    }
    if (static_cast<std::uint64_t>(central_offset) + central_size > eocd_offset) {
        return Result<void>::failure("ZIP central directory 越界");
    }

    std::error_code create_error;
    std::filesystem::create_directories(destination, create_error);
    if (create_error) return Result<void>::failure("创建 ZIP 解压目录失败: " + create_error.message());

    // Read the central directory entry-by-entry.  The directory itself is
    // bounded by the archive's declared central_size, and no second archive-
    // sized buffer is needed while payloads are streamed below.
    const auto central_end = static_cast<std::uint64_t>(central_offset) + central_size;
    std::uint64_t central_position = central_offset;
    std::uint64_t declared_uncompressed_total = 0;
    std::vector<std::uint8_t> transfer_buffer(64 * 1024);
    std::vector<std::uint8_t> inflate_buffer(64 * 1024);
    for (std::uint16_t entry_index = 0; entry_index < entry_count; ++entry_index) {
        if (central_position + 46 > central_end) {
            return Result<void>::failure("ZIP central directory entry 无效");
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(central_position));
        if (!input) return Result<void>::failure("定位 ZIP central directory 失败");
        std::array<std::uint8_t, 46> central_header{};
        if (!read_exact(input, central_header.data(), central_header.size()) ||
            read_le32(central_header.data()) != 0x02014b50u) {
            return Result<void>::failure("ZIP central directory entry 无效");
        }
        const auto flags = read_le16(central_header.data() + 8);
        const auto method = read_le16(central_header.data() + 10);
        const auto expected_crc = read_le32(central_header.data() + 16);
        const auto compressed_size = read_le32(central_header.data() + 20);
        const auto uncompressed_size = read_le32(central_header.data() + 24);
        declared_uncompressed_total += uncompressed_size;
        if (maximum_uncompressed_bytes != 0 &&
            declared_uncompressed_total > maximum_uncompressed_bytes) {
            return Result<void>::failure(
                "ZIP 解压总量超过 temporary_file_limit_bytes");
        }
        const auto name_length = read_le16(central_header.data() + 28);
        const auto extra_length = read_le16(central_header.data() + 30);
        const auto comment_length = read_le16(central_header.data() + 32);
        const auto local_offset = read_le32(central_header.data() + 42);
        if ((flags & 1u) != 0) return Result<void>::failure("不支持加密 ZIP entry");
        if (compressed_size == UINT32_MAX || uncompressed_size == UINT32_MAX || local_offset == UINT32_MAX) {
            return Result<void>::failure("不支持 ZIP64 entry");
        }
        const auto variable_size = static_cast<std::uint64_t>(name_length) + extra_length + comment_length;
        if (central_position + 46 + variable_size > central_end) {
            return Result<void>::failure("ZIP central directory entry 截断");
        }
        std::string entry_name(name_length, '\0');
        if (!read_exact(input, entry_name.data(), entry_name.size())) {
            return Result<void>::failure("读取 ZIP entry 名称失败");
        }
        if (extra_length != 0) {
            input.seekg(static_cast<std::streamoff>(extra_length), std::ios::cur);
            if (!input) return Result<void>::failure("读取 ZIP entry extra 失败");
        }
        if (comment_length != 0) {
            input.seekg(static_cast<std::streamoff>(comment_length), std::ios::cur);
            if (!input) return Result<void>::failure("读取 ZIP entry comment 失败");
        }
        central_position += 46 + variable_size;
        if (!safe_relative_path(entry_name)) return Result<void>::failure("ZIP entry 路径不安全: " + entry_name);
        std::string normalized_name = entry_name;
        std::replace(normalized_name.begin(), normalized_name.end(), '\\', '/');
        std::filesystem::path relative;
        try {
            relative = std::filesystem::u8path(normalized_name).lexically_normal();
        } catch (const std::filesystem::filesystem_error&) {
            return Result<void>::failure("ZIP entry 路径无效: " + entry_name);
        }
        if (!safe_relative_path(relative.generic_string())) {
            return Result<void>::failure("ZIP entry 路径不安全: " + entry_name);
        }
        const auto output_path = destination / relative;
        const bool directory = !entry_name.empty() && (entry_name.back() == '/' || entry_name.back() == '\\');
        if (directory) {
            if (compressed_size != 0 || uncompressed_size != 0 || expected_crc != crc32(0L, Z_NULL, 0)) {
                return Result<void>::failure("ZIP 目录 entry 长度无效: " + entry_name);
            }
            std::filesystem::create_directories(output_path, create_error);
            if (create_error) return Result<void>::failure("创建 ZIP entry 目录失败: " + create_error.message());
            continue;
        }
        std::filesystem::create_directories(output_path.parent_path(), create_error);
        if (create_error) return Result<void>::failure("创建 ZIP entry 父目录失败: " + create_error.message());
        if (static_cast<std::uint64_t>(local_offset) + 30 > static_cast<std::uint64_t>(file_size)) {
            return Result<void>::failure("ZIP local header 越界");
        }
        std::array<std::uint8_t, 30> local{};
        input.clear();
        input.seekg(static_cast<std::streamoff>(local_offset));
        if (!input || !read_exact(input, local.data(), local.size()) ||
            read_le32(local.data()) != 0x04034b50u) {
            return Result<void>::failure("ZIP local header 无效");
        }
        const auto local_flags = read_le16(local.data() + 6);
        const auto local_method = read_le16(local.data() + 8);
        if ((local_flags & 1u) != 0 || local_method != method) {
            return Result<void>::failure("ZIP local header 与 central directory 不匹配");
        }
        if ((flags & 0x0008u) == 0 &&
            (read_le32(local.data() + 14) != expected_crc ||
             read_le32(local.data() + 18) != compressed_size ||
             read_le32(local.data() + 22) != uncompressed_size)) {
            return Result<void>::failure("ZIP local header 长度或 CRC 不匹配");
        }
        const auto local_name_length = read_le16(local.data() + 26);
        const auto local_extra_length = read_le16(local.data() + 28);
        const auto data_offset = static_cast<std::uint64_t>(local_offset) + 30u +
            static_cast<std::uint64_t>(local_name_length) + local_extra_length;
        if (local_name_length != entry_name.size()) {
            return Result<void>::failure("ZIP local header 名称不匹配");
        }
        std::string local_name(local_name_length, '\0');
        if (!read_exact(input, local_name.data(), local_name.size()) || local_name != entry_name) {
            return Result<void>::failure("ZIP local header 名称不匹配");
        }
        if (data_offset < local_offset || data_offset > central_offset ||
            compressed_size > static_cast<std::uint64_t>(central_offset) - data_offset) {
            return Result<void>::failure("ZIP entry 数据越界");
        }
        if (method != 0 && method != Z_DEFLATED) {
            return Result<void>::failure("不支持 ZIP 压缩方法: " + std::to_string(method));
        }
        input.seekg(static_cast<std::streamoff>(data_offset));
        if (!input) return Result<void>::failure("定位 ZIP entry 数据失败");
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output) return Result<void>::failure("无法创建 ZIP entry: " + output_path.string());
        Result<void> extracted = Result<void>::failure("未知 ZIP 压缩方法");
        if (method == 0) {
            extracted = copy_stored_entry(input, output, compressed_size, uncompressed_size,
                expected_crc, entry_name, output_path.string(), transfer_buffer);
        } else if (method == Z_DEFLATED) {
            extracted = inflate_entry(input, output, compressed_size, uncompressed_size,
                expected_crc, entry_name, output_path.string(), transfer_buffer, inflate_buffer);
        }
        if (!extracted) {
            output.close();
            std::error_code remove_error;
            std::filesystem::remove(output_path, remove_error);
            return extracted;
        }
        if ((flags & 0x0008u) != 0) {
            const auto descriptor = validate_data_descriptor(input,
                central_offset, expected_crc,
                compressed_size, uncompressed_size, entry_name);
            if (!descriptor) {
                output.close();
                std::error_code remove_error;
                std::filesystem::remove(output_path, remove_error);
                return descriptor;
            }
        }
        output.close();
        if (!output) {
            std::error_code remove_error;
            std::filesystem::remove(output_path, remove_error);
            return Result<void>::failure("写入 ZIP entry 失败: " + output_path.string());
        }
    }
    if (central_position != central_end) {
        return Result<void>::failure("ZIP central directory 长度不匹配");
    }
    return Result<void>::success();
}

Result<void> create_zip(
    const std::filesystem::path& directory,
    const std::filesystem::path& archive_path,
    std::uint64_t maximum_temporary_bytes)
{
    if (!std::filesystem::is_directory(directory)) {
        return Result<void>::failure("待打包路径不是目录: " + directory.string());
    }
    std::vector<std::filesystem::path> files;
    std::uint64_t source_bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const auto file_bytes = entry.file_size();
        if (file_bytes > UINT64_MAX - source_bytes) {
            return Result<void>::failure("待打包世界总大小溢出");
        }
        source_bytes += file_bytes;
        if (maximum_temporary_bytes != 0 && source_bytes > maximum_temporary_bytes) {
            return Result<void>::failure(
                "待打包世界超过 temporary_file_limit_bytes");
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.size() > UINT16_MAX) return Result<void>::failure("ZIP 文件数量超过 65535");

    std::ofstream output(archive_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure("无法创建 ZIP: " + archive_path.string());
    std::vector<CentralEntry> entries;
    entries.reserve(files.size());
    for (const auto& file : files) {
        const auto size64 = std::filesystem::file_size(file);
        if (size64 > UINT32_MAX) return Result<void>::failure("ZIP entry 超过 4 GiB: " + file.string());
        auto relative = std::filesystem::relative(file, directory).generic_u8string();
        const std::string name(reinterpret_cast<const char*>(relative.data()), relative.size());
        if (name.size() > UINT16_MAX) return Result<void>::failure("ZIP entry 名称过长");
        const auto offset = output.tellp();
        if (offset < 0 || static_cast<std::uint64_t>(offset) > UINT32_MAX) {
            return Result<void>::failure("ZIP 超出 4 GiB，需 ZIP64");
        }
        const auto size = static_cast<std::uint32_t>(size64);
        write_le32(output, 0x04034b50u);
        write_le16(output, 20);
        write_le16(output, 0x0808u);
        write_le16(output, Z_DEFLATED);
        write_le16(output, 0);
        write_le16(output, 0);
        write_le32(output, 0);
        write_le32(output, 0);
        write_le32(output, 0);
        write_le16(output, static_cast<std::uint16_t>(name.size()));
        write_le16(output, 0);
        output.write(name.data(), static_cast<std::streamsize>(name.size()));

        std::ifstream input(file, std::ios::binary);
        if (!input) return Result<void>::failure("无法读取待打包文件: " + file.string());
        z_stream stream{};
        if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return Result<void>::failure("初始化 ZIP deflate 失败");
        }
        uLong crc = crc32(0L, Z_NULL, 0);
        std::array<std::uint8_t, 64 * 1024> input_buffer{};
        std::array<std::uint8_t, 64 * 1024> output_buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(input_buffer.data()), input_buffer.size());
            const auto count = input.gcount();
            if (count <= 0) continue;
            crc = crc32(crc, input_buffer.data(), static_cast<uInt>(count));
            stream.next_in = input_buffer.data();
            stream.avail_in = static_cast<uInt>(count);
            do {
                stream.next_out = output_buffer.data();
                stream.avail_out = static_cast<uInt>(output_buffer.size());
                const auto status = deflate(&stream, Z_NO_FLUSH);
                if (status != Z_OK) {
                    deflateEnd(&stream);
                    return Result<void>::failure("压缩 ZIP entry 失败: " + file.string());
                }
                output.write(
                    reinterpret_cast<const char*>(output_buffer.data()),
                    static_cast<std::streamsize>(output_buffer.size() - stream.avail_out));
            } while (stream.avail_in != 0);
        }
        if (!input.eof()) {
            deflateEnd(&stream);
            return Result<void>::failure("读取待打包文件失败: " + file.string());
        }
        int status = Z_OK;
        do {
            stream.next_out = output_buffer.data();
            stream.avail_out = static_cast<uInt>(output_buffer.size());
            status = deflate(&stream, Z_FINISH);
            if (status != Z_OK && status != Z_STREAM_END) {
                deflateEnd(&stream);
                return Result<void>::failure("结束 ZIP entry 压缩失败: " + file.string());
            }
            output.write(
                reinterpret_cast<const char*>(output_buffer.data()),
                static_cast<std::streamsize>(output_buffer.size() - stream.avail_out));
        } while (status != Z_STREAM_END);
        const auto compressed_size64 = stream.total_out;
        deflateEnd(&stream);
        if (compressed_size64 > UINT32_MAX || !output) {
            return Result<void>::failure("ZIP entry 压缩结果过大: " + file.string());
        }
        const auto compressed_size = static_cast<std::uint32_t>(compressed_size64);
        write_le32(output, 0x08074b50u);
        write_le32(output, static_cast<std::uint32_t>(crc));
        write_le32(output, compressed_size);
        write_le32(output, size);
        const auto temporary_size = output.tellp();
        if (maximum_temporary_bytes != 0 &&
            (temporary_size < 0 || static_cast<std::uint64_t>(temporary_size) > maximum_temporary_bytes)) {
            return Result<void>::failure(
                "临时 .mcworld 超过 temporary_file_limit_bytes");
        }
        entries.push_back({
            name,
            static_cast<std::uint32_t>(crc),
            compressed_size,
            size,
            static_cast<std::uint32_t>(offset)
        });
    }

    const auto central_offset_stream = output.tellp();
    if (central_offset_stream < 0 || static_cast<std::uint64_t>(central_offset_stream) > UINT32_MAX) {
        return Result<void>::failure("ZIP central directory 偏移需要 ZIP64");
    }
    const auto central_offset = static_cast<std::uint32_t>(central_offset_stream);
    for (const auto& entry : entries) {
        write_le32(output, 0x02014b50u);
        write_le16(output, 20);
        write_le16(output, 20);
        write_le16(output, 0x0808u);
        write_le16(output, Z_DEFLATED);
        write_le16(output, 0);
        write_le16(output, 0);
        write_le32(output, entry.crc);
        write_le32(output, entry.compressed_size);
        write_le32(output, entry.uncompressed_size);
        write_le16(output, static_cast<std::uint16_t>(entry.name.size()));
        write_le16(output, 0);
        write_le16(output, 0);
        write_le16(output, 0);
        write_le16(output, 0);
        write_le32(output, 0);
        write_le32(output, entry.local_offset);
        output.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    }
    const auto end = output.tellp();
    if (end < 0 || static_cast<std::uint64_t>(end) > UINT32_MAX) return Result<void>::failure("ZIP 需要 ZIP64");
    const auto central_size = static_cast<std::uint32_t>(end) - central_offset;
    write_le32(output, 0x06054b50u);
    write_le16(output, 0);
    write_le16(output, 0);
    write_le16(output, static_cast<std::uint16_t>(entries.size()));
    write_le16(output, static_cast<std::uint16_t>(entries.size()));
    write_le32(output, central_size);
    write_le32(output, central_offset);
    write_le16(output, 0);
    if (!output) return Result<void>::failure("完成 ZIP 写入失败");
    const auto archive_size = output.tellp();
    if (maximum_temporary_bytes != 0 &&
        (archive_size < 0 || static_cast<std::uint64_t>(archive_size) > maximum_temporary_bytes)) {
        return Result<void>::failure(
            "临时 .mcworld 超过 temporary_file_limit_bytes");
    }
    return Result<void>::success();
}

} // namespace water_structure::archive
