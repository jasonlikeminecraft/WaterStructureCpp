#include "archive.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
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

bool safe_relative_path(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

Result<std::vector<std::uint8_t>> inflate_raw(
    std::span<const std::uint8_t> compressed,
    std::size_t expected_size)
{
    if (compressed.size() > std::numeric_limits<uInt>::max() ||
        expected_size > std::numeric_limits<uInt>::max()) {
        return Result<std::vector<std::uint8_t>>::failure("ZIP entry 超出 zlib 单次解压范围");
    }
    // zlib needs at least one byte of output space to consume an empty raw
    // deflate stream and report Z_STREAM_END.
    std::vector<std::uint8_t> output(std::max<std::size_t>(expected_size, 1));
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return Result<std::vector<std::uint8_t>>::failure("初始化 raw deflate 解压失败");
    }
    const auto status = inflate(&stream, Z_FINISH);
    const bool valid = status == Z_STREAM_END && stream.total_out == expected_size;
    inflateEnd(&stream);
    if (!valid) return Result<std::vector<std::uint8_t>>::failure("ZIP entry deflate 数据无效");
    output.resize(expected_size);
    return Result<std::vector<std::uint8_t>>::success(std::move(output));
}

struct CentralEntry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t local_offset = 0;
};

} // namespace

Result<void> extract_zip(const std::filesystem::path& archive_path, const std::filesystem::path& destination)
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
        if (read_le32(tail.data() + i) == 0x06054b50u) {
            eocd_position = i;
            break;
        }
    }
    if (eocd_position == std::string::npos) return Result<void>::failure("ZIP 缺少 EOCD");
    const auto* eocd = tail.data() + eocd_position;
    const auto entry_count = read_le16(eocd + 10);
    const auto central_size = read_le32(eocd + 12);
    const auto central_offset = read_le32(eocd + 16);
    if (central_size == UINT32_MAX || central_offset == UINT32_MAX) {
        return Result<void>::failure("不支持 ZIP64");
    }
    if (static_cast<std::uint64_t>(central_offset) + central_size > static_cast<std::uint64_t>(file_size)) {
        return Result<void>::failure("ZIP central directory 越界");
    }

    std::vector<std::uint8_t> central(central_size);
    input.seekg(central_offset);
    input.read(reinterpret_cast<char*>(central.data()), static_cast<std::streamsize>(central.size()));
    if (!input) return Result<void>::failure("读取 ZIP central directory 失败");

    std::error_code create_error;
    std::filesystem::create_directories(destination, create_error);
    if (create_error) return Result<void>::failure("创建 ZIP 解压目录失败: " + create_error.message());

    std::size_t position = 0;
    for (std::uint16_t entry_index = 0; entry_index < entry_count; ++entry_index) {
        if (position + 46 > central.size() || read_le32(central.data() + position) != 0x02014b50u) {
            return Result<void>::failure("ZIP central directory entry 无效");
        }
        const auto flags = read_le16(central.data() + position + 8);
        const auto method = read_le16(central.data() + position + 10);
        const auto expected_crc = read_le32(central.data() + position + 16);
        const auto compressed_size = read_le32(central.data() + position + 20);
        const auto uncompressed_size = read_le32(central.data() + position + 24);
        const auto name_length = read_le16(central.data() + position + 28);
        const auto extra_length = read_le16(central.data() + position + 30);
        const auto comment_length = read_le16(central.data() + position + 32);
        const auto local_offset = read_le32(central.data() + position + 42);
        if ((flags & 1u) != 0) return Result<void>::failure("不支持加密 ZIP entry");
        if (compressed_size == UINT32_MAX || uncompressed_size == UINT32_MAX || local_offset == UINT32_MAX) {
            return Result<void>::failure("不支持 ZIP64 entry");
        }
        if (position + 46ull + name_length + extra_length + comment_length > central.size()) {
            return Result<void>::failure("ZIP central directory entry 截断");
        }
        const std::string entry_name(
            reinterpret_cast<const char*>(central.data() + position + 46), name_length);
        position += 46 + name_length + extra_length + comment_length;
        const auto relative = std::filesystem::path(entry_name).lexically_normal();
        if (!safe_relative_path(relative)) return Result<void>::failure("ZIP entry 路径不安全: " + entry_name);
        const auto output_path = destination / relative;
        const bool directory = !entry_name.empty() && (entry_name.back() == '/' || entry_name.back() == '\\');
        if (directory) {
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
        input.seekg(local_offset);
        input.read(reinterpret_cast<char*>(local.data()), local.size());
        if (!input || read_le32(local.data()) != 0x04034b50u) {
            return Result<void>::failure("ZIP local header 无效");
        }
        const auto local_name_length = read_le16(local.data() + 26);
        const auto local_extra_length = read_le16(local.data() + 28);
        const auto data_offset = static_cast<std::uint64_t>(local_offset) + 30 + local_name_length + local_extra_length;
        if (data_offset + compressed_size > static_cast<std::uint64_t>(file_size)) {
            return Result<void>::failure("ZIP entry 数据越界");
        }
        std::vector<std::uint8_t> compressed(compressed_size);
        input.seekg(static_cast<std::streamoff>(data_offset));
        input.read(reinterpret_cast<char*>(compressed.data()), compressed.size());
        if (!input && compressed_size != 0) return Result<void>::failure("读取 ZIP entry 数据失败");

        std::vector<std::uint8_t> bytes;
        if (method == 0) {
            bytes = std::move(compressed);
            if (bytes.size() != uncompressed_size) return Result<void>::failure("ZIP stored entry 长度不匹配");
        } else if (method == Z_DEFLATED) {
            auto inflated = inflate_raw(compressed, uncompressed_size);
            if (!inflated) return Result<void>::failure(inflated.error() + ": " + entry_name);
            bytes = std::move(inflated).value();
        } else {
            return Result<void>::failure("不支持 ZIP 压缩方法: " + std::to_string(method));
        }
        if (crc32(0L, bytes.data(), static_cast<uInt>(bytes.size())) != expected_crc) {
            return Result<void>::failure("ZIP entry CRC32 不匹配: " + entry_name);
        }
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) return Result<void>::failure("写入 ZIP entry 失败: " + output_path.string());
    }
    return Result<void>::success();
}

Result<void> create_zip(const std::filesystem::path& directory, const std::filesystem::path& archive_path)
{
    if (!std::filesystem::is_directory(directory)) {
        return Result<void>::failure("待打包路径不是目录: " + directory.string());
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
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
    return Result<void>::success();
}

} // namespace water_structure::archive
