#include <WaterStructure/canonical.hpp>
#include <WaterStructure/chunk_stream.hpp>
#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/types.hpp>
#include <WaterStructure/world.hpp>

#include "../src/world/archive.hpp"
#include "../src/formats/nbt_text.hpp"
#include "../src/formats/mcworld.hpp"
#include "md5_fixture.hpp"

#include <io/stream_reader.h>
#include <io/ozlibstream.h>
#include <io/stream_writer.h>
#include <tag_array.h>
#include <tag_compound.h>
#include <tag_list.h>
#include <tag_primitive.h>
#include <tag_string.h>
#include <brotli/encode.h>
#include <msgpack.hpp>
#include <nlohmann/json.hpp>
#include <zlib.h>
#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path write_schematic_sample()
{
    const auto path = std::filesystem::temp_directory_path() / "water_structure_cpp_test.schematic";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("create schematic sample");
    }
    nbt::tag_compound root;
    root.emplace<nbt::tag_short>("Width", 2);
    root.emplace<nbt::tag_short>("Height", 1);
    root.emplace<nbt::tag_short>("Length", 1);
    root.emplace<nbt::tag_string>("Materials", "Alpha");
    root["Blocks"] = nbt::tag_byte_array(std::vector<std::int8_t>{ 1, 0 });
    root["Data"] = nbt::tag_byte_array(std::vector<std::int8_t>{ 0, 0 });
    zlib::ozlibstream compressed(file, Z_DEFAULT_COMPRESSION, true);
    nbt::io::write_tag("Schematic", root, compressed, endian::big);
    compressed.close();
    file.close();
    return path;
}

std::filesystem::path write_schem_v2_sample()
{
    const auto path = std::filesystem::temp_directory_path() / "water_structure_cpp_test.schem";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create SchemV2 sample");

    nbt::tag_compound palette;
    palette["minecraft:air"] = std::int32_t{ 0 };
    palette["minecraft:stone"] = std::int32_t{ 1 };
    nbt::tag_compound blocks;
    blocks["Palette"] = std::move(palette);
    blocks["Data"] = nbt::tag_byte_array(std::vector<std::int8_t>{ 1, 0 });
    nbt::tag_compound document;
    document.emplace<nbt::tag_short>("Width", 2);
    document.emplace<nbt::tag_short>("Height", 1);
    document.emplace<nbt::tag_short>("Length", 1);
    document["Offset"] = nbt::tag_int_array(
        std::vector<std::int32_t>{ -40, -206, -61 });
    document["Blocks"] = std::move(blocks);
    nbt::tag_compound root;
    root["Schematic"] = std::move(document);

    zlib::ozlibstream compressed(file, Z_DEFAULT_COMPRESSION, true);
    nbt::io::write_tag("", root, compressed, endian::big);
    compressed.close();
    file.close();
    return path;
}

std::filesystem::path write_schem_v1_sample(
    std::string_view filename,
    std::int16_t width,
    std::vector<std::int8_t> block_data)
{
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create SchemV1 sample");

    nbt::tag_compound palette;
    palette["minecraft:air"] = std::int32_t{ 0 };
    palette["minecraft:stone"] = std::int32_t{ 1 };
    nbt::tag_compound document;
    document.emplace<nbt::tag_short>("Width", width);
    document.emplace<nbt::tag_short>("Height", 1);
    document.emplace<nbt::tag_short>("Length", 1);
    document["Palette"] = std::move(palette);
    document["BlockData"] = nbt::tag_byte_array(std::move(block_data));

    zlib::ozlibstream compressed(file, Z_DEFAULT_COMPRESSION, true);
    nbt::io::write_tag("Schematic", document, compressed, endian::big);
    compressed.close();
    file.close();
    return path;
}

std::filesystem::path write_schem_v1_multibit_sample()
{
    constexpr std::int16_t width = 130;
    std::vector<std::int8_t> block_data;
    block_data.reserve(width);
    for (std::int16_t index = 0; index < width; ++index) {
        block_data.push_back(static_cast<std::int8_t>(index % 4));
    }

    const auto path = std::filesystem::temp_directory_path() /
        "water_structure_cpp_schem_multibit.schem";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create multibit SchemV1 sample");

    nbt::tag_compound palette;
    palette["minecraft:air"] = std::int32_t{ 0 };
    palette["minecraft:stone"] = std::int32_t{ 1 };
    palette["minecraft:dirt"] = std::int32_t{ 2 };
    palette["minecraft:cobblestone"] = std::int32_t{ 3 };
    nbt::tag_compound document;
    document.emplace<nbt::tag_short>("Width", width);
    document.emplace<nbt::tag_short>("Height", 1);
    document.emplace<nbt::tag_short>("Length", 1);
    document["Palette"] = std::move(palette);
    document["BlockData"] = nbt::tag_byte_array(std::move(block_data));

    zlib::ozlibstream compressed(file, Z_DEFAULT_COMPRESSION, true);
    nbt::io::write_tag("Schematic", document, compressed, endian::big);
    compressed.close();
    file.close();
    return path;
}

std::filesystem::path write_mcfunction_sample()
{
    const auto path = std::filesystem::temp_directory_path() / "water_structure_cpp_test.mcfunction";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create MCFunction sample");
    file << "# ignored\n";
    file << "setblock ~-1 ~ ~2 minecraft:stone\n";
    file << "fill 0 0 0 1 0 0 minecraft:dirt\n";
    file << "say ignored\n";
    return path;
}

std::filesystem::path write_kbdx_sample()
{
    const auto path = std::filesystem::temp_directory_path() / "water_structure_cpp_test.kbdx";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create KBDX sample");
    const auto write_u32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            file.put(static_cast<char>(value >> shift));
        }
    };
    write_u32(1);
    write_u32(0);
    write_u32(0);
    write_u32(0);
    write_u32(0);
    write_u32(0);
    file << R"({"minecraft:stone":0})";
    return path;
}

std::filesystem::path write_bdx_payload(std::vector<std::uint8_t> decoded, std::string_view filename)
{
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::vector<std::uint8_t> compressed(BrotliEncoderMaxCompressedSize(decoded.size()));
    std::size_t compressed_size = compressed.size();
    if (BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW,
        BROTLI_MODE_GENERIC, decoded.size(), decoded.data(), &compressed_size, compressed.data()) != BROTLI_TRUE) {
        throw std::runtime_error("compress BDX sample");
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write("BD@", 3);
    file.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed_size));
    return path;
}

std::filesystem::path write_bdx_sample()
{
    std::vector<std::uint8_t> decoded{ 'B','D','X','t','e','s','t',0,1 };
    auto append_u16 = [&](std::uint16_t value) {
        decoded.push_back(static_cast<std::uint8_t>(value >> 8));
        decoded.push_back(static_cast<std::uint8_t>(value));
    };
    auto append_u32 = [&](std::uint32_t value) {
        decoded.push_back(static_cast<std::uint8_t>(value >> 24));
        decoded.push_back(static_cast<std::uint8_t>(value >> 16));
        decoded.push_back(static_cast<std::uint8_t>(value >> 8));
        decoded.push_back(static_cast<std::uint8_t>(value));
    };
    auto append_cstring = [&](std::string_view value) {
        decoded.insert(decoded.end(), value.begin(), value.end());
        decoded.push_back(0);
    };
    auto append_constant = [&](std::string_view value) {
        decoded.push_back(1);
        append_cstring(value);
    };
    auto append_command_data = [&] {
        append_u32(0);
        append_cstring("say test");
        append_cstring("command");
        append_cstring("");
        append_u32(2);
        decoded.insert(decoded.end(), { 1, 1, 0, 1 });
    };

    append_constant("minecraft:stone");
    append_constant("[]");
    append_constant("minecraft:chest");
    decoded.push_back(7); append_u16(0); append_u16(0);
    decoded.push_back(14);
    decoded.push_back(27); append_u16(0); append_u16(0); append_command_data();
    decoded.push_back(18);
    decoded.push_back(40); append_u16(2); append_u16(0);
    decoded.push_back(1);
    append_cstring("minecraft:diamond");
    decoded.push_back(3); append_u16(5); decoded.push_back(2);
    decoded.push_back(16);
    decoded.push_back(41); append_u16(0); append_u16(1); append_u16(0);
    nbt::tag_compound raw_nbt;
    raw_nbt["id"] = nbt::tag_string("Sign");
    raw_nbt["Text"] = nbt::tag_string("hello");
    std::ostringstream nbt_output(std::ios::binary);
    nbt::io::write_tag("", raw_nbt, nbt_output, endian::little);
    const auto nbt_bytes = nbt_output.str();
    decoded.insert(decoded.end(), nbt_bytes.begin(), nbt_bytes.end());

    decoded.push_back(31); decoded.push_back(117);
    decoded.push_back(32); append_u16(6936);
    decoded.push_back(33); append_u32(6936);
    decoded.push_back(34); append_u16(6936); append_command_data();
    decoded.push_back(35); append_u32(6936); append_command_data();
    decoded.push_back(36); append_u16(0); append_command_data();
    decoded.push_back(37); append_u16(6936); decoded.push_back(0);
    decoded.push_back(38); append_u32(6936); decoded.push_back(0);
    decoded.push_back(39); append_u32(3); decoded.insert(decoded.end(), { 1, 2, 3 });
    decoded.push_back(9);
    decoded.push_back(6); append_u16(2);
    decoded.push_back(12); append_u32(static_cast<std::uint32_t>(-2));
    decoded.insert(decoded.end(), { 14, 15, 16, 17, 18, 19 });
    decoded.push_back(20); append_u16(3);
    decoded.push_back(21); append_u32(static_cast<std::uint32_t>(-3));
    decoded.push_back(22); append_u16(4);
    decoded.push_back(23); append_u32(static_cast<std::uint32_t>(-4));
    decoded.push_back(24); append_u16(5);
    decoded.push_back(25); append_u32(static_cast<std::uint32_t>(-5));
    decoded.push_back(28); decoded.push_back(6);
    decoded.push_back(29); decoded.push_back(7);
    decoded.push_back(30); decoded.push_back(8);
    decoded.push_back(28); decoded.push_back(static_cast<std::uint8_t>(-6));
    decoded.push_back(29); decoded.push_back(static_cast<std::uint8_t>(-7));
    decoded.push_back(30); decoded.push_back(static_cast<std::uint8_t>(-8));
    decoded.push_back(88);
    return write_bdx_payload(std::move(decoded), "water_structure_cpp_test.bdx");
}

std::filesystem::path write_truncated_bdx_sample()
{
    std::vector<std::uint8_t> decoded{
        'B','D','X','t','e','s','t',0,1,
        27,0
    };
    return write_bdx_payload(std::move(decoded), "water_structure_cpp_truncated_test.bdx");
}

std::filesystem::path write_truncated_bdx_brotli_sample()
{
    std::vector<std::uint8_t> decoded{ 'B','D','X','t','e','s','t',0,1,39 };
    const std::uint32_t debug_size = 4096;
    decoded.push_back(static_cast<std::uint8_t>(debug_size >> 24));
    decoded.push_back(static_cast<std::uint8_t>(debug_size >> 16));
    decoded.push_back(static_cast<std::uint8_t>(debug_size >> 8));
    decoded.push_back(static_cast<std::uint8_t>(debug_size));
    decoded.insert(decoded.end(), debug_size, 0x5a);
    decoded.push_back(88);
    const auto path = write_bdx_payload(
        std::move(decoded), "water_structure_cpp_truncated_brotli_test.bdx");
    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    bytes.resize(std::max<std::size_t>(4, bytes.size() / 2));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::filesystem::path write_ibimport_sample()
{
    const auto path = std::filesystem::temp_directory_path() / "water_structure_cpp_test.ibi";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write("IBImport ", 9);
    auto segment = [&](std::string data) {
        std::size_t length = data.size();
        do {
            auto byte = static_cast<std::uint8_t>(length & 0x7fu);
            length >>= 7u;
            if (length != 0) byte |= 0x80u;
            file.put(static_cast<char>(byte));
        } while (length != 0);
        constexpr std::uint8_t key = 0x5a;
        file.put(static_cast<char>(key));
        for (const auto byte : data) file.put(static_cast<char>(static_cast<std::uint8_t>(byte) ^ key));
    };
    segment("setblock ~-1 ~0 ~2 minecraft:stone\n");
    segment(R"([{"posX":"~-1","posY":"~0","posZ":"~2","CommandMessage":"say test","Commandtitle":"cmd","Conditional":"1","isRedstone":false,"isTime":2}])");
    return path;
}

void set_test_environment(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (value == nullptr || *value == '\0') unsetenv(name);
    else setenv(name, value, 1);
#endif
}

std::filesystem::path write_schem_v1_ordered_sample()
{
    const auto path = std::filesystem::temp_directory_path() /
        "water_structure_cpp_schem_ordered.schem";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create ordered SchemV1 sample");
    zlib::ozlibstream compressed(file, Z_DEFAULT_COMPRESSION, true);
    nbt::io::stream_writer writer(compressed, endian::big);
    writer.write_type(nbt::tag_type::Compound);
    writer.write_string("Schematic");
    writer.write_tag("Width", nbt::tag_short(2));
    writer.write_tag("Height", nbt::tag_short(1));
    writer.write_tag("Length", nbt::tag_short(1));
    nbt::tag_compound palette;
    palette["minecraft:air"] = std::int32_t{ 0 };
    palette["minecraft:stone"] = std::int32_t{ 1 };
    writer.write_tag("Palette", palette);
    writer.write_tag("BlockData", nbt::tag_byte_array(std::vector<std::int8_t>{ 1, 0 }));
    writer.write_type(nbt::tag_type::End);
    compressed.close();
    file.close();
    return path;
}

std::filesystem::path write_schem_v1_multibyte_varint_sample()
{
    const auto path = std::filesystem::temp_directory_path() /
        "water_structure_cpp_schem_multibyte_varint.schem";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create multi-byte varint SchemV1 sample");
    zlib::ozlibstream compressed(file, Z_DEFAULT_COMPRESSION, true);
    nbt::io::stream_writer writer(compressed, endian::big);
    writer.write_type(nbt::tag_type::Compound);
    writer.write_string("Schematic");
    writer.write_tag("Width", nbt::tag_short(2));
    writer.write_tag("Height", nbt::tag_short(1));
    writer.write_tag("Length", nbt::tag_short(1));
    nbt::tag_compound palette;
    palette["minecraft:air"] = std::int32_t{ 0 };
    palette["minecraft:stone"] = std::int32_t{ 128 };
    writer.write_tag("Palette", palette);
    writer.write_tag("BlockData", nbt::tag_byte_array(
        std::vector<std::int8_t>{
            static_cast<std::int8_t>(0x80),
            static_cast<std::int8_t>(0x01),
            static_cast<std::int8_t>(0x00)
        }));
    writer.write_type(nbt::tag_type::End);
    compressed.close();
    file.close();
    return path;
}

std::filesystem::path write_ibimport_fill_sample()
{
    const auto path = std::filesystem::temp_directory_path() /
        "water_structure_cpp_fill_test.ibi";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write("IBImport ", 9);
    auto segment = [&](std::string_view data) {
        std::uint64_t length = data.size();
        do {
            auto byte = static_cast<std::uint8_t>(length & 0x7fu);
            length >>= 7u;
            if (length != 0) byte |= 0x80u;
            file.put(static_cast<char>(byte));
        } while (length != 0);
        constexpr std::uint8_t key = 0xa7;
        file.put(static_cast<char>(key));
        for (const auto byte : data)
            file.put(static_cast<char>(static_cast<std::uint8_t>(byte) ^ key));
    };
    segment(
        "fill ~31 ~31 ~31 ~0 ~0 ~0 minecraft:stone []\r\n"
        "setblock ~1 ~2 ~3 minecraft:dirt [\"dirt_type\"=\"normal\"]\r\n");
    segment("[]");
    return path;
}

std::filesystem::path write_truncated_large_ibimport_sample()
{
    const auto path = std::filesystem::temp_directory_path() /
        "water_structure_cpp_large_truncated_test.ibi";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write("IBImport ", 9);
    std::uint64_t length = 300ull * 1024ull * 1024ull;
    do {
        auto byte = static_cast<std::uint8_t>(length & 0x7fu);
        length >>= 7u;
        if (length != 0) byte |= 0x80u;
        file.put(static_cast<char>(byte));
    } while (length != 0);
    file.put(static_cast<char>(0x5a));
    file.write("short", 5);
    return path;
}

std::vector<std::uint8_t> compress_zlib(
    std::span<const std::uint8_t> input, int window_bits)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
        window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("initialize zlib fixture compressor");
    }
    struct Guard {
        z_stream* stream;
        ~Guard() { deflateEnd(stream); }
    } guard{ &stream };
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    std::vector<std::uint8_t> output(deflateBound(&stream, static_cast<uLong>(input.size())));
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    const auto status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) throw std::runtime_error("compress zlib fixture");
    output.resize(stream.total_out);
    return output;
}

std::filesystem::path write_tibi_sample(bool truncated)
{
    std::vector<std::uint8_t> payload;
    auto varint = [&](std::uint64_t value) {
        do {
            auto byte = static_cast<std::uint8_t>(value & 0x7fu);
            value >>= 7u;
            if (value != 0) byte |= 0x80u;
            payload.push_back(byte);
        } while (value != 0);
    };
    auto string = [&](std::string_view value) {
        varint(value.size());
        payload.insert(payload.end(), value.begin(), value.end());
    };
    varint(2);
    varint(0); string("minecraft:stone");
    varint(1); string("minecraft:dirt");
    varint(1);
    varint(0); string("");
    varint(3);
    varint(0); varint(0); varint(5); varint(7); varint(9); varint(0);
    varint(1); varint(1); varint(6); varint(7); varint(9);
    varint(7); varint(8); varint(10); varint(0);
    varint(1); varint(0); varint(10); varint(7); varint(9);
    varint(8); varint(7); varint(9); varint(0);

    const std::string header = "TIBI-HEADER-001";
    std::vector<std::uint8_t> key_material(header.begin(), header.end());
    const auto suffix = std::string("TIBI_2025/5/19-Start") + std::to_string(payload.size());
    key_material.insert(key_material.end(), suffix.begin(), suffix.end());
    const auto key = fixture_md5_portable(key_material);
    std::vector<std::uint8_t> decoded(header.begin(), header.end());
    for (std::size_t index = 0; index < payload.size(); ++index) {
        decoded.push_back(payload[index] ^ key[index % key.size()]);
    }
    auto compressed = compress_zlib(decoded, -MAX_WBITS);
    if (truncated) compressed.pop_back();
    const auto path = std::filesystem::temp_directory_path() /
        (truncated ? "water_structure_cpp_tibi_truncated.tibi" :
            "water_structure_cpp_tibi.tibi");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(compressed.data()),
        static_cast<std::streamsize>(compressed.size()));
    return path;
}

std::string mianyang_json_text()
{
    nlohmann::json document;
    document["namespaces"] = { "minecraft:stone", "minecraft:chest" };
    document["chunkedBlocks"] = nlohmann::json::array({
        {
            { "startX", -17 },
            { "startZ", 32 },
            { "blocks", nlohmann::json::array({
                nlohmann::json::array({ 0, 0, 1, -2, 0 }),
                nlohmann::json::array({
                    1, 0, 2, -1, 1,
                    R"({"blockCompleteNBT":"%7Bid%3A%22Chest%22%2CCount%3A1b%2CNums%3A%5BI%3B1%2C2%5D%7D"})"
                })
            }) }
        }
    });
    return document.dump();
}

std::filesystem::path write_mianyang_v1_sample()
{
    const auto path = std::filesystem::temp_directory_path() / "water_structure_cpp_mianyang_v1.json";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << mianyang_json_text();
    return path;
}

std::filesystem::path write_mianyang_v3_sample()
{
    const auto path = std::filesystem::temp_directory_path() / "water_structure_cpp_mianyang_v3.building";
    const auto text = mianyang_json_text();
    const auto compressed = compress_zlib(std::span{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }, MAX_WBITS);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    return path;
}

void append_le16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_le32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::vector<std::uint8_t> mianyang_v4_decoded()
{
    std::vector<std::uint8_t> decoded;
    append_le32(decoded, static_cast<std::uint32_t>(-20));
    append_le32(decoded, static_cast<std::uint32_t>(-5));
    append_le32(decoded, 100);
    append_le32(decoded, 4);
    append_le32(decoded, 3);
    append_le32(decoded, 5);
    append_le32(decoded, 2);
    append_le32(decoded, 2);
    for (const std::string_view name : { "minecraft:stone", "minecraft:chest" }) {
        append_le16(decoded, static_cast<std::uint16_t>(name.size()));
        decoded.insert(decoded.end(), name.begin(), name.end());
    }
    append_le16(decoded, static_cast<std::uint16_t>(-1));
    append_le16(decoded, static_cast<std::uint16_t>(-1));
    append_le16(decoded, static_cast<std::uint16_t>(-1));
    decoded.insert(decoded.end(), { 0, 0 });
    append_le32(decoded, 0);
    append_le32(decoded, 0);

    append_le16(decoded, 2);
    append_le16(decoded, 1);
    append_le16(decoded, static_cast<std::uint16_t>(-3));
    decoded.insert(decoded.end(), { 1, 0 });
    const std::string nbt = R"({"id":"Chest","DoubleValue":1})";
    append_le32(decoded, static_cast<std::uint32_t>(nbt.size()));
    decoded.insert(decoded.end(), nbt.begin(), nbt.end());
    append_le32(decoded, 3);
    decoded.insert(decoded.end(), { 1, 2, 3 });
    return decoded;
}

std::filesystem::path write_mianyang_v4_sample(bool truncated)
{
    auto decoded = mianyang_v4_decoded();
    if (truncated) decoded.resize(decoded.size() - 5);
    const auto compressed = compress_zlib(decoded, MAX_WBITS + 16);
    const auto path = std::filesystem::temp_directory_path() /
        (truncated ? "water_structure_cpp_mianyang_v4_truncated.buildingX" :
            "water_structure_cpp_mianyang_v4.buildingX");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    return path;
}

std::filesystem::path write_gangban_json(
    std::string_view filename, const nlohmann::json& document, bool compressed = false)
{
    const auto path = std::filesystem::temp_directory_path() / filename;
    const auto text = document.dump();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("create GangBan fixture");
    if (compressed) {
        const auto bytes = compress_zlib(std::span{
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }, MAX_WBITS);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    } else {
        file << text;
    }
    return path;
}

std::array<std::filesystem::path, 7> write_gangban_samples()
{
    const auto palette = nlohmann::json::array({ "minecraft:stone", "minecraft:air" });

    auto v1 = nlohmann::json::array();
    v1.push_back({
        { "id", 0 }, { "aux", 0 }, { "p", { -2, 4, 3 } },
        { "cmds", {
            { "mode", "chain" }, { "auto", true }, { "condition", false },
            { "cmd", "say v1" }, { "last", "" }, { "name", "test" },
            { "tick", 2 }, { "should", true }, { "on", false }
        } }
    });
    v1.push_back({ { "id", 1 }, { "p", { -1, 4, 3 } } });
    v1.push_back({ { "start", { -2, 4, 3 } }, { "end", { -1, 4, 3 } } });
    v1.push_back({ { "list", palette } });

    auto v2 = nlohmann::json::array();
    v2.push_back({ { "id", 0 }, { "aux", 0 }, { "p", { 10, -2, 20 } } });
    v2.push_back({ { "id", 1 }, { "aux", 0 }, { "p", { 11, -2, 20 } } });
    v2.push_back({ { "list", palette } });

    auto v3 = nlohmann::json::array();
    v3.push_back({
        { "name", "v3" }, { "x", -2 }, { "y", 4 }, { "z", 3 },
        { "xcha", 2 }, { "ycha", 1 }, { "zcha", 1 }
    });
    v3.push_back("[0]minecraft:stone[0][1]minecraft:air[1]");
    v3.push_back({
        { "id", 0 }, { "grids", { { "x", -2 }, { "z", 3 }, { "x1", 0 }, { "z1", 0 } } },
        { "data", nlohmann::json::array({
            nlohmann::json::array({ 0, 0, 0, 4, 0, "nbt", "{id:\"Chest\"}" }),
            nlohmann::json::array({ 1, 0, 1, 4, 0 }),
            // Fatalder preserves V3 records immediately beyond a declared
            // header extent without expanding GetSize().
            nlohmann::json::array({ 0, 0, 0, 4, 1 })
        }) }
    });

    auto v4 = nlohmann::json::array();
    v4.push_back({ { "name", "v4" }, { "xcha", 2 }, { "ycha", 1 }, { "zcha", 1 } });
    v4.push_back(palette);
    v4.push_back({
        { "id", 0 }, { "grids", { { "x", -2 }, { "z", 3 } } },
        { "data", nlohmann::json::array({
            nlohmann::json::array({ 0, 0, 0, 4, 0, "nbt", "{id:\"Chest\"}" }),
            nlohmann::json::array({ 1, 0, 1, 4, 0 })
        }) }
    });

    auto v5 = nlohmann::json::array();
    for (const auto& value : nlohmann::json::array({ 0, -2, 4, 3, 0, 0 })) v5.push_back(value);
    v5.push_back({
        { "cmd", "say v5" }, { "name", "test" }, { "delay", 2 },
        { "auto", true }, { "condition", false }
    });
    v5.push_back({ { "ep", { 0, 0, 0 } } });
    v5.push_back(palette);

    auto v6 = nlohmann::json::array();
    v6.push_back(nlohmann::json::array({
        -2, 4, 3, 0, 0,
        nlohmann::json{
            { "cmd", "say v6" }, { "name", "test" }, { "delay", 2 },
            { "auto", true }, { "condition", false }
        }
    }));
    v6.push_back(nlohmann::json::array({ 1, 0, 0, 1, 0 }));
    v6.push_back(palette);

    return {
        write_gangban_json("water_structure_cpp_gangban_v1.json", v1),
        write_gangban_json("water_structure_cpp_gangban_v2.json", v2),
        write_gangban_json("water_structure_cpp_gangban_v3.json", v3),
        write_gangban_json("water_structure_cpp_gangban_v4.json", v4),
        write_gangban_json("water_structure_cpp_gangban_v5.json", v5),
        write_gangban_json("water_structure_cpp_gangban_v6.json", v6),
        write_gangban_json("water_structure_cpp_gangban_v7.reb", v6, true)
    };
}

std::filesystem::path write_qingxu_sample(bool invalid)
{
    nlohmann::json root;
    root["totalBlocks"] = 1;
    if (invalid) {
        root["0"] = R"({"totalPoints":1,"0":)";
    } else {
        nlohmann::json chunk;
        chunk["totalPoints"] = 3;
        chunk["0"] = nlohmann::json{
            { "Name", "stone" }, { "X", -1 }, { "Y", 2 }, { "Z", 3 }
        }.dump();
        chunk["1"] = nlohmann::json{
            { "Name", "air" }, { "X", 0 }, { "Y", 2 }, { "Z", 3 }
        }.dump();
        chunk["2"] = nlohmann::json{
            { "Name", "stone" }, { "X", 0 }, { "Y", 2 }, { "Z", 3 }
        }.dump();
        root["0"] = chunk.dump();
    }
    return write_gangban_json(
        invalid ? "water_structure_cpp_qingxu_invalid.json" : "water_structure_cpp_qingxu.json",
        root);
}

std::filesystem::path write_timebuilder_sample(bool invalid_version)
{
    nlohmann::json root;
    root["version"] = invalid_version ? "Other" : "TimeBuilder";
    root["block"] = nlohmann::json::array({
        {
            { "name", "stone" }, { "aux", 0 },
            { "pos", nlohmann::json::array({
                nlohmann::json::array({ -2, 3, 4 }),
                nlohmann::json::array({ -1, 3, 4 }),
                nlohmann::json::array({ 0, 1 })
            }) }
        },
        { { "name", "air" }, { "aux", 0 }, { "pos", nlohmann::json::array({
            nlohmann::json::array({ -1, 3, 4 }) }) } },
        { { "name", "stone" }, { "aux", 0 }, { "pos", nlohmann::json::array({
            nlohmann::json::array({ -1, 3, 4 }) }) } }
    });
    return write_gangban_json(
        invalid_version ? "water_structure_cpp_timebuilder_invalid.json" :
            "water_structure_cpp_timebuilder.json",
        root);
}

std::string append_test_utf8(std::string output, std::uint32_t value)
{
    if (value <= 0x7f) output.push_back(static_cast<char>(value));
    else if (value <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
    return output;
}

std::filesystem::path write_fuhong_v5_sample(
    const nlohmann::json& root, bool truncated)
{
    constexpr std::string_view key = "FuHongBuild";
    const auto plain = root.dump();
    std::string encrypted;
    encrypted.reserve(plain.size());
    for (std::size_t index = 0; index < plain.size(); ++index) {
        const auto value = static_cast<std::uint32_t>(
            static_cast<unsigned char>(plain[index]) ^
            static_cast<unsigned char>(key[index % key.size()])) + index % 3;
        encrypted = append_test_utf8(std::move(encrypted), value);
    }
    auto compressed = compress_zlib(std::span{
        reinterpret_cast<const std::uint8_t*>(encrypted.data()), encrypted.size() }, MAX_WBITS);
    if (truncated) compressed.resize(compressed.size() - 2);
    const auto path = std::filesystem::temp_directory_path() /
        (truncated ? "water_structure_cpp_fuhong_v5_truncated.fhbuild" :
            "water_structure_cpp_fuhong_v5.fhbuild");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(compressed.data()),
        static_cast<std::streamsize>(compressed.size()));
    return path;
}

std::array<std::filesystem::path, 5> write_fuhong_samples()
{
    auto v1 = nlohmann::json::array({
        { { "name", "minecraft:stone" }, { "aux", 0 },
            { "x", nlohmann::json::array({ -2, 99 }) }, { "y", 3 }, { "z", 4 } },
        { { "name", "minecraft:air" }, { "aux", 0 }, { "x", -1 }, { "y", 3 }, { "z", 4 } },
        { { "name", "minecraft:stone" }, { "aux", 0 }, { "x", -1 }, { "y", 3 }, { "z", 4 } }
    });

    nlohmann::json v2;
    v2["Build_Info"] = nlohmann::json::object();
    v2["FuHongBuild_FinalFormat"] = nlohmann::json::array({
        { { "block", nlohmann::json::array({
            {
                { "n", "minecraft:chest" }, { "a", nlohmann::json::array({ 0 }) },
                { "x", nlohmann::json::array({ -1 }) }, { "y", nlohmann::json::array({ 2 }) },
                { "z", nlohmann::json::array({ 3 }) },
                { "d", nlohmann::json::array({ {
                    { "d", nlohmann::json::array({ {
                        { "name", "stone" }, { "damage", 0 }, { "count", 1 }, { "slot", 0 }
                    } }) }
                } }) }
            },
            {
                { "n", "minecraft:command_block" }, { "a", 0 },
                { "x", nlohmann::json::array({ 0 }) }, { "y", nlohmann::json::array({ 2 }) },
                { "z", nlohmann::json::array({ 3 }) },
                { "state", nlohmann::json::array({ "conditional_bit=true" }) },
                { "c", {
                    { "c", nlohmann::json::array({ "say v2" }) },
                    { "t", nlohmann::json::array({ 2 }) },
                    { "a", nlohmann::json::array({ true }) },
                    { "n", nlohmann::json::array({ "Tester" }) }
                } }
            }
        }) } }
    });

    nlohmann::json v3;
    v3["BlockCalculationPos"] = true;
    v3["BlocksList"] = { "minecraft:air", "minecraft:stone", "minecraft:chest" };
    v3["FuHongBuild"] = nlohmann::json::array({ {
        { "startX", -16 }, { "startZ", 32 },
        { "block", nlohmann::json::array({
            nlohmann::json::array({ 1, 0, nlohmann::json::array({ 0 }),
                nlohmann::json::array({ 1 }), nlohmann::json::array({ 0 }) }),
            nlohmann::json::array({ 2, 0, nlohmann::json::array({ 1 }),
                nlohmann::json::array({ 1 }), nlohmann::json::array({ 0 }),
                nlohmann::json::array({ nlohmann::json::array({
                    nlohmann::json::array({ "stone", 0, 1, 0 }) }) }) })
        }) }
    } });

    auto v4 = v3;
    v4.erase("BlockCalculationPos");
    v4["FuHongBuild"][0]["startX"] = 0;
    v4["FuHongBuild"][0]["startZ"] = 0;
    v4["FuHongBuild"][0]["block"][0][2][0] = -2;
    v4["FuHongBuild"][0]["block"][1][2][0] = -1;

    return {
        write_gangban_json("water_structure_cpp_fuhong_v1.json", v1),
        write_gangban_json("water_structure_cpp_fuhong_v2.json", v2),
        write_gangban_json("water_structure_cpp_fuhong_v3.json", v3),
        write_gangban_json("water_structure_cpp_fuhong_v4.json", v4),
        write_fuhong_v5_sample(v4, false)
    };
}

std::filesystem::path write_msgpack_structure_sample(
    water_structure::StructureId format, bool truncated)
{
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(buffer);
    if (format == water_structure::StructureId::BDS) {
        packer.pack_array(1);
        packer.pack_array(3);
        packer.pack_array(6); packer.pack("minecraft:stone");
        packer.pack(-2); packer.pack(3); packer.pack(4); packer.pack(0); packer.pack(false);
        packer.pack_array(6); packer.pack("minecraft:air");
        packer.pack(-1); packer.pack(3); packer.pack(4); packer.pack(0); packer.pack(true);
        packer.pack_array(6); packer.pack("minecraft:dirt");
        packer.pack(-1); packer.pack(3); packer.pack(4); packer.pack(0); packer.pack(false);
    } else {
        packer.pack_array(2);
        packer.pack_array(2);
        packer.pack_array(5); packer.pack("minecraft:stone");
        packer.pack(-2); packer.pack(3); packer.pack(4); packer.pack(0);
        packer.pack_array(5); packer.pack("minecraft:dirt");
        packer.pack(-1); packer.pack(3); packer.pack(4); packer.pack(0);
        packer.pack_array(0);
    }
    const auto filename = format == water_structure::StructureId::BDS
        ? (truncated ? "water_structure_cpp_bds_truncated.bds" : "water_structure_cpp_bds.bds")
        : (truncated ? "water_structure_cpp_nexus_truncated.np" : "water_structure_cpp_nexus.np");
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const auto length = static_cast<std::streamsize>(buffer.size() - (truncated ? 1 : 0));
    file.write(buffer.data(), length);
    return path;
}

void preserve_msgpack_fixture(const std::filesystem::path& path)
{
    const auto* output = std::getenv("WATER_STRUCTURE_MSGPACK_FIXTURE_DIR");
    if (output == nullptr || *output == '\0') return;
    const auto directory = std::filesystem::path(output);
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(path, directory / path.filename(),
        std::filesystem::copy_options::overwrite_existing);
}

void preserve_benchmark_fixture(const std::filesystem::path& path)
{
    const auto* output = std::getenv("WATER_STRUCTURE_BENCH_FIXTURE_DIR");
    if (output == nullptr || *output == '\0') return;
    const auto directory = std::filesystem::path(output);
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(path, directory / path.filename(),
        std::filesystem::copy_options::overwrite_existing);
}

std::filesystem::path write_bcf_sample(bool truncated, bool zero_z_span = false)
{
    std::vector<std::uint8_t> bytes{ 'B', 'C', 'F', 1 };
    auto u16 = [&](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    };
    auto u32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    auto u64 = [&](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    auto patch_u64 = [&](std::size_t position, std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes[position + shift / 8] = static_cast<std::uint8_t>(value >> shift);
    };
    auto string16 = [&](std::string_view value) {
        u16(static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    };

    u16(2); u16(1); u16(1); bytes.push_back(16); u64(1);
    const auto pointer_position = bytes.size();
    for (int index = 0; index < 5; ++index) u64(0);

    const auto section_offset = bytes.size();
    u64(50); u16(static_cast<std::uint16_t>(-2)); u16(3); u16(4); u32(2);
    u32(1); for (int index = 0; index < 6; ++index) u16(0);
    u32(2); u16(1); u16(0); u16(zero_z_span ? 1 : 0); u16(1); u16(0); u16(0);

    const auto offsets_offset = bytes.size();
    u64(1); u64(section_offset);
    const auto type_map_offset = bytes.size();
    u32(2); u16(1); string16("minecraft:stone"); u16(2); string16("minecraft:dirt");
    const auto state_name_offset = bytes.size(); u32(0);
    const auto state_value_offset = bytes.size(); u32(0);
    const auto palette_offset = bytes.size();
    u32(2); u32(1); u16(1); u16(0); u32(2); u16(2); u16(0);

    patch_u64(pointer_position + 0, offsets_offset);
    patch_u64(pointer_position + 8, palette_offset);
    patch_u64(pointer_position + 16, type_map_offset);
    patch_u64(pointer_position + 24, state_name_offset);
    patch_u64(pointer_position + 32, state_value_offset);
    if (truncated) bytes.pop_back();
    const auto path = std::filesystem::temp_directory_path() /
        (truncated ? "water_structure_cpp_bcf_truncated.bcf" :
            zero_z_span ? "water_structure_cpp_bcf_zero_z_span.bcf" :
                "water_structure_cpp_bcf.bcf");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::filesystem::path write_covstructure_sample(bool invalid)
{
    const auto path = std::filesystem::temp_directory_path() /
        (invalid ? "water_structure_cpp_cov_invalid.covstructure" :
            "water_structure_cpp_cov.covstructure");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (invalid) {
        file << R"({"size":[2,1,1],"structure":)";
        return path;
    }
    nlohmann::json root;
    root["size"] = { 2, 1, 1 };
    root["structure"] = {
        { "palette", nlohmann::json::array({
            { { "val", 0 }, { "name", "minecraft:stone" }, { "data", 0 } },
            { { "val", 1 }, { "name", "minecraft:air" } },
            { { "val", 2 }, { "name", "minecraft:dirt" }, { "data", 0 } }
        }) },
        { "block_indices", nlohmann::json::array({
            nlohmann::json::array({ nlohmann::json::array({ 0, 1, 2 }) })
        }) }
    };
    file << root.dump();
    return path;
}

std::string gzip_nbt(const nbt::tag_compound& root)
{
    std::ostringstream output(std::ios::binary);
    zlib::ozlibstream compressed(output, Z_DEFAULT_COMPRESSION, true);
    nbt::io::write_tag("", root, compressed, endian::big);
    compressed.close();
    return output.str();
}

std::filesystem::path write_axiom_sample(bool truncated)
{
    auto palette_entry = [](std::string name, nbt::tag_compound properties) {
        nbt::tag_compound entry;
        entry.put("Name", nbt::value_initializer(nbt::tag_string(std::move(name))));
        entry["Properties"] = std::move(properties);
        return entry;
    };
    nbt::tag_list palette;
    palette.push_back(nbt::value_initializer(
        palette_entry("minecraft:air", nbt::tag_compound{})));
    nbt::tag_compound lever_properties;
    lever_properties.emplace<nbt::tag_string>("face", "ceiling");
    lever_properties.emplace<nbt::tag_string>("facing", "east");
    lever_properties.emplace<nbt::tag_string>("powered", "true");
    palette.push_back(nbt::value_initializer(
        palette_entry("minecraft:lever", std::move(lever_properties))));

    std::vector<std::int64_t> packed(256, 0);
    packed[0] = 1;
    nbt::tag_compound block_states;
    block_states["palette"] = std::move(palette);
    block_states["data"] = nbt::tag_long_array(std::move(packed));
    nbt::tag_compound region;
    region.emplace<nbt::tag_int>("X", 0);
    region.emplace<nbt::tag_int>("Y", 0);
    region.emplace<nbt::tag_int>("Z", 0);
    region["BlockStates"] = std::move(block_states);
    nbt::tag_list regions;
    regions.push_back(nbt::value_initializer(std::move(region)));
    nbt::tag_compound root;
    root["BlockRegion"] = std::move(regions);
    const auto payload = gzip_nbt(root);

    std::vector<std::uint8_t> bytes;
    auto write_be_i32 = [&](std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    write_be_i32(0x0AE5BB36);
    write_be_i32(0);
    write_be_i32(0);
    write_be_i32(static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (truncated) bytes.pop_back();

    const auto path = std::filesystem::temp_directory_path() /
        (truncated ? "water_structure_cpp_axiom_truncated.bp" :
            "water_structure_cpp_axiom.bp");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::filesystem::path write_construction_sample(bool truncated)
{
    nbt::tag_compound entity_nbt;
    entity_nbt.emplace<nbt::tag_byte>("isMovable", 1);
    nbt::tag_compound entity;
    entity.emplace<nbt::tag_int>("x", 10);
    entity.emplace<nbt::tag_int>("y", 20);
    entity.emplace<nbt::tag_int>("z", 30);
    entity.emplace<nbt::tag_string>("namespace", "minecraft");
    entity.emplace<nbt::tag_string>("base_name", "Chest");
    entity["nbt"] = std::move(entity_nbt);
    nbt::tag_list entities;
    entities.push_back(nbt::value_initializer(std::move(entity)));

    nbt::tag_compound section;
    section.emplace<nbt::tag_byte>("blocks_array_type", 7);
    section["blocks"] = nbt::tag_byte_array(std::vector<std::int8_t>{ 1, 0 });
    section["shape"] = nbt::tag_list{ std::int32_t{ 2 }, std::int32_t{ 1 }, std::int32_t{ 1 } };
    section["block_entities"] = std::move(entities);
    const auto section_bytes = gzip_nbt(section);

    std::vector<std::int8_t> index(23, 0);
    auto write_le_i32 = [&](std::size_t offset, std::int32_t value) {
        const auto encoded = static_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            index[offset + shift / 8] = static_cast<std::int8_t>(encoded >> shift);
        }
    };
    write_le_i32(0, 10);
    write_le_i32(4, 20);
    write_le_i32(8, 30);
    index[12] = 2;
    index[13] = 1;
    index[14] = 1;
    write_le_i32(15, 9);
    write_le_i32(19, static_cast<std::int32_t>(section_bytes.size()));

    auto palette_entry = [](std::string name) {
        nbt::tag_compound entry;
        entry.emplace<nbt::tag_string>("namespace", "minecraft");
        entry.put("blockname", nbt::value_initializer(nbt::tag_string(std::move(name))));
        entry["properties"] = nbt::tag_compound{};
        return entry;
    };
    nbt::tag_list palette;
    palette.push_back(nbt::value_initializer(palette_entry("air")));
    palette.push_back(nbt::value_initializer(palette_entry("stone")));
    nbt::tag_compound metadata;
    metadata["selection_boxes"] = nbt::tag_int_array(
        std::vector<std::int32_t>{ 10, 20, 30, 12, 21, 31 });
    metadata["block_palette"] = std::move(palette);
    metadata["section_index_table"] = nbt::tag_byte_array(std::move(index));
    metadata.emplace<nbt::tag_byte>("section_version", 0);
    const auto metadata_bytes = gzip_nbt(metadata);

    std::vector<std::uint8_t> bytes;
    const std::string_view magic = "constrct";
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    bytes.push_back(0);
    bytes.insert(bytes.end(), section_bytes.begin(), section_bytes.end());
    const auto metadata_offset = static_cast<std::uint32_t>(bytes.size());
    bytes.insert(bytes.end(), metadata_bytes.begin(), metadata_bytes.end());
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(metadata_offset >> shift));
    }
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    if (truncated) bytes.pop_back();

    const auto path = std::filesystem::temp_directory_path() /
        (truncated ? "water_structure_cpp_construction_truncated.construction" :
            "water_structure_cpp_construction.construction");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

void preserve_mianyang_fixture(const std::filesystem::path& path)
{
    const auto* output = std::getenv("WATER_STRUCTURE_MIANYANG_FIXTURE_DIR");
    if (output == nullptr || *output == '\0') return;
    const auto directory = std::filesystem::path(output);
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(path, directory / path.filename(),
        std::filesystem::copy_options::overwrite_existing);
}

std::filesystem::path find_mapping_asset()
{
    auto directory = std::filesystem::current_path();
    for (int i = 0; i < 7 && !directory.empty(); ++i) {
        const auto candidate = directory / "assets" / "block_mappings_v1.json";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
        directory = directory.parent_path();
    }
    return {};
}

void test_zip_round_trip()
{
    const auto unique = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("water_structure_cpp_zip_" + unique);
    const auto source = root / "source";
    const auto extracted = root / "extracted";
    const auto archive_path = root / "world.mcworld";
    std::filesystem::create_directories(source / "db");
    {
        std::ofstream level(source / "level.dat", std::ios::binary);
        level << "level";
        std::ofstream data(source / "db" / "CURRENT", std::ios::binary);
        data << "MANIFEST-000001\n";
        std::ofstream empty(source / "db" / "LOCK", std::ios::binary);
    }
    const auto packed = water_structure::archive::create_zip(source, archive_path);
    check(packed.ok(), "mcworld pack");
    const auto unpacked = water_structure::archive::extract_zip(archive_path, extracted);
    check(unpacked.ok(), "mcworld extract");
    std::ifstream round_trip(extracted / "db" / "CURRENT", std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(round_trip)), std::istreambuf_iterator<char>());
    check(contents == "MANIFEST-000001\n", "mcworld zip contents");
    check(std::filesystem::is_regular_file(extracted / "db" / "LOCK") &&
        std::filesystem::file_size(extracted / "db" / "LOCK") == 0,
        "mcworld zip empty file");
    round_trip.close();
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    check(!cleanup_error, "mcworld zip cleanup");
}

void test_zip_streaming_entry()
{
    // This payload is intentionally larger than the archive implementation's
    // 64 KiB transfer window.  The test exercises both bounded inflate input
    // and bounded output writes without requiring an entry-sized buffer.
    const auto unique = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("water_structure_cpp_zip_stream_" + unique);
    const auto source = root / "source";
    const auto extracted = root / "extracted";
    const auto archive_path = root / "stream.mcworld";
    std::filesystem::create_directories(source);
    const auto source_file = source / "large.bin";
    constexpr std::size_t payload_size = 8 * 1024 * 1024 + 123;
    std::vector<std::uint8_t> pattern(64 * 1024);
    std::uint32_t random_state = 0x6d2b79f5u;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        random_state ^= random_state << 13u;
        random_state ^= random_state >> 17u;
        random_state ^= random_state << 5u;
        pattern[i] = static_cast<std::uint8_t>(random_state & 0xffu);
    }
    {
        std::ofstream output(source_file, std::ios::binary | std::ios::trunc);
        check(output.good(), "create large ZIP source");
        std::size_t remaining = payload_size;
        while (remaining != 0) {
            const auto count = std::min<std::size_t>(remaining, pattern.size());
            output.write(reinterpret_cast<const char*>(pattern.data()),
                static_cast<std::streamsize>(count));
            check(output.good(), "write large ZIP source");
            remaining -= count;
        }
    }
    const auto packed = water_structure::archive::create_zip(source, archive_path);
    check(packed.ok(), "stream ZIP pack");
    const auto unpacked = water_structure::archive::extract_zip(archive_path, extracted);
    check(unpacked.ok(), "stream ZIP extract");
    const auto output_file = extracted / "large.bin";
    check(std::filesystem::is_regular_file(output_file) &&
        std::filesystem::file_size(output_file) == payload_size,
        "stream ZIP output size");
    {
        std::ifstream input(output_file, std::ios::binary);
        check(input.good(), "open stream ZIP output");
        std::size_t remaining = payload_size;
        std::vector<std::uint8_t> actual(pattern.size());
        while (remaining != 0) {
            const auto count = std::min<std::size_t>(remaining, pattern.size());
            input.read(reinterpret_cast<char*>(actual.data()),
                static_cast<std::streamsize>(count));
            check(input.gcount() == static_cast<std::streamsize>(count),
                "read stream ZIP output");
            for (std::size_t i = 0; i < count; ++i) {
                check(actual[i] == pattern[i], "stream ZIP output contents");
            }
            remaining -= count;
        }
    }

    // Corrupt the central-directory CRC and ensure extraction fails after
    // streaming the entry instead of silently committing bad output.
    const auto read_bytes = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto write_bytes = [](const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        return output.good();
    };
    const auto find_signature = [](const std::vector<std::uint8_t>& bytes, std::uint32_t signature) {
        if (bytes.size() < 4) return std::string::npos;
        for (std::size_t i = bytes.size() - 4;; --i) {
            const auto value = static_cast<std::uint32_t>(bytes[i]) |
                (static_cast<std::uint32_t>(bytes[i + 1]) << 8u) |
                (static_cast<std::uint32_t>(bytes[i + 2]) << 16u) |
                (static_cast<std::uint32_t>(bytes[i + 3]) << 24u);
            if (value == signature) return i;
            if (i == 0) break;
        }
        return std::string::npos;
    };
    const auto archive_bytes = read_bytes(archive_path);
    const auto central_offset = find_signature(archive_bytes, 0x02014b50u);
    check(central_offset != std::string::npos, "find ZIP central directory");
    const auto eocd_offset = find_signature(archive_bytes, 0x06054b50u);
    check(eocd_offset != std::string::npos, "find ZIP EOCD");
    auto zip64_marker = archive_bytes;
    zip64_marker[eocd_offset + 8] = zip64_marker[eocd_offset + 9] = 0xffu;
    zip64_marker[eocd_offset + 10] = zip64_marker[eocd_offset + 11] = 0xffu;
    const auto zip64_marker_path = root / "zip64-marker.mcworld";
    check(write_bytes(zip64_marker_path, zip64_marker), "write ZIP64 marker");
    const auto zip64_result = water_structure::archive::extract_zip(
        zip64_marker_path, root / "zip64-out");
    check(!zip64_result.ok() && zip64_result.error().find("ZIP64") != std::string::npos,
        "ZIP64 marker explicitly rejected");
    auto corrupt_crc = archive_bytes;
    corrupt_crc[central_offset + 16] ^= 0xffu;
    const auto corrupt_crc_path = root / "corrupt-crc.mcworld";
    check(write_bytes(corrupt_crc_path, corrupt_crc), "write corrupt ZIP CRC");
    const auto corrupt_crc_result = water_structure::archive::extract_zip(
        corrupt_crc_path, root / "corrupt-crc-out");
    check(!corrupt_crc_result.ok() &&
        corrupt_crc_result.error().find("CRC32") != std::string::npos,
        "corrupt ZIP CRC rejected");
    check(!std::filesystem::exists(root / "corrupt-crc-out" / "large.bin"),
        "corrupt ZIP output removed");

    auto truncated = archive_bytes;
    truncated.resize(truncated.size() - 1);
    const auto truncated_path = root / "truncated.mcworld";
    check(write_bytes(truncated_path, truncated), "write truncated ZIP");
    const auto truncated_result = water_structure::archive::extract_zip(
        truncated_path, root / "truncated-out");
    check(!truncated_result.ok(), "truncated ZIP rejected");

    // A name with parent traversal must be rejected before any output file is
    // opened.  Keep the replacement name the same length as the original so
    // the test only changes ZIP name bytes, not offsets or descriptors.
    const auto traversal_source = root / "traversal-source";
    std::filesystem::create_directories(traversal_source);
    {
        std::ofstream output(traversal_source / "payload.bin", std::ios::binary | std::ios::trunc);
        output << "payload";
    }
    const auto traversal_archive = root / "traversal.mcworld";
    check(water_structure::archive::create_zip(traversal_source, traversal_archive).ok(),
        "create traversal ZIP");
    auto traversal_bytes = read_bytes(traversal_archive);
    const auto local_offset = find_signature(traversal_bytes, 0x04034b50u);
    const auto traversal_central_offset = find_signature(traversal_bytes, 0x02014b50u);
    constexpr std::string_view traversal_name = "../evil.bin";
    check(local_offset != std::string::npos &&
        traversal_central_offset != std::string::npos &&
        traversal_name.size() == 11,
        "find traversal ZIP headers");
    std::copy(traversal_name.begin(), traversal_name.end(),
        traversal_bytes.begin() + local_offset + 30);
    std::copy(traversal_name.begin(), traversal_name.end(),
        traversal_bytes.begin() + traversal_central_offset + 46);
    check(write_bytes(traversal_archive, traversal_bytes), "write traversal ZIP");
    const auto traversal_result = water_structure::archive::extract_zip(
        traversal_archive, root / "traversal-out");
    check(!traversal_result.ok() &&
        traversal_result.error().find("路径不安全") != std::string::npos,
        "traversal ZIP rejected");
    check(!std::filesystem::exists(root / "evil.bin"), "traversal ZIP does not escape destination");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    check(!cleanup_error, "stream ZIP cleanup");
}

void test_new_mcworld_archive()
{
    const auto unique = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("water_structure_cpp_new_mcworld_" + unique);
    const auto archive_path = root / "created.mcworld";
    const auto extracted = root / "extracted";
    std::filesystem::create_directories(root);

    auto world = water_structure::BedrockWorldAdapter::open(archive_path);
    check(world.ok(), "missing .mcworld opens as archive output");
    check(world.ok() && std::filesystem::is_directory(world.value().directory()),
        "new .mcworld uses a temporary world directory");
    if (world) {
        const auto closed = world.value().close();
        check(closed.ok(), "new .mcworld closes and packs");
    }
    check(std::filesystem::is_regular_file(archive_path),
        "new .mcworld output is a regular archive, not a directory");
    const auto unpacked = water_structure::archive::extract_zip(archive_path, extracted);
    check(unpacked.ok(), "new .mcworld archive extracts");
    check(std::filesystem::is_regular_file(extracted / "level.dat") &&
        std::filesystem::is_directory(extracted / "db"),
        "new .mcworld contains Bedrock world metadata and LevelDB");
    const auto no_spool_input = water_structure::BedrockWorldAdapter::open(
        archive_path, water_structure::WorldOpenOptions{
            .write_back_archive = false,
            .allow_temporary_spool = false
        });
    check(!no_spool_input.ok() &&
        no_spool_input.error().find("allow_temporary_spool=false") != std::string::npos,
        ".mcworld input rejects disabled temporary spool");
    const auto no_spool_output_path = root / "no-spool.mcworld";
    const auto no_spool_output = water_structure::BedrockWorldAdapter::open(
        no_spool_output_path, water_structure::WorldOpenOptions{
            .allow_temporary_spool = false
        });
    check(!no_spool_output.ok() && !std::filesystem::exists(no_spool_output_path),
        ".mcworld output rejects disabled temporary spool without creating a file");
    const auto limited_extract = water_structure::archive::extract_zip(
        archive_path, root / "limited-extract", 1);
    check(!limited_extract.ok() &&
        limited_extract.error().find("temporary_file_limit_bytes") != std::string::npos,
        ".mcworld extraction enforces the declared spool limit");
    const auto limited_output_path = root / "limited-output.mcworld";
    auto limited_output = water_structure::BedrockWorldAdapter::open(
        limited_output_path, water_structure::WorldOpenOptions{
            .temporary_file_limit_bytes = 1
        });
    check(limited_output.ok(), "limited .mcworld output opens transactionally");
    if (limited_output) {
        const auto limited_close = limited_output.value().close();
        check(!limited_close.ok() && !std::filesystem::exists(limited_output_path),
            ".mcworld output limit aborts before atomic replace");
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    check(!cleanup_error, "new .mcworld cleanup");
}

class TestStructure final : public water_structure::IStructure {
public:
    water_structure::StructureId id() const noexcept override { return water_structure::StructureId::Unknown; }
    std::string_view name() const noexcept override { return "test"; }
    water_structure::Size size() const noexcept override { return { 16, 16, 16 }; }
    water_structure::BlockPos offset() const noexcept override { return {}; }
    void set_offset(water_structure::BlockPos) noexcept override {}
    water_structure::Result<void> read(const std::filesystem::path&) override {
        return water_structure::Result<void>::failure("unused");
    }
    water_structure::Result<water_structure::ChunkMap> get_chunks(
        std::span<const water_structure::ChunkPos> positions) const override
    {
        chunk_batch_peak = std::max(chunk_batch_peak, positions.size());
        water_structure::ChunkMap chunks;
        for (const auto pos : positions) {
            water_structure::ChunkData chunk;
            water_structure::SubChunkData sub_chunk;
            sub_chunk.layer0.fill(1);
            sub_chunk.layer1.fill(1);
            sub_chunk.layer0[0] = 7;
            chunk.sub_chunks.emplace(-4, std::move(sub_chunk));
            chunks.emplace(pos, std::move(chunk));
        }
        return water_structure::Result<water_structure::ChunkMap>::success(std::move(chunks));
    }
    water_structure::Result<void> visit_chunks(
        std::span<const water_structure::ChunkPos> positions,
        const water_structure::ChunkVisitor& visitor) const override
    {
        visit_batch_peak = std::max(visit_batch_peak, positions.size());
        auto chunks = get_chunks(positions);
        if (!chunks) return water_structure::Result<void>::failure(chunks.error());
        for (const auto position : positions) {
            const auto found = chunks.value().find(position);
            if (found == chunks.value().end()) continue;
            auto visited = visitor(position, found->second);
            if (!visited) return visited;
            if (fail_after_first_chunk) {
                return water_structure::Result<void>::failure(
                    "intentional structure stream failure");
            }
        }
        return water_structure::Result<void>::success();
    }
    water_structure::Result<water_structure::NbtChunkMap> get_chunk_nbt(
        std::span<const water_structure::ChunkPos> positions) const override
    {
        nbt_batch_peak = std::max(nbt_batch_peak, positions.size());
        water_structure::NbtChunkMap entities;
        for (const auto pos : positions) {
            entities.emplace(pos, std::vector<water_structure::BlockEntity>{});
        }
        return water_structure::Result<water_structure::NbtChunkMap>::success(std::move(entities));
    }
    water_structure::Result<std::size_t> count_non_air_blocks() const override {
        return water_structure::Result<std::size_t>::success(1);
    }
    water_structure::Result<void> write_to_world(
        water_structure::WorldTarget& world,
        water_structure::SubChunkPos start,
        water_structure::ConversionCallbacks callbacks) const override
    {
        return water_structure::convert_to_world(*this, world, start, std::move(callbacks));
    }
    water_structure::Result<void> read_from_world(
        water_structure::WorldSource&,
        water_structure::BlockBox,
        water_structure::ConversionCallbacks) override
    {
        return water_structure::Result<void>::failure("unused");
    }

    mutable std::size_t chunk_batch_peak = 0;
    mutable std::size_t nbt_batch_peak = 0;
    mutable std::size_t visit_batch_peak = 0;
    bool fail_after_first_chunk = false;
};

class TestWorld final : public water_structure::WorldTarget {
public:
    water_structure::Result<void> save_chunk(
        water_structure::ChunkPos pos,
        const water_structure::ChunkData& chunk) override
    {
        saved_pos = pos;
        saved_chunk = chunk;
        return water_structure::Result<void>::success();
    }

    water_structure::Result<void> save_chunk_nbt(
        water_structure::ChunkPos,
        std::span<const water_structure::BlockEntity>) override
    {
        return water_structure::Result<void>::success();
    }

    water_structure::ChunkPos saved_pos{};
    water_structure::ChunkData saved_chunk;
};

class RecordingChunkSink final : public water_structure::ChunkSink {
public:
    water_structure::Result<void> push(
        water_structure::StreamChunk&& chunk) override
    {
        positions.push_back(chunk.position);
        return water_structure::Result<void>::success();
    }

    water_structure::Result<void> finish() override
    {
        finished = true;
        return water_structure::Result<void>::success();
    }

    std::vector<water_structure::ChunkPos> positions;
    bool finished = false;
};

class FailingChunkSink final : public water_structure::ChunkSink {
public:
    water_structure::Result<void> push(water_structure::StreamChunk&&) override
    {
        ++received;
        if (received >= fail_at) {
            return water_structure::Result<void>::failure("intentional sink failure");
        }
        return water_structure::Result<void>::success();
    }

    void cancel() noexcept override { cancelled = true; }

    std::size_t fail_at = 2;
    std::size_t received = 0;
    bool cancelled = false;
};

class BlockEntityStructure final : public water_structure::IStructure {
public:
    explicit BlockEntityStructure(const water_structure::IStructure& source) : mSource(source) {}

    water_structure::StructureId id() const noexcept override { return mSource.id(); }
    std::string_view name() const noexcept override { return mSource.name(); }
    water_structure::Size size() const noexcept override { return mSource.size(); }
    water_structure::BlockPos offset() const noexcept override { return mSource.offset(); }
    void set_offset(water_structure::BlockPos) noexcept override {}
    water_structure::Result<void> read(const std::filesystem::path&) override {
        return water_structure::Result<void>::failure("unused");
    }
    water_structure::Result<water_structure::ChunkMap> get_chunks(
        std::span<const water_structure::ChunkPos> positions) const override
    {
        return mSource.get_chunks(positions);
    }
    water_structure::Result<water_structure::NbtChunkMap> get_chunk_nbt(
        std::span<const water_structure::ChunkPos> positions) const override
    {
        water_structure::NbtChunkMap result;
        for (const auto position : positions) result.emplace(position, std::vector<water_structure::BlockEntity>{});
        if (!positions.empty()) {
            result.at(positions.front()).push_back({ { 1, 2, 3 }, { 10, 0, 0, 0 } });
        }
        return water_structure::Result<water_structure::NbtChunkMap>::success(std::move(result));
    }
    water_structure::Result<std::size_t> count_non_air_blocks() const override {
        return mSource.count_non_air_blocks();
    }
    water_structure::Result<void> write_to_world(
        water_structure::WorldTarget&,
        water_structure::SubChunkPos,
        water_structure::ConversionCallbacks) const override
    {
        return water_structure::Result<void>::failure("unused");
    }
    water_structure::Result<void> read_from_world(
        water_structure::WorldSource&,
        water_structure::BlockBox,
        water_structure::ConversionCallbacks) override
    {
        return water_structure::Result<void>::failure("unused");
    }

private:
    const water_structure::IStructure& mSource;
};
}

int main()
{
    try {
        water_structure::Size size{17, 32, 33};
        check(size.chunk_x_count() == 2, "x chunk count");
        check(size.chunk_z_count() == 3, "z chunk count");
        check(size.volume() == 17 * 32 * 33, "volume");
        check(water_structure::floor_div(15, 16) == 0, "positive floor_div");
        check(water_structure::floor_div(-1, 16) == -1, "negative floor_div");
        check(water_structure::floor_div(-17, 16) == -2, "negative floor_div across boundary");
        check(water_structure::floor_div64(-64, 16) == -4,
            "floor_div64 exact negative boundary");
        check(water_structure::floor_div64(-65, 16) == -5 &&
            water_structure::floor_mod64(-65, 16) == 15,
            "floor_div64 negative boundary remainder");
        check(water_structure::floor_mod(-1, 16) == 15, "negative floor_mod");
        check(water_structure::structure_y_to_chunk_local(0) == -64, "block entity minimum y");
        check(water_structure::structure_y_to_chunk_local(63) == -1, "block entity local y");

        water_structure::RuntimeRegistry registry;
        water_structure::BlockState stone{ "stone", {}, 0 };
        const auto first = registry.register_state(stone);
        check(first != registry.air_runtime_id(), "stone runtime id");
        check(registry.find("minecraft:stone").value() == first, "stone lookup");
        check(registry.state(first)->name == "minecraft:stone", "stone reverse lookup");
        water_structure::BlockState versioned{ "stone", {}, 1 };
        check(registry.register_state(versioned) != first, "version participates in runtime key");
        water_structure::BlockState byte_state{
            "minecraft:test",
            {{ "enabled", water_structure::BlockStateValueType::Byte, "1" }},
            0
        };
        water_structure::BlockState string_state{
            "minecraft:test",
            {{ "enabled", water_structure::BlockStateValueType::String, "1" }},
            0
        };
        check(registry.register_state(byte_state) != registry.register_state(string_state),
            "state property type participates in runtime key");
        water_structure::BlockState current_state{
            "minecraft:version_lookup",
            {{ "value", water_structure::BlockStateValueType::Int, "3" }},
            42
        };
        const auto current_runtime = registry.register_state(current_state);
        check(registry.find(current_state.name, current_state.states).value() == current_runtime,
            "zero-version lookup resolves current block state");
        const auto canonical_byte_state = water_structure::canonical_block_state(byte_state);
        const auto canonical_string_state = water_structure::canonical_block_state(string_state);
        check(canonical_byte_state.ok() && canonical_string_state.ok(), "canonical block states encode");
        check(canonical_byte_state.value() != canonical_string_state.value(),
            "canonical block states preserve property types");
        water_structure::BlockState reordered_state{
            "minecraft:test",
            {
                { "second", water_structure::BlockStateValueType::String, "b" },
                { "first", water_structure::BlockStateValueType::Int, "7" }
            },
            42
        };
        auto sorted_state = reordered_state;
        std::reverse(sorted_state.states.begin(), sorted_state.states.end());
        check(water_structure::canonical_block_state(reordered_state).value() ==
            water_structure::canonical_block_state(sorted_state).value(),
            "canonical block state sorts properties");
        nbt::tag_compound canonical_root;
        canonical_root.emplace<nbt::tag_int>("z", 9);
        canonical_root.emplace<nbt::tag_byte>("a", 1);
        std::ostringstream canonical_nbt_stream(std::ios::binary);
        nbt::io::write_tag("", canonical_root, canonical_nbt_stream, endian::little);
        const auto canonical_nbt_bytes = canonical_nbt_stream.str();
        const auto canonical_nbt = water_structure::canonical_nbt(std::span{
            reinterpret_cast<const std::uint8_t*>(canonical_nbt_bytes.data()), canonical_nbt_bytes.size() });
        check(canonical_nbt.ok() && !canonical_nbt.value().empty(), "canonical NBT encodes");
        const std::vector<std::uint8_t> expected_canonical_nbt{
            'W', 'S', 'N', 'B', 1,
            10, 2, 0, 0, 0,
            1, 0, 0, 0, 'a', 1, 1,
            1, 0, 0, 0, 'z', 3, 9, 0, 0, 0
        };
        check(canonical_nbt.value() == expected_canonical_nbt,
            "canonical NBT preserves types and sorts compound keys");
        const auto canonical_fields = water_structure::canonical_nbt_fields(std::span{
            reinterpret_cast<const std::uint8_t*>(canonical_nbt_bytes.data()), canonical_nbt_bytes.size() });
        check(canonical_fields.ok() && canonical_fields.value().size() == 2,
            "canonical NBT exposes field paths");
        check(canonical_fields.value()[0].path == "/a" &&
            canonical_fields.value()[0].value.front() == 1 &&
            canonical_fields.value()[1].path == "/z" &&
            canonical_fields.value()[1].value.front() == 3,
            "canonical NBT field paths are sorted and typed");
        const auto mapping_asset = find_mapping_asset();
        check(!mapping_asset.empty(), "block mapping asset exists");
        const auto mappings = registry.load_block_mappings(mapping_asset);
        check(mappings.ok(), "block mapping asset loads");
        check(registry.state(registry.air_runtime_id()).has_value(), "air runtime reverse mapping");
        check(registry.java_state(registry.air_runtime_id()).has_value(),
            "Java runtime reverse mapping");
        check(registry.schematic_runtime_id(0, 0).value() == registry.air_runtime_id(),
            "schematic air mapping");
        check(registry.schematic_runtime_id(1, 0).value() != registry.air_runtime_id(),
            "schematic stone mapping");
        check(registry.legacy_runtime_id(117, 0).has_value(), "BDX 117 mapping");
        check(registry.legacy_runtime_id("minecraft:STONE_BLOCK_SLAB", 0) ==
            registry.legacy_runtime_id("minecraft:stone_block_slab", 0),
            "legacy names are case-normalized");
        check(water_structure::FormatRegistry::formats().size() == 37, "format count");
        check(std::ranges::count_if(
                water_structure::FormatRegistry::formats(),
                [](const auto& format) { return format.reader_implemented; }) == 36,
            "audited reader capability count");
        check(std::ranges::count_if(
                water_structure::FormatRegistry::formats(),
                [](const auto& format) { return format.writer_implemented; }) == 11,
            "audited writer capability count");
        for (const auto format_id : {
            water_structure::StructureId::Schematic,
            water_structure::StructureId::SchemV1,
            water_structure::StructureId::SchemV2,
            water_structure::StructureId::Litematic,
            water_structure::StructureId::MCStructure,
            water_structure::StructureId::BDX,
            water_structure::StructureId::AxiomBP,
            water_structure::StructureId::MCFunction,
            water_structure::StructureId::IBImport,
            water_structure::StructureId::FuHongV4,
            water_structure::StructureId::FuHongV5 }) {
            const auto format = std::ranges::find_if(
                water_structure::FormatRegistry::formats(),
                [format_id](const auto& value) { return value.id == format_id; });
            check(format != water_structure::FormatRegistry::formats().end() &&
                format->writer_implemented && format->world_export_implemented,
                "Go FromMCWorld writer capability");
        }
        const auto file_capability = water_structure::FormatRegistry::capability(
            water_structure::StructureId::Schematic,
            water_structure::StructureId::SchemV1);
        check(file_capability.ok() && file_capability.value().supported &&
            !file_capability.value().streaming && file_capability.value().lossy,
            "audited file-to-file capability does not overstate native streaming or fidelity");
        const auto missing_writer_capability = water_structure::FormatRegistry::capability(
            water_structure::StructureId::Schematic,
            water_structure::StructureId::Construction);
        check(missing_writer_capability.ok() &&
            !missing_writer_capability.value().supported &&
            missing_writer_capability.value().reason.find("writer") != std::string::npos,
            "audited capability rejects missing writer");
        const auto to_world_capability = water_structure::FormatRegistry::capability(
            water_structure::StructureId::MianYangV1,
            water_structure::StructureId::MCWorld,
            water_structure::ConversionDirection::StructureToWorld);
        check(to_world_capability.ok() && to_world_capability.value().supported,
            "audited structure-to-world capability");
        const auto from_world_capability = water_structure::FormatRegistry::capability(
            water_structure::StructureId::MCWorld,
            water_structure::StructureId::SchemV1,
            water_structure::ConversionDirection::WorldToStructure);
        check(from_world_capability.ok() && from_world_capability.value().supported &&
            from_world_capability.value().streaming && from_world_capability.value().lossy,
            "world-to-structure uses MCWorld reader plus verified file writer");
        const auto unsupported_loss = water_structure::FormatRegistry::capability(
            water_structure::StructureId::MCFunction,
            water_structure::StructureId::Construction);
        check(unsupported_loss.ok() && !unsupported_loss.value().supported &&
            !unsupported_loss.value().streaming && !unsupported_loss.value().lossy,
            "unsupported capability does not advertise pipeline properties");
        const auto mcworld_copy = water_structure::FormatRegistry::capability(
            water_structure::StructureId::MCWorld,
            water_structure::StructureId::MCWorld,
            water_structure::ConversionDirection::StructureToWorld);
        check(mcworld_copy.ok() && mcworld_copy.value().supported,
            "MCWorld can stream into another world target");
        const auto sibi_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::SIBI; });
        check(sibi_format != water_structure::FormatRegistry::formats().end() &&
            !sibi_format->reader_implemented && !sibi_format->writer_implemented &&
            !sibi_format->magic_signatures.empty() && sibi_format->magic_signatures.front() == "H4/Go oracle unsupported",
            "SIBI capability and magic metadata");
        for (const auto version : {
            water_structure::StructureId::MianYangV1,
            water_structure::StructureId::MianYangV2,
            water_structure::StructureId::MianYangV3,
            water_structure::StructureId::MianYangV4 }) {
            const auto format = std::ranges::find_if(
                water_structure::FormatRegistry::formats(),
                [version](const auto& value) { return value.id == version; });
            check(format != water_structure::FormatRegistry::formats().end() &&
                format->reader_implemented && format->world_import_implemented,
                "MianYang reader capability");
        }
        test_zip_round_trip();
        test_zip_streaming_entry();
        test_new_mcworld_archive();

        const auto schematic_path = write_schematic_sample();
        const auto schematic = water_structure::FormatRegistry::open(schematic_path, registry);
        check(schematic.ok(), "synthetic Schematic parses");
        check(schematic.value()->size().width == 2, "synthetic Schematic width");
        check(schematic.value()->count_non_air_blocks().value() == 1, "synthetic Schematic non-air count");
        const std::vector<water_structure::ChunkPos> schematic_positions{{ 0, 0 }};
        const auto schematic_chunks = schematic.value()->get_chunks(schematic_positions);
        check(schematic_chunks.ok(), "synthetic Schematic chunks");
        check(schematic_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
            registry.schematic_runtime_id(1, 0).value(), "synthetic Schematic stone mapping");
        const auto mcstructure_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_round_trip.mcstructure";
        const auto mcstructure_written = water_structure::FormatRegistry::write(
            *schematic.value(), water_structure::StructureId::MCStructure, mcstructure_path, registry);
        check(mcstructure_written.ok(), "MCStructure writer succeeds");
        const auto mcstructure = water_structure::FormatRegistry::open(mcstructure_path, registry);
        check(mcstructure.ok(), "written MCStructure parses");
        check(mcstructure.value()->id() == water_structure::StructureId::MCStructure,
            "written MCStructure format");
        const auto mcstructure_size = mcstructure.value()->size();
        const auto schematic_size = schematic.value()->size();
        check(mcstructure_size.width == schematic_size.width &&
            mcstructure_size.height == schematic_size.height &&
            mcstructure_size.length == schematic_size.length, "MCStructure round-trip size");
        check(mcstructure.value()->count_non_air_blocks().value() == 1,
            "MCStructure round-trip non-air count");
        const auto mcstructure_chunks = mcstructure.value()->get_chunks(schematic_positions);
        check(mcstructure_chunks.ok(), "MCStructure round-trip chunks");
        check(mcstructure_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
            registry.schematic_runtime_id(1, 0).value(), "MCStructure round-trip block state");

        const auto bdx_writer_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_roundtrip.bdx";
        const auto bdx_written = water_structure::FormatRegistry::write(
            *schematic.value(), water_structure::StructureId::BDX, bdx_writer_path, registry);
        check(bdx_written.ok(), "BDX writer succeeds");
        const auto bdx_round_trip = water_structure::FormatRegistry::open(bdx_writer_path, registry);
        check(bdx_round_trip.ok(), "BDX writer output parses");
        check(bdx_round_trip.value()->count_non_air_blocks().value() == 1,
            "BDX writer round-trip non-air count");
        const auto bdx_round_trip_chunks = bdx_round_trip.value()->get_chunks(schematic_positions);
        check(bdx_round_trip_chunks.ok() &&
            bdx_round_trip_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                registry.schematic_runtime_id(1, 0).value(),
            "BDX writer round-trip block state");
        const auto axiom_writer_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_roundtrip.bp";
        const auto axiom_written = water_structure::FormatRegistry::write(
            *schematic.value(), water_structure::StructureId::AxiomBP, axiom_writer_path, registry);
        check(axiom_written.ok(), "AxiomBP writer succeeds");
        const auto axiom_round_trip = water_structure::FormatRegistry::open(axiom_writer_path, registry);
        check(axiom_round_trip.ok(), "AxiomBP writer output parses");
        check(axiom_round_trip.value()->count_non_air_blocks().value() == 1,
            "AxiomBP writer round-trip non-air count");
        const auto axiom_round_trip_chunks = axiom_round_trip.value()->get_chunks(schematic_positions);
        check(axiom_round_trip_chunks.ok() &&
            axiom_round_trip_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                registry.schematic_runtime_id(1, 0).value(),
            "AxiomBP writer round-trip block state");
        for (const auto schem_format : {
            water_structure::StructureId::SchemV1,
            water_structure::StructureId::SchemV2 }) {
            const auto schem_writer_path = std::filesystem::temp_directory_path() /
                (schem_format == water_structure::StructureId::SchemV1
                    ? "water_structure_cpp_writer_v1.schem"
                    : "water_structure_cpp_writer_v2.schem");
            const auto schem_written = water_structure::FormatRegistry::write(
                *schematic.value(), schem_format, schem_writer_path, registry);
            check(schem_written.ok(), "Schem writer succeeds");
            const auto schem_round_trip = water_structure::FormatRegistry::open_as(
                schem_writer_path, schem_format, registry);
            check(schem_round_trip.ok(), "Schem writer output parses");
            check(schem_round_trip.value()->count_non_air_blocks().value() == 1,
                "Schem writer round-trip non-air count");
            const auto schem_round_trip_chunks = schem_round_trip.value()->get_chunks(schematic_positions);
            check(schem_round_trip_chunks.ok() &&
                schem_round_trip_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                    registry.schematic_runtime_id(1, 0).value(),
                "Schem writer round-trip block state");
            std::filesystem::remove(schem_writer_path);
        }
        const auto litematic_writer_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_roundtrip.litematic";
        const auto litematic_written = water_structure::FormatRegistry::write(
            *schematic.value(), water_structure::StructureId::Litematic,
            litematic_writer_path, registry);
        check(litematic_written.ok(), "Litematic writer succeeds");
        const auto litematic_round_trip = water_structure::FormatRegistry::open(
            litematic_writer_path, registry);
        check(litematic_round_trip.ok(), "Litematic writer output parses");
        check(litematic_round_trip.value()->count_non_air_blocks().value() == 1,
            "Litematic writer round-trip non-air count");
        const auto litematic_round_trip_chunks = litematic_round_trip.value()->get_chunks(schematic_positions);
        check(litematic_round_trip_chunks.ok() &&
            litematic_round_trip_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                registry.schematic_runtime_id(1, 0).value(),
            "Litematic writer round-trip block state");
        const auto schematic_writer_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_roundtrip.schematic";
        const auto schematic_written = water_structure::FormatRegistry::write(
            *schematic.value(), water_structure::StructureId::Schematic,
            schematic_writer_path, registry);
        check(schematic_written.ok(), "Schematic writer succeeds");
        const auto schematic_round_trip = water_structure::FormatRegistry::open(
            schematic_writer_path, registry);
        check(schematic_round_trip.ok(), "Schematic writer output parses");
        check(schematic_round_trip.value()->count_non_air_blocks().value() == 1,
            "Schematic writer round-trip non-air count");
        const auto schematic_round_trip_chunks = schematic_round_trip.value()->get_chunks(schematic_positions);
        check(schematic_round_trip_chunks.ok() &&
            schematic_round_trip_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                registry.schematic_runtime_id(1, 0).value(),
            "Schematic writer round-trip block state");
        const auto mcfunction_source_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_source.mcfunction";
        const auto mcfunction_writer_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_roundtrip.mcfunction";
        const auto mcfunction_single_thread_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_single_thread.mcfunction";
        const auto mcfunction_no_clear_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_no_clear.mcfunction";
        const auto mcfunction_chunk_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_chunk_partition.mcfunction";
        {
            std::ofstream source(mcfunction_source_path, std::ios::binary | std::ios::trunc);
            source << "setblock 0 0 0 minecraft:oak_log[axis=x]\n"
                   << "setblock 1 0 0 minecraft:green_candle[candles=3,lit=false]\n"
                   << "setblock 2 0 0 minecraft:water[level=0]\n"
                   << "setblock 15 0 0 minecraft:oak_log[axis=x]\n"
                   << "setblock 16 0 0 minecraft:oak_log[axis=x]\n"
                   << "setblock 32 32 32 minecraft:air\n";
        }
        const auto mcfunction_source = water_structure::FormatRegistry::open(
            mcfunction_source_path, registry);
        check(mcfunction_source.ok(), "MCFunction writer source parses");
        set_test_environment("WATER_STRUCTURE_MCFUNCTION_PARSE_THREADS", "2");
        const auto mcfunction_parallel_source = water_structure::FormatRegistry::open(
            mcfunction_source_path, registry);
        set_test_environment("WATER_STRUCTURE_MCFUNCTION_PARSE_THREADS", "");
        check(mcfunction_parallel_source.ok(),
            "MCFunction bounded parallel reader parses");
        check(mcfunction_parallel_source.value()->size().width ==
                  mcfunction_source.value()->size().width &&
              mcfunction_parallel_source.value()->size().height ==
                  mcfunction_source.value()->size().height &&
              mcfunction_parallel_source.value()->size().length ==
                  mcfunction_source.value()->size().length,
            "MCFunction parallel reader preserves bounds");
        const auto mcfunction_written = water_structure::FormatRegistry::write(
            *mcfunction_source.value(), water_structure::StructureId::MCFunction,
            mcfunction_writer_path, registry);
        check(mcfunction_written.ok(), "MCFunction writer succeeds");
        std::size_t mcfunction_progress_total = 0;
        std::size_t mcfunction_progress_done = 0;
        water_structure::ConversionOptions mcfunction_progress_options{
            .thread_count = 1
        };
        mcfunction_progress_options.collect_statistics = true;
        water_structure::ConversionStats mcfunction_stats;
        mcfunction_progress_options.callbacks.start =
            [&](std::size_t total) { mcfunction_progress_total = total; };
        mcfunction_progress_options.callbacks.progress =
            [&]() { ++mcfunction_progress_done; };
        mcfunction_progress_options.callbacks.statistics =
            [&](const water_structure::ConversionStats& value) { mcfunction_stats = value; };
        const auto mcfunction_single_thread_written = water_structure::FormatRegistry::write(
            *mcfunction_source.value(), water_structure::StructureId::MCFunction,
            mcfunction_single_thread_path, registry, mcfunction_progress_options);
        check(mcfunction_single_thread_written.ok(),
            "MCFunction single-thread writer succeeds");
        check(mcfunction_progress_total > 0 &&
            mcfunction_progress_done == mcfunction_progress_total,
            "MCFunction writer reports chunk progress");
        check(mcfunction_stats.success &&
            mcfunction_stats.target_format == water_structure::StructureId::MCFunction &&
            mcfunction_stats.source_chunks == mcfunction_stats.completed_chunks,
            "conversion statistics report completed chunks");
        water_structure::ConversionOptions mcfunction_no_clear_options{
            .clear_air = false
        };
        const auto mcfunction_no_clear_written = water_structure::FormatRegistry::write(
            *mcfunction_source.value(), water_structure::StructureId::MCFunction,
            mcfunction_no_clear_path, registry, mcfunction_no_clear_options);
        check(mcfunction_no_clear_written.ok(),
            "MCFunction writer supports disabling destination clearing");
        water_structure::ConversionOptions mcfunction_chunk_options{
            .clear_air = false,
            .mcfunction_chunk_partition = true
        };
        const auto mcfunction_chunk_written = water_structure::FormatRegistry::write(
            *mcfunction_source.value(), water_structure::StructureId::MCFunction,
            mcfunction_chunk_path, registry, mcfunction_chunk_options);
        check(mcfunction_chunk_written.ok(),
            "MCFunction writer supports chunk-partitioned optimization");
        {
            std::ifstream no_clear(mcfunction_no_clear_path, std::ios::binary);
            const std::string no_clear_bytes{
                std::istreambuf_iterator<char>(no_clear), std::istreambuf_iterator<char>() };
            check(no_clear_bytes.find("minecraft:air") == std::string::npos,
                "MCFunction no-clear mode emits no air commands");
        }
        {
            std::ifstream chunk_output(mcfunction_chunk_path, std::ios::binary);
            const std::string chunk_bytes{
                std::istreambuf_iterator<char>(chunk_output),
                std::istreambuf_iterator<char>() };
            check(chunk_bytes.find("fill ~15 ~0 ~0 ~16 ~0 ~0") == std::string::npos &&
                chunk_bytes.find("setblock ~15 ~0 ~0") != std::string::npos &&
                chunk_bytes.find("setblock ~16 ~0 ~0") != std::string::npos,
                "MCFunction chunk optimization keeps commands within chunk boundaries");
        }
        {
            std::ifstream parallel(mcfunction_writer_path, std::ios::binary);
            std::ifstream sequential(mcfunction_single_thread_path, std::ios::binary);
            const std::string parallel_bytes{
                std::istreambuf_iterator<char>(parallel), std::istreambuf_iterator<char>() };
            const std::string sequential_bytes{
                std::istreambuf_iterator<char>(sequential), std::istreambuf_iterator<char>() };
            check(parallel_bytes == sequential_bytes,
                "MCFunction thread counts preserve deterministic output");
        }
        const auto mcfunction_round_trip = water_structure::FormatRegistry::open(
            mcfunction_writer_path, registry);
        check(mcfunction_round_trip.ok(), "MCFunction writer output parses");
        const auto mcfunction_size = mcfunction_round_trip.value()->size();
        check(mcfunction_size.width == 33 && mcfunction_size.height == 33 &&
            mcfunction_size.length == 33,
            "MCFunction writer preserves trailing-air dimensions");
        check(mcfunction_round_trip.value()->count_non_air_blocks().value() == 5,
            "MCFunction writer round-trip non-air count");
        const auto mcfunction_source_chunks = mcfunction_source.value()->get_chunks(
            std::array<water_structure::ChunkPos, 1>{ water_structure::ChunkPos{ 0, 0 } });
        const auto mcfunction_round_trip_chunks = mcfunction_round_trip.value()->get_chunks(
            std::array<water_structure::ChunkPos, 1>{ water_structure::ChunkPos{ 0, 0 } });
        check(mcfunction_source_chunks.ok() && mcfunction_round_trip_chunks.ok() &&
            mcfunction_round_trip.value()->count_non_air_blocks().value() == 5,
            "MCFunction writer round-trip Bedrock state properties");
        {
            std::ifstream written(mcfunction_writer_path, std::ios::binary);
            std::string line;
            bool found_state = false;
            bool all_coordinates_relative = true;
            while (std::getline(written, line)) {
                if (line.find("setblock ") != std::string::npos ||
                    line.find("fill ") != std::string::npos) found_state = true;
                if (line.starts_with("setblock ")) {
                    std::istringstream fields(line);
                    std::string command;
                    std::array<std::string, 3> coordinates;
                    fields >> command >> coordinates[0] >> coordinates[1] >> coordinates[2];
                    for (const auto& coordinate : coordinates) {
                        if (coordinate.empty() || coordinate.front() != '~') {
                            all_coordinates_relative = false;
                        }
                    }
                    continue;
                }
                if (!line.starts_with("fill ")) continue;
                std::istringstream fields(line);
                std::string command;
                std::array<std::string, 6> coordinates;
                fields >> command >> coordinates[0] >> coordinates[1] >> coordinates[2]
                    >> coordinates[3] >> coordinates[4] >> coordinates[5];
                for (const auto& coordinate : coordinates) {
                    if (coordinate.empty() || coordinate.front() != '~') {
                        all_coordinates_relative = false;
                    }
                }
                const auto parse_relative = [](const std::string& coordinate) {
                    return std::stoi(coordinate.size() > 1 ? coordinate.substr(1) : "0");
                };
                const auto x1 = parse_relative(coordinates[0]);
                const auto y1 = parse_relative(coordinates[1]);
                const auto z1 = parse_relative(coordinates[2]);
                const auto x2 = parse_relative(coordinates[3]);
                const auto y2 = parse_relative(coordinates[4]);
                const auto z2 = parse_relative(coordinates[5]);
                const auto fill_volume =
                    static_cast<std::int64_t>(std::abs(x2 - x1) + 1) *
                    (std::abs(y2 - y1) + 1) * (std::abs(z2 - z1) + 1);
                check(fill_volume <= 32768, "MCFunction fill respects command block limit");
            }
            check(found_state, "MCFunction writer emits Bedrock block states");
            check(all_coordinates_relative,
                "MCFunction writer emits relative coordinates");
        }
        const auto mcfunction_nbt_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_nbt.mcfunction";
        std::filesystem::remove(mcfunction_nbt_path);
        const BlockEntityStructure mcfunction_with_nbt(*mcfunction_source.value());
        const auto mcfunction_nbt_written = water_structure::FormatRegistry::write(
            mcfunction_with_nbt, water_structure::StructureId::MCFunction,
            mcfunction_nbt_path, registry);
        check(mcfunction_nbt_written.ok(),
            "MCFunction writer skips block entity NBT");
        check(std::filesystem::exists(mcfunction_nbt_path),
            "MCFunction writer creates output when block entity NBT is skipped");
        std::filesystem::remove(mcfunction_nbt_path);
        const auto ibimport_writer_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_roundtrip.ibi";
        const auto ibimport_written = water_structure::FormatRegistry::write(
            *schematic.value(), water_structure::StructureId::IBImport,
            ibimport_writer_path, registry);
        check(ibimport_written.ok(), "IBImport writer succeeds");
        const auto ibimport_round_trip = water_structure::FormatRegistry::open(
            ibimport_writer_path, registry);
        check(ibimport_round_trip.ok(), "IBImport writer output parses");
        check(ibimport_round_trip.value()->count_non_air_blocks().value() == 1,
            "IBImport writer round-trip non-air count");
        const auto ibimport_round_trip_chunks = ibimport_round_trip.value()->get_chunks(schematic_positions);
        check(ibimport_round_trip_chunks.ok() &&
            ibimport_round_trip_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                registry.schematic_runtime_id(1, 0).value(),
            "IBImport writer round-trip block state");
        const auto ibimport_parallel_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_parallel.ibi";
        const auto ibimport_single_thread_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_writer_single_thread.ibi";
        const auto ibimport_parallel = water_structure::FormatRegistry::write(
            *mcfunction_source.value(), water_structure::StructureId::IBImport,
            ibimport_parallel_path, registry,
            water_structure::ConversionOptions{ .thread_count = 4 });
        const auto ibimport_single_thread = water_structure::FormatRegistry::write(
            *mcfunction_source.value(), water_structure::StructureId::IBImport,
            ibimport_single_thread_path, registry,
            water_structure::ConversionOptions{ .thread_count = 1 });
        check(ibimport_parallel.ok() && ibimport_single_thread.ok(),
            "IBImport generic writer supports configured thread counts");
        {
            std::ifstream parallel(ibimport_parallel_path, std::ios::binary);
            std::ifstream sequential(ibimport_single_thread_path, std::ios::binary);
            const std::string parallel_bytes{
                std::istreambuf_iterator<char>(parallel), std::istreambuf_iterator<char>() };
            const std::string sequential_bytes{
                std::istreambuf_iterator<char>(sequential), std::istreambuf_iterator<char>() };
            check(parallel_bytes == sequential_bytes,
                "IBImport generic thread counts preserve deterministic output");
        }
        for (const auto fuhong_format : {
            water_structure::StructureId::FuHongV4,
            water_structure::StructureId::FuHongV5 }) {
            const auto fuhong_writer_path = std::filesystem::temp_directory_path() /
                (fuhong_format == water_structure::StructureId::FuHongV4
                    ? "water_structure_cpp_writer_roundtrip_fuhong_v4.json"
                    : "water_structure_cpp_writer_roundtrip_fuhong_v5.fhbuild");
            const auto fuhong_written = water_structure::FormatRegistry::write(
                *schematic.value(), fuhong_format, fuhong_writer_path, registry);
            check(fuhong_written.ok(), "FuHong writer succeeds");
            const auto fuhong_round_trip = water_structure::FormatRegistry::open(
                fuhong_writer_path, registry);
            check(fuhong_round_trip.ok(), "FuHong writer output parses");
            check(fuhong_round_trip.value()->count_non_air_blocks().value() == 1,
                "FuHong writer round-trip non-air count");
            const auto fuhong_round_trip_chunks =
                fuhong_round_trip.value()->get_chunks(schematic_positions);
            check(fuhong_round_trip_chunks.ok() &&
                fuhong_round_trip_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                    registry.schematic_runtime_id(1, 0).value(),
                "FuHong writer round-trip block state");
            std::filesystem::remove(fuhong_writer_path);
        }
        std::filesystem::remove(ibimport_writer_path);
        std::filesystem::remove(ibimport_parallel_path);
        std::filesystem::remove(ibimport_single_thread_path);
        std::filesystem::remove(mcfunction_single_thread_path);
        std::filesystem::remove(mcfunction_writer_path);
        std::filesystem::remove(mcfunction_chunk_path);
        std::filesystem::remove(mcfunction_source_path);
        std::filesystem::remove(schematic_writer_path);
        std::filesystem::remove(litematic_writer_path);
        std::filesystem::remove(axiom_writer_path);
        std::filesystem::remove(bdx_writer_path);
        std::filesystem::remove(mcstructure_path);
        std::filesystem::remove(schematic_path);

        const auto schem_v2_path = write_schem_v2_sample();
        const auto schem_v2 = water_structure::FormatRegistry::open(schem_v2_path, registry);
        check(schem_v2.ok(), "synthetic SchemV2 parses");
        check(schem_v2.value()->id() == water_structure::StructureId::SchemV2,
            "SchemV2 trial detection");
        check(schem_v2.value()->size().width == 2 &&
                schem_v2.value()->size().height == 1 &&
                schem_v2.value()->size().length == 1 &&
                schem_v2.value()->offset() == water_structure::BlockPos{},
            "SchemV2 file Offset remains placement metadata");
        check(schem_v2.value()->count_non_air_blocks().value() == 1, "synthetic SchemV2 non-air count");
        const std::array<water_structure::ChunkPos, 1> schem_v2_positions{ {
            water_structure::ChunkPos{ 0, 0 }
        } };
        const auto schem_v2_chunks = schem_v2.value()->get_chunks(schem_v2_positions);
        check(schem_v2_chunks.ok() &&
                schem_v2_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                    registry.compatible_java_runtime_id("minecraft:stone").value(),
            "SchemV2 file Offset does not move decoded blocks");
        const auto schem_direct_path = write_schem_v1_ordered_sample();
        const auto schem_direct = water_structure::FormatRegistry::open_as(
            schem_direct_path, water_structure::StructureId::SchemV1, registry,
            {
                .streaming_world_import = true,
                .direct_schem_world_stream = true
            });
        check(schem_direct.ok(), "Schem direct world stream opens without BlockData spool");
        TestWorld schem_direct_world;
        const auto schem_direct_written = schem_direct.value()->write_to_world(
            schem_direct_world, { 0, -4, 0 }, {});
        check(schem_direct_written.ok(), "Schem direct gzip stream writes to world");
        check(schem_direct_world.saved_chunk.layout == water_structure::BlockLayerLayout::Native &&
                schem_direct_world.saved_chunk.sub_chunks.contains(-4) &&
                schem_direct_world.saved_chunk.sub_chunks.at(-4).layer0[0] !=
                    registry.air_runtime_id() &&
                schem_direct_world.saved_chunk.sub_chunks.at(-4).layer0[256] ==
                    registry.air_runtime_id(),
            "Schem direct stream preserves palette indices and native coordinates");
        const auto schem_multibyte_direct_path = write_schem_v1_multibyte_varint_sample();
        const auto schem_multibyte_direct = water_structure::FormatRegistry::open_as(
            schem_multibyte_direct_path, water_structure::StructureId::SchemV1, registry,
            {
                .streaming_world_import = true,
                .direct_schem_world_stream = true
            });
        check(schem_multibyte_direct.ok(), "Schem direct multi-byte varint stream opens");
        TestWorld schem_multibyte_direct_world;
        const auto schem_multibyte_direct_written =
            schem_multibyte_direct.value()->write_to_world(
                schem_multibyte_direct_world, { 0, -4, 0 }, {});
        check(schem_multibyte_direct_written.ok(),
            "Schem direct multi-byte varint stream writes to world");
        check(schem_multibyte_direct_world.saved_chunk.sub_chunks.at(-4).layer0[0] ==
                registry.compatible_java_runtime_id("minecraft:stone").value() &&
            schem_multibyte_direct_world.saved_chunk.sub_chunks.at(-4).layer0[256] ==
                registry.air_runtime_id(),
            "Schem bulk varint decoder preserves scalar multi-byte fallback");
        std::filesystem::remove(schem_multibyte_direct_path);
        std::filesystem::remove(schem_direct_path);
        std::filesystem::remove(schem_v2_path);

        std::vector<std::int8_t> boundary_block_data(17, 0);
        boundary_block_data.front() = 1;
        boundary_block_data.back() = 1;
        const auto schem_boundary_path = write_schem_v1_sample(
            "water_structure_cpp_schem_boundary.schem", 17, std::move(boundary_block_data));
        const auto schem_boundary = water_structure::FormatRegistry::open_as(
            schem_boundary_path, water_structure::StructureId::SchemV1, registry);
        check(schem_boundary.ok(), "SchemV1 boundary sample parses");
        check(schem_boundary.value()->count_non_air_blocks().value() == 2,
            "SchemV1 boundary sample non-air count");
        schem_boundary.value()->set_offset({ -1, -1, -1 });
        const auto boundary_offset = schem_boundary.value()->offset();
        check(boundary_offset.x == -1 && boundary_offset.y == -1 && boundary_offset.z == -1,
            "SchemV1 preserves negative offset");
        const std::vector<water_structure::ChunkPos> repeated_boundary_positions{
            { -1, -1 }, { 0, -1 }, { -1, -1 }
        };
        const auto boundary_chunks =
            schem_boundary.value()->get_chunks(repeated_boundary_positions);
        check(boundary_chunks.ok() && boundary_chunks.value().size() == 2,
            "SchemV1 get_chunks deduplicates repeated positions");
        const auto stone_runtime =
            registry.compatible_java_runtime_id("minecraft:stone").value();
        check(boundary_chunks.value().at({ -1, -1 }).sub_chunks.at(-5).layer0[4095] ==
                stone_runtime &&
            boundary_chunks.value().at({ 0, -1 }).sub_chunks.at(-5).layer0[4095] ==
                stone_runtime,
            "SchemV1 negative offset materializes across chunk boundaries");
        std::filesystem::remove(schem_boundary_path);

        const auto schem_extra_varint_path = write_schem_v1_sample(
            "water_structure_cpp_schem_extra_varint.schem", 1, { 1, 0 });
        const auto schem_extra_varint = water_structure::FormatRegistry::open_as(
            schem_extra_varint_path, water_structure::StructureId::SchemV1, registry);
        check(!schem_extra_varint.ok() &&
                schem_extra_varint.error().find("方块数超过 size") != std::string::npos,
            "SchemV1 rejects an extra complete varint");
        const auto schem_extra_direct = water_structure::FormatRegistry::open_as(
            schem_extra_varint_path, water_structure::StructureId::SchemV1, registry,
            {
                .streaming_world_import = true,
                .direct_schem_world_stream = true
            });
        check(schem_extra_direct.ok(), "SchemV1 direct stream defers BlockData validation");
        TestWorld schem_extra_world;
        const auto schem_extra_written = schem_extra_direct.value()->write_to_world(
            schem_extra_world, { 0, -4, 0 }, {});
        check(!schem_extra_written.ok() &&
                schem_extra_written.error().find("方块数超过 size") != std::string::npos,
            "SchemV1 direct stream rejects an extra complete varint while writing");
        std::filesystem::remove(schem_extra_varint_path);

        const auto schem_multibit_path = write_schem_v1_multibit_sample();
        const auto schem_multibit = water_structure::FormatRegistry::open_as(
            schem_multibit_path, water_structure::StructureId::SchemV1, registry);
        check(schem_multibit.ok(), "SchemV1 multi-bit sample parses");
        check(schem_multibit.value()->count_non_air_blocks().value() == 97,
            "SchemV1 multi-bit sample non-air count");
        const std::vector<water_structure::ChunkPos> multibit_positions{
            { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 },
            { 5, 0 }, { 6, 0 }, { 7, 0 }, { 8, 0 }
        };
        const auto multibit_chunks = schem_multibit.value()->get_chunks(multibit_positions);
        const auto multibit_stone =
            registry.compatible_java_runtime_id("minecraft:stone").value();
        const auto multibit_dirt =
            registry.compatible_java_runtime_id("minecraft:dirt").value();
        const auto multibit_cobblestone =
            registry.compatible_java_runtime_id("minecraft:cobblestone").value();
        check(multibit_chunks.ok() &&
                multibit_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[1] ==
                    multibit_stone &&
                multibit_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[2] ==
                    multibit_dirt &&
                multibit_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[3] ==
                    multibit_cobblestone &&
                multibit_chunks.value().at({ 4, 0 }).sub_chunks.at(-4).layer0[1] ==
                    multibit_stone &&
                multibit_chunks.value().at({ 4, 0 }).sub_chunks.at(-4).layer0[0] ==
                    registry.air_runtime_id(),
            "SchemV1 multi-bit cursor crosses packed words");
        std::filesystem::remove(schem_multibit_path);

        const auto mcfunction_path = write_mcfunction_sample();
        preserve_benchmark_fixture(mcfunction_path);
        const auto mcfunction = water_structure::FormatRegistry::open(mcfunction_path, registry);
        check(mcfunction.ok(), "synthetic MCFunction parses");
        check(mcfunction.value()->id() == water_structure::StructureId::MCFunction,
            "MCFunction detection");
        check(mcfunction.value()->size().width == 3 && mcfunction.value()->size().length == 3,
            "MCFunction normalized size");
        check(mcfunction.value()->count_non_air_blocks().value() == 3,
            "MCFunction non-air count");
        const std::vector<water_structure::ChunkPos> mcfunction_positions{{ 0, 0 }};
        const auto mcfunction_chunks = mcfunction.value()->get_chunks(mcfunction_positions);
        check(mcfunction_chunks.ok() && mcfunction_chunks.value().at({ 0, 0 }).sub_chunks.contains(-4),
            "MCFunction chunks");
        std::filesystem::remove(mcfunction_path);

        const auto large_mcfunction_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_large_fill.mcfunction";
        {
            std::ofstream large_file(large_mcfunction_path, std::ios::binary | std::ios::trunc);
            large_file << "fill 0 0 0 127 31 127 minecraft:stone\n";
        }
        const auto large_mcfunction = water_structure::FormatRegistry::open(
            large_mcfunction_path, registry);
        check(large_mcfunction.ok(), "large MCFunction fill parses without expansion");
        check(large_mcfunction.value()->size().width == 128 &&
                large_mcfunction.value()->size().height == 32 &&
                large_mcfunction.value()->size().length == 128,
            "large MCFunction fill dimensions");
        check(large_mcfunction.value()->count_non_air_blocks().value() ==
                static_cast<std::size_t>(128 * 32 * 128),
            "large MCFunction fill non-air count");
        std::size_t streamed_mcfunction_chunks = 0;
        const std::array<water_structure::ChunkPos, 2> streamed_positions{{ {0, 0}, {7, 7} }};
        const auto streamed = large_mcfunction.value()->visit_chunks(
            streamed_positions,
            [&](water_structure::ChunkPos, const water_structure::ChunkData& chunk) {
                ++streamed_mcfunction_chunks;
                check(!chunk.sub_chunks.empty(), "MCFunction streamed chunk has subchunk");
                return water_structure::Result<void>::success();
            });
        check(streamed.ok() && streamed_mcfunction_chunks == 2, "MCFunction chunk streaming visitor");
        std::filesystem::remove(large_mcfunction_path);

        const auto kbdx_path = write_kbdx_sample();
        const auto kbdx = water_structure::FormatRegistry::open(kbdx_path, registry);
        check(kbdx.ok() && kbdx.value()->id() == water_structure::StructureId::KBDX,
            "synthetic KBDX parses");
        check(kbdx.value()->size().width == 1 && kbdx.value()->size().height == 1 &&
            kbdx.value()->size().length == 1 &&
            kbdx.value()->count_non_air_blocks().value() == 1,
            "KBDX dimensions and non-air count");
        preserve_benchmark_fixture(kbdx_path);
        std::filesystem::remove(kbdx_path);

        const auto bdx_path = write_bdx_sample();
        const auto bdx = water_structure::FormatRegistry::open(bdx_path, registry);
        check(bdx.ok(), "synthetic BDX parses");
        check(bdx.value()->id() == water_structure::StructureId::BDX, "BDX detection");
        check(bdx.value()->size().width == 8 && bdx.value()->size().height == 9 && bdx.value()->size().length == 10,
            "BDX dimensions");
        check(bdx.value()->count_non_air_blocks().value() == 10, "BDX non-air count");
        const std::vector<water_structure::ChunkPos> bdx_positions{{ 0, 0 }};
        const auto bdx_entities = bdx.value()->get_chunk_nbt(bdx_positions);
        check(bdx_entities.ok() && bdx_entities.value().at({ 0, 0 }).size() == 3,
            "BDX command/chest/raw NBT");
        const auto bdx_stats_path = bdx_path.parent_path() /
            "water_structure_cpp_bdx_stats.schem";
        std::optional<water_structure::ConversionStats> bdx_stats;
        water_structure::ConversionOptions bdx_stats_options;
        bdx_stats_options.collect_statistics = true;
        bdx_stats_options.callbacks.statistics =
            [&](const water_structure::ConversionStats& stats) { bdx_stats = stats; };
        const auto bdx_stats_write = water_structure::FormatRegistry::write(
            *bdx.value(), water_structure::StructureId::SchemV2,
            bdx_stats_path, registry, bdx_stats_options);
        check(bdx_stats_write.ok() && bdx_stats.has_value() && bdx_stats->success,
            "BDX random-access writer reports conversion statistics");
        check(bdx_stats && bdx_stats->temporary_spool_bytes != 0,
            "BDX placement spool bytes are included in conversion statistics");
        std::filesystem::remove(bdx_stats_path);
        const auto bdx_world_path = bdx_path.parent_path() /
            "water_structure_cpp_bdx_stats_world";
        std::error_code bdx_world_cleanup_error;
        std::filesystem::remove_all(bdx_world_path, bdx_world_cleanup_error);
        auto bdx_world = water_structure::BedrockWorldAdapter::open(bdx_world_path);
        check(bdx_world.ok(), "BDX statistics world opens");
        std::optional<water_structure::ConversionStats> bdx_world_stats;
        if (bdx_world) {
            water_structure::ConversionCallbacks bdx_world_callbacks;
            bdx_world_callbacks.worker_count = 1;
            bdx_world_callbacks.soft_memory_budget_bytes = 32u * 1024u * 1024u;
            bdx_world_callbacks.collect_statistics = true;
            bdx_world_callbacks.statistics =
                [&](const water_structure::ConversionStats& stats) {
                    bdx_world_stats = stats;
                };
            const auto converted = bdx.value()->write_to_world(
                bdx_world.value(), {}, std::move(bdx_world_callbacks));
            check(converted.ok() && !bdx_world_stats.has_value(),
                "BDX world statistics wait for close/archive completion");
            const auto closed = bdx_world.value().close();
            check(closed.ok() && bdx_world_stats && bdx_world_stats->success &&
                    bdx_world_stats->target_format == water_structure::StructureId::MCWorld &&
                    bdx_world_stats->decoded_blocks == 10 &&
                    bdx_world_stats->leveldb_batches != 0,
                "BDX fast world path reports final stage statistics");
        }
        std::filesystem::remove_all(bdx_world_path, bdx_world_cleanup_error);
        if (const auto* fixture_output = std::getenv("WATER_STRUCTURE_BDX_FIXTURE_OUTPUT")) {
            std::filesystem::copy_file(
                bdx_path,
                fixture_output,
                std::filesystem::copy_options::overwrite_existing);
        }

        const auto hinted_bdx_path = bdx_path.parent_path() /
            "water_structure_cpp_test@[0,0,0]~[7,8,9].bdx";
        std::filesystem::copy_file(
            bdx_path,
            hinted_bdx_path,
            std::filesystem::copy_options::overwrite_existing);
        const auto hinted_bdx = water_structure::FormatRegistry::open(
            hinted_bdx_path,
            registry,
            { .streaming_world_import = true });
        check(hinted_bdx.ok() && hinted_bdx.value()->size().width == 8 &&
            hinted_bdx.value()->size().height == 9 && hinted_bdx.value()->size().length == 10,
            "BDX filename bounds enable streaming world import");
        std::filesystem::remove(hinted_bdx_path);
        std::filesystem::remove(bdx_path);

        const auto truncated_bdx_path = write_truncated_bdx_sample();
        const auto truncated_bdx = water_structure::FormatRegistry::open(truncated_bdx_path, registry);
        check(!truncated_bdx.ok() && truncated_bdx.error().find("command #1") != std::string::npos,
            "BDX truncated command error context");
        std::filesystem::remove(truncated_bdx_path);

        const auto truncated_brotli_path = write_truncated_bdx_brotli_sample();
        const auto truncated_brotli = water_structure::FormatRegistry::open(
            truncated_brotli_path, registry);
        check(!truncated_brotli.ok() &&
            truncated_brotli.error().find("Brotli") != std::string::npos,
            "BDX truncated Brotli stream error");
        std::filesystem::remove(truncated_brotli_path);

        const auto ibimport_path = write_ibimport_sample();
        preserve_benchmark_fixture(ibimport_path);
        const auto ibimport = water_structure::FormatRegistry::open(ibimport_path, registry);
        check(ibimport.ok(), "synthetic IBImport parses");
        check(ibimport.value()->count_non_air_blocks().value() == 1, "IBImport non-air count");
        const std::vector<water_structure::ChunkPos> ib_positions{{ 0, 0 }};
        const auto ib_entities = ibimport.value()->get_chunk_nbt(ib_positions);
        check(ib_entities.ok() && ib_entities.value().at({ 0, 0 }).size() == 1,
            "IBImport command block NBT");
        std::filesystem::remove(ibimport_path);

        const auto ibimport_fill_path = write_ibimport_fill_sample();
        const auto ibimport_fill = water_structure::FormatRegistry::open(
            ibimport_fill_path, registry);
        check(ibimport_fill.ok(), "IBImport fill and mixed setblock parse");
        check(ibimport_fill.value()->size().width == 32 &&
            ibimport_fill.value()->size().height == 32 &&
            ibimport_fill.value()->size().length == 32,
            "IBImport reversed relative fill preserves bounds");
        check(ibimport_fill.value()->count_non_air_blocks().value() == 32768,
            "IBImport fill remains compact and setblock overwrite preserves count");
        const auto ibimport_fill_chunks = ibimport_fill.value()->get_chunks(
            std::array<water_structure::ChunkPos, 1>{ water_structure::ChunkPos{ 0, 0 } });
        const std::array dirt_states{
            water_structure::BlockStateProperty{
                "dirt_type", water_structure::BlockStateValueType::String, "normal" }
        };
        const auto dirt_runtime = registry.find("minecraft:dirt", dirt_states);
        check(ibimport_fill_chunks.ok() && dirt_runtime &&
            ibimport_fill_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[
                static_cast<std::size_t>((2 * 16 + 3) * 16 + 1)] == *dirt_runtime,
            "IBImport setblock overrides an earlier fill with state properties");
        std::filesystem::remove(ibimport_fill_path);

        const auto large_truncated_ibimport_path = write_truncated_large_ibimport_sample();
        const auto large_truncated_ibimport = water_structure::FormatRegistry::open(
            large_truncated_ibimport_path, registry);
        check(!large_truncated_ibimport.ok() &&
            large_truncated_ibimport.error().find("段数据截断") != std::string::npos &&
            large_truncated_ibimport.error().find("256 MiB") == std::string::npos,
            "IBImport reader streams segments larger than the old 256 MiB limit");
        std::filesystem::remove(large_truncated_ibimport_path);

        const auto typed_nbt = water_structure::parse_mianyang_nbt(
            R"({"blockCompleteNBT":"%7Bid%3A%22Chest%22%2CCount%3A1b%2CNums%3A%5BI%3B1%2C2%5D%7D"})");
        check(typed_nbt.ok(), "MianYang URL/SNBT parses");
        {
            const std::string bytes(
                reinterpret_cast<const char*>(typed_nbt.value().data()), typed_nbt.value().size());
            std::istringstream input(bytes, std::ios::binary);
            auto [_, compound] = nbt::io::read_compound(input, endian::little);
            check(compound->at("Count").get_type() == nbt::tag_type::Byte,
                "MianYang SNBT preserves byte type");
            check(compound->at("Nums").get_type() == nbt::tag_type::Int_Array,
                "MianYang SNBT preserves int array type");
        }

        const auto mianyang_v1_path = write_mianyang_v1_sample();
        const auto mianyang_v1 = water_structure::FormatRegistry::open(mianyang_v1_path, registry);
        check(mianyang_v1.ok() && mianyang_v1.value()->id() == water_structure::StructureId::MianYangV1,
            "MianYangV1 field detection and parse");
        check(mianyang_v1.value()->size().width == 2 && mianyang_v1.value()->size().height == 2 &&
            mianyang_v1.value()->size().length == 2, "MianYangV1 normalizes world coordinates");
        check(mianyang_v1.value()->count_non_air_blocks().value() == 2,
            "MianYangV1 non-air count");
        const std::vector<water_structure::ChunkPos> mianyang_positions{{ 0, 0 }};
        const auto mianyang_v1_entities = mianyang_v1.value()->get_chunk_nbt(mianyang_positions);
        check(mianyang_v1_entities.ok() && mianyang_v1_entities.value().at({ 0, 0 }).size() == 1,
            "MianYangV1 block entity NBT");
        preserve_mianyang_fixture(mianyang_v1_path);

        const auto mianyang_v3_path = write_mianyang_v3_sample();
        const auto mianyang_v3 = water_structure::FormatRegistry::open(mianyang_v3_path, registry);
        check(mianyang_v3.ok() && mianyang_v3.value()->id() == water_structure::StructureId::MianYangV3,
            "MianYangV3 zlib parse");
        check(mianyang_v3.value()->size().width == 2 &&
            mianyang_v3.value()->count_non_air_blocks().value() == 2,
            "MianYangV3 shared data adaptation");
        preserve_mianyang_fixture(mianyang_v3_path);

        const auto mianyang_v4_path = write_mianyang_v4_sample(false);
        const auto mianyang_v4 = water_structure::FormatRegistry::open(mianyang_v4_path, registry);
        check(mianyang_v4.ok() && mianyang_v4.value()->id() == water_structure::StructureId::MianYangV4,
            "MianYangV4 gzip binary parse");
        check(mianyang_v4.value()->size().width == 4 && mianyang_v4.value()->size().height == 3 &&
            mianyang_v4.value()->size().length == 5, "MianYangV4 header dimensions");
        check(mianyang_v4.value()->count_non_air_blocks().value() == 2,
            "MianYangV4 non-air count");
        const auto mianyang_v4_entities = mianyang_v4.value()->get_chunk_nbt(mianyang_positions);
        check(mianyang_v4_entities.ok() && mianyang_v4_entities.value().at({ 0, 0 }).size() == 1,
            "MianYangV4 block entity NBT");
        {
            const auto& payload = mianyang_v4_entities.value().at({ 0, 0 }).front().payload;
            const std::string bytes(reinterpret_cast<const char*>(payload.data()), payload.size());
            std::istringstream input(bytes, std::ios::binary);
            auto [_, compound] = nbt::io::read_compound(input, endian::little);
            check(compound->at("DoubleValue").get_type() == nbt::tag_type::Double,
                "MianYang JSON fallback preserves Go float64 type");
        }
        preserve_mianyang_fixture(mianyang_v4_path);

        const auto truncated_mianyang_v4_path = write_mianyang_v4_sample(true);
        const auto truncated_mianyang_v4 = water_structure::FormatRegistry::open(
            truncated_mianyang_v4_path, registry);
        check(!truncated_mianyang_v4.ok() &&
            truncated_mianyang_v4.error().find("block #1") != std::string::npos &&
            truncated_mianyang_v4.error().find("decoded offset") != std::string::npos,
            "MianYangV4 truncation reports block index and decoded offset");
        preserve_mianyang_fixture(truncated_mianyang_v4_path);
        std::filesystem::remove(truncated_mianyang_v4_path);
        std::filesystem::remove(mianyang_v4_path);
        std::filesystem::remove(mianyang_v3_path);
        std::filesystem::remove(mianyang_v1_path);

        const auto gangban_paths = write_gangban_samples();
        const std::array gangban_ids{
            water_structure::StructureId::GangBanV1,
            water_structure::StructureId::GangBanV2,
            water_structure::StructureId::GangBanV3,
            water_structure::StructureId::GangBanV4,
            water_structure::StructureId::GangBanV5,
            water_structure::StructureId::GangBanV6,
            water_structure::StructureId::GangBanV7
        };
        const std::array<std::size_t, 7> gangban_entities{ 1, 0, 1, 1, 1, 1, 1 };
        for (std::size_t index = 0; index < gangban_paths.size(); ++index) {
            preserve_benchmark_fixture(gangban_paths[index]);
            const auto opened = water_structure::FormatRegistry::open(gangban_paths[index], registry);
            if (!opened) {
                throw std::runtime_error(
                    "GangBan magic detection and parse v" + std::to_string(index + 1) +
                    ": " + opened.error());
            }
            if (opened.value()->id() != gangban_ids[index]) {
                throw std::runtime_error(
                    "GangBan magic detection returned wrong version at fixture v" +
                    std::to_string(index + 1));
            }
            const auto size = opened.value()->size();
            check(size.width == (index == 4 ? 1 : 2) && size.height == 1 && size.length == 1,
                "GangBan dimensions");
            check(opened.value()->count_non_air_blocks().value() == (index == 2 ? 2 : 1),
                "GangBan non-air count");
            if (index == 2) {
                const auto chunks = opened.value()->get_chunks(mianyang_positions);
                check(chunks.ok() &&
                        chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[16] ==
                            registry.compatible_java_runtime_id("minecraft:stone").value(),
                    "GangBanV3 preserves records outside declared header extent");
            }
            const auto entities = opened.value()->get_chunk_nbt(mianyang_positions);
            check(entities.ok() && entities.value().at({ 0, 0 }).size() == gangban_entities[index],
                "GangBan typed block entity NBT");
            const auto format = std::ranges::find_if(
                water_structure::FormatRegistry::formats(),
                [&](const auto& value) { return value.id == gangban_ids[index]; });
            check(format != water_structure::FormatRegistry::formats().end() &&
                format->reader_implemented && format->world_import_implemented,
                "GangBan capabilities");
        }
        const auto large_detection_path = std::filesystem::temp_directory_path() /
            "water_structure_cpp_gangban_detection_stream.json";
        {
            std::ofstream output(large_detection_path, std::ios::binary | std::ios::trunc);
            output << '[';
            for (std::size_t index = 0; index < 250'000; ++index) output << "0,";
            output << R"({"ep":[0,0,0]},["minecraft:stone"]])";
        }
        const auto large_detection = water_structure::FormatRegistry::detect(
            large_detection_path);
        check(large_detection.ok() &&
            large_detection.value().id == water_structure::StructureId::GangBanV5,
            "JSON format detection streams large root arrays");
        std::filesystem::remove(large_detection_path);
        const auto truncated_gangban = std::filesystem::temp_directory_path() /
            "water_structure_cpp_gangban_v7_truncated.reb";
        {
            std::ifstream source(gangban_paths.back(), std::ios::binary);
            std::vector<char> bytes(
                (std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
            bytes.resize(bytes.size() - 2);
            std::ofstream target(truncated_gangban, std::ios::binary | std::ios::trunc);
            target.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const auto truncated_gangban_result = water_structure::FormatRegistry::open(
            truncated_gangban, registry);
        check(!truncated_gangban_result.ok() &&
            truncated_gangban_result.error().find("compressed offset") != std::string::npos,
            "GangBanV7 truncation reports compressed offset");
        std::filesystem::remove(truncated_gangban);
        for (const auto& path : gangban_paths) std::filesystem::remove(path);

        const auto qingxu_path = write_qingxu_sample(false);
        preserve_benchmark_fixture(qingxu_path);
        const auto qingxu = water_structure::FormatRegistry::open(qingxu_path, registry);
        check(qingxu.ok() && qingxu.value()->id() == water_structure::StructureId::QingXuV1,
            "QingXu nested JSON detection and parse");
        const auto qingxu_size = qingxu.value()->size();
        check(qingxu_size.width == 2 && qingxu_size.height == 1 && qingxu_size.length == 1 &&
            qingxu.value()->count_non_air_blocks().value() == 2,
            "QingXu bounds and duplicate coordinate overwrite");
        const auto qingxu_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::QingXuV1; });
        check(qingxu_format != water_structure::FormatRegistry::formats().end() &&
            qingxu_format->reader_implemented && qingxu_format->world_import_implemented,
            "QingXu capabilities");
        const auto invalid_qingxu_path = write_qingxu_sample(true);
        const auto invalid_qingxu = water_structure::FormatRegistry::open(invalid_qingxu_path, registry);
        check(!invalid_qingxu.ok() && invalid_qingxu.error().find("chunk index 0") != std::string::npos,
            "QingXu invalid nested JSON reports chunk index");
        std::filesystem::remove(invalid_qingxu_path);
        std::filesystem::remove(qingxu_path);

        const auto timebuilder_path = write_timebuilder_sample(false);
        preserve_benchmark_fixture(timebuilder_path);
        const auto timebuilder = water_structure::FormatRegistry::open(timebuilder_path, registry);
        check(timebuilder.ok() &&
            timebuilder.value()->id() == water_structure::StructureId::TimeBuilderV1,
            "TimeBuilder version detection and parse");
        const auto timebuilder_size = timebuilder.value()->size();
        check(timebuilder_size.width == 2 && timebuilder_size.height == 1 &&
            timebuilder_size.length == 1 &&
            timebuilder.value()->count_non_air_blocks().value() == 2,
            "TimeBuilder bounds, short positions and duplicate overwrite");
        const auto timebuilder_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::TimeBuilderV1; });
        check(timebuilder_format != water_structure::FormatRegistry::formats().end() &&
            timebuilder_format->reader_implemented && timebuilder_format->world_import_implemented,
            "TimeBuilder capabilities");
        const auto invalid_timebuilder_path = write_timebuilder_sample(true);
        const auto invalid_timebuilder = water_structure::FormatRegistry::open(
            invalid_timebuilder_path, registry);
        check(!invalid_timebuilder.ok(), "TimeBuilder rejects unknown version");
        std::filesystem::remove(invalid_timebuilder_path);
        std::filesystem::remove(timebuilder_path);

        const auto runaway_path = write_gangban_json(
            "water_structure_cpp_runaway.json",
            nlohmann::json::array({
                { { "name", "minecraft:double_stone_block_slab4" }, { "aux", 0 },
                    { "x", -2 }, { "y", 3 }, { "z", 4 } },
                { { "name", "minecraft:dirt" }, { "aux", 0 },
                    { "x", -1 }, { "y", 3 }, { "z", 4 } }
            }));
        preserve_benchmark_fixture(runaway_path);
        const auto runaway = water_structure::FormatRegistry::open(runaway_path, registry);
        check(runaway.ok() && runaway.value()->id() == water_structure::StructureId::RunAway,
            "RunAway scalar-coordinate detection");
        check(runaway.value()->size().width == 2 &&
            runaway.value()->count_non_air_blocks().value() == 2,
            "RunAway parse");
        const auto runaway_chunks = runaway.value()->get_chunks(mianyang_positions);
        check(runaway_chunks.ok() &&
            runaway_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                registry.legacy_runtime_id("minecraft:double_stone_block_slab4", 0).value(),
            "RunAway legacy aux mapping");
        std::filesystem::remove(runaway_path);

        const auto fuhong_paths = write_fuhong_samples();
        const std::array fuhong_ids{
            water_structure::StructureId::FuHongV1,
            water_structure::StructureId::FuHongV2,
            water_structure::StructureId::FuHongV3,
            water_structure::StructureId::FuHongV4,
            water_structure::StructureId::FuHongV5
        };
        const std::array<std::size_t, 5> fuhong_entities{ 0, 2, 1, 1, 1 };
        for (std::size_t index = 0; index < fuhong_paths.size(); ++index) {
            preserve_benchmark_fixture(fuhong_paths[index]);
            const auto opened = water_structure::FormatRegistry::open_as(
                fuhong_paths[index], fuhong_ids[index], registry);
            check(opened.ok() && opened.value()->id() == fuhong_ids[index],
                "FuHong V1-V5 parse");
            const auto fuhong_size = opened.value()->size();
            check(fuhong_size.width == 2 && fuhong_size.height == 1 &&
                fuhong_size.length == 1 &&
                opened.value()->count_non_air_blocks().value() == 2,
                "FuHong bounds and non-air count");
            const auto entities = opened.value()->get_chunk_nbt(mianyang_positions);
            check(entities.ok() && entities.value().at({ 0, 0 }).size() == fuhong_entities[index],
                "FuHong typed block entity NBT");
            const auto format = std::ranges::find_if(
                water_structure::FormatRegistry::formats(),
                [&](const auto& value) { return value.id == fuhong_ids[index]; });
            check(format != water_structure::FormatRegistry::formats().end() &&
                format->reader_implemented && format->world_import_implemented,
                "FuHong capabilities");
        }
        check(water_structure::FormatRegistry::detect(fuhong_paths[0]).value().id ==
            water_structure::StructureId::FuHongV1, "FuHongV1 field detection");
        check(water_structure::FormatRegistry::detect(fuhong_paths[2]).value().id ==
            water_structure::StructureId::FuHongV3, "FuHongV3 field detection");
        check(water_structure::FormatRegistry::detect(fuhong_paths[3]).value().id ==
            water_structure::StructureId::FuHongV4, "FuHongV4 field detection");
        check(water_structure::FormatRegistry::detect(fuhong_paths[4]).value().id ==
            water_structure::StructureId::FuHongV5, "FuHongV5 magic detection");
        const auto truncated_fuhong = write_fuhong_v5_sample(
            nlohmann::json::parse(std::ifstream(fuhong_paths[3])), true);
        const auto truncated_fuhong_result = water_structure::FormatRegistry::open(
            truncated_fuhong, registry);
        check(!truncated_fuhong_result.ok() &&
            truncated_fuhong_result.error().find("compressed offset") != std::string::npos,
            "FuHongV5 truncation reports compressed offset");
        std::filesystem::remove(truncated_fuhong);
        for (const auto& path : fuhong_paths) std::filesystem::remove(path);

        for (const auto format_id : {
            water_structure::StructureId::BDS, water_structure::StructureId::NexusNP }) {
            const auto path = write_msgpack_structure_sample(format_id, false);
            preserve_msgpack_fixture(path);
            const auto opened = water_structure::FormatRegistry::open(path, registry);
            check(opened.ok() && opened.value()->id() == format_id,
                "MessagePack format detection and parse");
            const auto msgpack_size = opened.value()->size();
            check(msgpack_size.width == 2 && msgpack_size.height == 1 &&
                msgpack_size.length == 1 &&
                opened.value()->count_non_air_blocks().value() == 2,
                "MessagePack format bounds and count");
            const std::array<water_structure::ChunkPos, 1> msgpack_positions{{ { 0, 0 } }};
            const auto msgpack_chunks = opened.value()->get_chunks(msgpack_positions);
            check(msgpack_chunks.ok() &&
                    msgpack_chunks.value().at({ 0, 0 }).sub_chunks.contains(-4) &&
                    !msgpack_chunks.value().at({ 0, 0 }).sub_chunks.contains(-5) &&
                    msgpack_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] !=
                        registry.air_runtime_id(),
                "MessagePack exact -64 storage boundary materializes in subchunk -4");
            const auto format = std::ranges::find_if(
                water_structure::FormatRegistry::formats(),
                [&](const auto& value) { return value.id == format_id; });
            check(format != water_structure::FormatRegistry::formats().end() &&
                format->reader_implemented && format->world_import_implemented,
                "MessagePack format capabilities");
            const auto truncated = write_msgpack_structure_sample(format_id, true);
            const auto invalid = water_structure::FormatRegistry::open(truncated, registry);
            check(!invalid.ok(), "truncated MessagePack input is rejected");
            std::filesystem::remove(truncated);
            std::filesystem::remove(path);
        }

        const auto bcf_path = write_bcf_sample(false);
        preserve_msgpack_fixture(bcf_path);
        const auto bcf = water_structure::FormatRegistry::open(bcf_path, registry);
        check(bcf.ok() && bcf.value()->id() == water_structure::StructureId::BCF,
            "BCF magic detection and parse");
        const auto bcf_size = bcf.value()->size();
        check(bcf_size.width == 2 && bcf_size.height == 1 && bcf_size.length == 1 &&
            bcf.value()->count_non_air_blocks().value() == 2,
            "BCF cuboid bounds and count");
        const std::array<water_structure::ChunkPos, 1> bcf_positions{{ { 0, 0 } }};
        const auto bcf_chunks = bcf.value()->get_chunks(bcf_positions);
        check(bcf_chunks.ok() &&
                bcf_chunks.value().at({ 0, 0 }).sub_chunks.contains(-4) &&
                !bcf_chunks.value().at({ 0, 0 }).sub_chunks.contains(-5) &&
                bcf_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] !=
                    registry.air_runtime_id(),
            "BCF exact -64 storage boundary materializes in subchunk -4 layer 0");
        const auto bcf_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::BCF; });
        check(bcf_format != water_structure::FormatRegistry::formats().end() &&
            bcf_format->reader_implemented && bcf_format->world_import_implemented,
            "BCF capabilities");
        const auto zero_span_bcf_path = write_bcf_sample(false, true);
        const auto zero_span_bcf = water_structure::FormatRegistry::open(zero_span_bcf_path, registry);
        check(zero_span_bcf.ok() && zero_span_bcf.value()->count_non_air_blocks().value() == 1,
            "BCF Go-compatible zero reversed span count");
        std::filesystem::remove(zero_span_bcf_path);
        const auto truncated_bcf = write_bcf_sample(true);
        const auto invalid_bcf = water_structure::FormatRegistry::open(truncated_bcf, registry);
        check(!invalid_bcf.ok() && invalid_bcf.error().find("offset") != std::string::npos,
            "BCF truncation reports offset");
        std::filesystem::remove(truncated_bcf);
        std::filesystem::remove(bcf_path);

        const auto cov_path = write_covstructure_sample(false);
        preserve_msgpack_fixture(cov_path);
        const auto cov = water_structure::FormatRegistry::open(cov_path, registry);
        check(cov.ok() && cov.value()->id() == water_structure::StructureId::CovStructure,
            "CovStructure field detection and parse");
        const auto cov_size = cov.value()->size();
        check(cov_size.width == 2 && cov_size.height == 1 && cov_size.length == 1,
            "CovStructure dimensions");
        check(cov.value()->count_non_air_blocks().value() == 2,
            "CovStructure counts all flattened non-air indices like Go");
        const std::vector<water_structure::ChunkPos> cov_positions{{ 0, 0 }};
        const auto cov_chunks = cov.value()->get_chunks(cov_positions);
        check(cov_chunks.ok() && cov_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
            registry.legacy_runtime_id("minecraft:stone", 0).value(),
            "CovStructure places only indices inside the declared volume");
        const auto cov_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::CovStructure; });
        check(cov_format != water_structure::FormatRegistry::formats().end() &&
            cov_format->reader_implemented && cov_format->world_import_implemented,
            "CovStructure capabilities");
        const auto invalid_cov_path = write_covstructure_sample(true);
        const auto invalid_cov = water_structure::FormatRegistry::open(invalid_cov_path, registry);
        check(!invalid_cov.ok(), "invalid CovStructure JSON is rejected");
        std::filesystem::remove(invalid_cov_path);
        std::filesystem::remove(cov_path);

        const auto construction_path = write_construction_sample(false);
        preserve_msgpack_fixture(construction_path);
        const auto construction = water_structure::FormatRegistry::open(construction_path, registry);
        check(construction.ok() && construction.value()->id() == water_structure::StructureId::Construction,
            "Construction magic detection and parse");
        const auto construction_size = construction.value()->size();
        check(construction_size.width == 2 && construction_size.height == 1 &&
            construction_size.length == 1 &&
            construction.value()->count_non_air_blocks().value() == 1,
            "Construction bounds and non-air count");
        const std::vector<water_structure::ChunkPos> construction_positions{{ 0, 0 }};
        const auto construction_chunks = construction.value()->get_chunks(construction_positions);
        check(construction_chunks.ok() &&
            construction_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] ==
                registry.find("minecraft:stone").value(),
            "Construction X/Y/Z block order");
        const auto construction_entities = construction.value()->get_chunk_nbt(construction_positions);
        check(construction_entities.ok() && construction_entities.value().at({ 0, 0 }).size() == 1,
            "Construction normalized block entity NBT");
        const auto construction_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::Construction; });
        check(construction_format != water_structure::FormatRegistry::formats().end() &&
            construction_format->reader_implemented && construction_format->world_import_implemented,
            "Construction capabilities");
        const auto truncated_construction_path = write_construction_sample(true);
        const auto truncated_construction = water_structure::FormatRegistry::open(
            truncated_construction_path, registry);
        check(!truncated_construction.ok() &&
            truncated_construction.error().find("footer") != std::string::npos,
            "Construction truncated footer is rejected with context");
        std::filesystem::remove(truncated_construction_path);
        std::filesystem::remove(construction_path);

        const auto axiom_path = write_axiom_sample(false);
        preserve_msgpack_fixture(axiom_path);
        const auto axiom = water_structure::FormatRegistry::open(axiom_path, registry);
        check(axiom.ok() && axiom.value()->id() == water_structure::StructureId::AxiomBP,
            "AxiomBP magic detection and parse");
        const auto axiom_size = axiom.value()->size();
        check(axiom_size.width == 1 && axiom_size.height == 1 && axiom_size.length == 1 &&
            axiom.value()->count_non_air_blocks().value() == 1,
            "AxiomBP packed palette bounds and count");
        const auto expected_lever = registry.java_runtime_id(
            "minecraft:lever[face=ceiling,facing=west,powered=true]");
        const std::array axiom_positions{ water_structure::ChunkPos{ 0, 0 } };
        const auto axiom_chunks = axiom.value()->get_chunks(axiom_positions);
        check(expected_lever && axiom_chunks.ok() &&
            axiom_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0[0] == *expected_lever,
            "AxiomBP Go-compatible fuzzy Java state mapping");
        const auto axiom_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::AxiomBP; });
        check(axiom_format != water_structure::FormatRegistry::formats().end() &&
            axiom_format->reader_implemented && axiom_format->world_import_implemented,
            "AxiomBP capabilities");
        const auto truncated_axiom_path = write_axiom_sample(true);
        const auto truncated_axiom = water_structure::FormatRegistry::open(
            truncated_axiom_path, registry);
        check(!truncated_axiom.ok() && truncated_axiom.error().find("AxiomBP") != std::string::npos,
            "AxiomBP truncated payload is rejected with context");
        std::filesystem::remove(truncated_axiom_path);
        std::filesystem::remove(axiom_path);

        const auto tibi_path = write_tibi_sample(false);
        preserve_msgpack_fixture(tibi_path);
        const auto tibi = water_structure::FormatRegistry::open(tibi_path, registry);
        check(tibi.ok() && tibi.value()->id() == water_structure::StructureId::TIBI,
            "TIBI extension detection, raw DEFLATE, MD5 XOR and parse");
        const auto tibi_size = tibi.value()->size();
        check(tibi_size.width == 6 && tibi_size.height == 2 && tibi_size.length == 2,
            "TIBI command bounds and origin normalization");
        check(tibi.value()->count_non_air_blocks().value() == 10,
            "TIBI Go-compatible command-volume count including reversed fill behavior");
        const std::array tibi_positions{ water_structure::ChunkPos{ 0, 0 } };
        const auto tibi_chunks = tibi.value()->get_chunks(tibi_positions);
        check(tibi_chunks.ok(), "TIBI chunk materialization succeeds");
        const auto& tibi_layer = tibi_chunks.value().at({ 0, 0 }).sub_chunks.at(-4).layer0;
        check(tibi_layer[0] == registry.find("minecraft:stone").value() &&
            tibi_layer[1] == registry.find("minecraft:dirt").value() &&
            tibi_layer[5] == registry.find("minecraft:stone").value(),
            "TIBI setblock, fill and reversed fill materialization");
        const auto no_tibi_spool = water_structure::FormatRegistry::open(
            tibi_path, registry, { .allow_temporary_spool = false });
        check(!no_tibi_spool.ok() &&
            no_tibi_spool.error().find("allow_temporary_spool=false") != std::string::npos,
            "TIBI reports a capability error when its required spool is disabled");
        const auto tibi_spool_directory = std::filesystem::temp_directory_path() /
            "water_structure_cpp_tibi_spool_test";
        std::error_code tibi_spool_error;
        std::filesystem::remove_all(tibi_spool_directory, tibi_spool_error);
        const auto bounded_tibi = water_structure::FormatRegistry::open(
            tibi_path, registry, {
                .soft_memory_budget_bytes = 64u * 1024u * 1024u,
                .allow_temporary_spool = true,
                .temporary_directory = tibi_spool_directory,
                .temporary_file_limit_bytes = 16u * 1024u * 1024u
            });
        check(bounded_tibi.ok(), "TIBI two-pass bounded spool parses under a 64 MiB soft budget");
        check(std::filesystem::is_directory(tibi_spool_directory) &&
            std::filesystem::directory_iterator(tibi_spool_directory) ==
                std::filesystem::directory_iterator{},
            "TIBI removes its decoded spool after parsing");
        std::filesystem::remove_all(tibi_spool_directory, tibi_spool_error);
        const auto tibi_format = std::ranges::find_if(
            water_structure::FormatRegistry::formats(),
            [](const auto& value) { return value.id == water_structure::StructureId::TIBI; });
        check(tibi_format != water_structure::FormatRegistry::formats().end() &&
            tibi_format->reader_implemented && tibi_format->world_import_implemented &&
            !tibi_format->writer_implemented,
            "TIBI capabilities match Go ToMCWorld without FromMCWorld");
        const auto truncated_tibi_path = write_tibi_sample(true);
        const auto truncated_tibi = water_structure::FormatRegistry::open(truncated_tibi_path, registry);
        check(!truncated_tibi.ok() && truncated_tibi.error().find("compressed offset") != std::string::npos,
            "TIBI truncated raw DEFLATE reports compressed offset");
        std::filesystem::remove(truncated_tibi_path);
        std::filesystem::remove(tibi_path);

        TestStructure structure;
        TestWorld world;
        std::size_t visited_chunks = 0;
        const auto visited = structure.visit_chunks(
            std::array{ water_structure::ChunkPos{ 0, 0 } },
            [&visited_chunks](water_structure::ChunkPos pos,
                const water_structure::ChunkData& chunk) {
                if (pos != water_structure::ChunkPos{ 0, 0 } ||
                    !chunk.sub_chunks.contains(-4) ||
                    chunk.sub_chunks.at(-4).layer0[0] != 7) {
                    return water_structure::Result<void>::failure("visit_chunks payload mismatch");
                }
                ++visited_chunks;
                return water_structure::Result<void>::success();
            });
        check(visited.ok() && visited_chunks == 1, "default visit_chunks compatibility path");

        RecordingChunkSink recording_sink;
        water_structure::ChunkStream ordered_stream(
            [](water_structure::ChunkSink& sink) -> water_structure::Result<void> {
                for (std::int32_t index = 0; index < 64; ++index) {
                    water_structure::StreamChunk chunk;
                    chunk.position = { index, -index };
                    auto pushed = sink.push(std::move(chunk));
                    if (!pushed) return pushed;
                }
                return water_structure::Result<void>::success();
            });
        const auto chunk_stream_result = ordered_stream.pump(recording_sink, {
            .worker_count = 2,
            .max_in_flight_chunks = 1,
            .soft_memory_budget_bytes = 64u * 1024u * 1024u
        });
        check(chunk_stream_result.ok() && recording_sink.finished &&
            recording_sink.positions.size() == 64 &&
            recording_sink.positions.front() == water_structure::ChunkPos{ 0, 0 } &&
            recording_sink.positions.back() == water_structure::ChunkPos{ 63, -63 },
            "ChunkStream preserves order with a one-chunk backpressure window");

        std::vector<water_structure::ChunkPos> bounded_positions;
        for (std::int32_t index = 0; index < 11; ++index) {
            bounded_positions.push_back({ index, 0 });
        }
        RecordingChunkSink bounded_sink;
        auto bounded_stream = water_structure::ChunkStream::from_structure(
            structure, std::move(bounded_positions), 3);
        const auto bounded_result = bounded_stream.pump(bounded_sink, {
            .max_in_flight_chunks = 3,
            .soft_memory_budget_bytes = 8u * 1024u * 1024u
        });
        check(bounded_result.ok() && bounded_sink.positions.size() == 11 &&
            structure.visit_batch_peak <= 3 && structure.nbt_batch_peak <= 3,
            "ChunkStream bounds source materialization and NBT batches");

        FailingChunkSink failing_sink;
        water_structure::ChunkStream failing_stream(
            [](water_structure::ChunkSink& sink) -> water_structure::Result<void> {
                for (std::int32_t index = 0; index < 16; ++index) {
                    water_structure::StreamChunk chunk;
                    chunk.position = { index, 0 };
                    auto pushed = sink.push(std::move(chunk));
                    if (!pushed) return pushed;
                }
                return water_structure::Result<void>::success();
            });
        const auto sink_failure = failing_stream.pump(failing_sink, {
            .max_in_flight_chunks = 1,
            .soft_memory_budget_bytes = 4u * 1024u * 1024u
        });
        check(!sink_failure.ok() &&
            sink_failure.error().find("intentional sink failure") != std::string::npos &&
            failing_sink.cancelled,
            "ChunkStream propagates sink failure and cancels the pipeline");

        RecordingChunkSink producer_error_sink;
        water_structure::ChunkStream producer_error_stream(
            [](water_structure::ChunkSink&) -> water_structure::Result<void> {
                return water_structure::Result<void>::failure("intentional producer failure");
            });
        const auto producer_failure = producer_error_stream.pump(producer_error_sink, {
            .max_in_flight_chunks = 1,
            .soft_memory_budget_bytes = 4u * 1024u * 1024u
        });
        check(!producer_failure.ok() &&
            producer_failure.error().find("intentional producer failure") != std::string::npos,
            "ChunkStream propagates producer failure");

        // --- MCWorld palette streaming interface + MCFunction palette path ---
        {
            const auto palette_world_dir = std::filesystem::temp_directory_path() /
                "water_structure_cpp_palette_world";
            std::error_code cleanup_error;
            std::filesystem::remove_all(palette_world_dir, cleanup_error);
            const auto stone_runtime = registry.register_state(
                water_structure::BlockState{ "minecraft:stone", {}, 0 });
            const auto log_runtime = registry.register_state(
                water_structure::BlockState{
                    "minecraft:oak_log",
                    { { "axis", water_structure::BlockStateValueType::String, "x" } },
                    0 });
            const auto water_runtime = registry.register_state(
                water_structure::BlockState{
                    "minecraft:water",
                    { { "level", water_structure::BlockStateValueType::Int, "0" } },
                    0 });
            // BWO encodes/decodes through the resolver; without it the world
            // payload would be written as all-air.
            registry.install_as_bwo_resolver();
            {
                auto world = water_structure::BedrockWorldAdapter::open(palette_world_dir, true);
                check(world.ok(), "palette test world opens");
                water_structure::ChunkData chunk;
                water_structure::SubChunkData sub;
                sub.layer0.fill(registry.air_runtime_id());
                sub.layer0[0] = stone_runtime;
                sub.layer0[1] = log_runtime;
                sub.layer0[2] = water_runtime;
                chunk.sub_chunks.emplace(-4, std::move(sub));
                const auto saved = world.value().save_chunk({ 0, 0 }, chunk);
                check(saved.ok(), "palette test world saves chunk");
                const auto closed = world.value().close();
                check(closed.ok(), "palette test world closes");
            }

            // The generic world writer uses two bounded encoding workers for
            // multi-chunk batches, but keeps one LevelDB WriteBatch commit.
            // Exercise the parallel path with distinct chunks and verify both
            // payloads can be read back without changing their coordinates.
            const auto parallel_world_dir = std::filesystem::temp_directory_path() /
                "water_structure_cpp_parallel_world";
            std::filesystem::remove_all(parallel_world_dir, cleanup_error);
            {
                auto world = water_structure::BedrockWorldAdapter::open(parallel_world_dir, true);
                check(world.ok(), "parallel world opens");
                water_structure::ChunkData first;
                water_structure::ChunkData second;
                water_structure::SubChunkData first_sub;
                water_structure::SubChunkData second_sub;
                first_sub.layer0.fill(registry.air_runtime_id());
                second_sub.layer0.fill(registry.air_runtime_id());
                first_sub.layer0[0] = stone_runtime;
                second_sub.layer0[4095] = water_runtime;
                first.sub_chunks.emplace(-4, std::move(first_sub));
                second.sub_chunks.emplace(-4, std::move(second_sub));
                const std::array writes{
                    water_structure::ChunkWrite{ { 0, 0 }, &first },
                    water_structure::ChunkWrite{ { 1, 0 }, &second }
                };
                const auto saved = world.value().save_chunks(writes);
                check(saved.ok(), "parallel world batch saves");
                const auto closed = world.value().close();
                check(closed.ok(), "parallel world closes");
            }
            {
                auto world = water_structure::BedrockWorldAdapter::open(parallel_world_dir, false);
                check(world.ok(), "parallel world reopens");
                if (world) {
                    const auto first = world.value().load_chunk({ 0, 0 });
                    const auto second = world.value().load_chunk({ 1, 0 });
                    check(first.ok() && second.ok(), "parallel world reads both chunks");
                    if (first && second) {
                        check(first.value().sub_chunks.at(-4).layer0[0] == stone_runtime,
                            "parallel world preserves first chunk");
                        check(second.value().sub_chunks.at(-4).layer0[4095] == water_runtime,
                            "parallel world preserves second chunk");
                    }
                    const auto closed = world.value().close();
                    check(closed.ok(), "parallel world read closes");
                }
            }
            std::filesystem::remove_all(parallel_world_dir, cleanup_error);

            water_structure::McWorldStructure world_structure(registry);
            const auto world_read = world_structure.read(palette_world_dir);
            check(world_read.ok(), "palette test world reads");
            check(world_structure.size().width == 16 && world_structure.size().height == 16 &&
                world_structure.size().length == 16,
                "palette test world selection size");

            // visit_chunk_palettes exposes one palette + indices per subchunk
            // and agrees with the on-disk blocks at the first positions.
            std::vector<std::string> palette_states_at;
            const auto palette_visited = world_structure.visit_chunk_palettes(
                std::array{ water_structure::ChunkPos{ 0, 0 } },
                [&](water_structure::ChunkPos pos,
                    std::span<const water_structure::SubChunkPaletteData> subchunks) {
                    check(pos == water_structure::ChunkPos{ 0, 0 },
                        "palette visitor chunk position");
                    check(subchunks.size() == 1, "palette visitor subchunk count");
                    const auto& data = subchunks.front();
                    check(data.sub_y == -4, "palette visitor subchunk Y");
                    check(data.indices.size() == 4096, "palette visitor index count");
                    // Blocks were placed at internal (0,0,0), (1,0,0), (2,0,0);
                    // palette indices use native (x,y,z): index = x*256 + y*16 + z.
                    for (const auto native_index : { 0u, 256u, 512u }) {
                        const auto palette_index = data.indices[native_index];
                        check(palette_index < data.palette.size(),
                            "palette index in range");
                        palette_states_at.push_back(data.palette[palette_index].name);
                    }
                    return water_structure::Result<void>::success();
                });
            check(palette_visited.ok(), "visit_chunk_palettes succeeds");
            check(palette_states_at.size() == 3 &&
                palette_states_at[0] == "minecraft:stone" &&
                palette_states_at[1] == "minecraft:oak_log" &&
                palette_states_at[2] == "minecraft:water",
                "palette indices map to the stored block states");

            // MCFunction writer palette path vs generic path: byte-identical
            // output for the same MCWorld structure.
            const auto palette_mcfunction = std::filesystem::temp_directory_path() /
                "water_structure_cpp_palette_path.mcfunction";
            const auto generic_mcfunction = std::filesystem::temp_directory_path() /
                "water_structure_cpp_generic_path.mcfunction";
            const auto written_palette = water_structure::FormatRegistry::write(
                world_structure, water_structure::StructureId::MCFunction,
                palette_mcfunction, registry);
            check(written_palette.ok(), "MCFunction palette path succeeds");
            set_test_environment("WATER_STRUCTURE_MCFUNCTION_NO_PALETTE", "1");
            const auto written_generic = water_structure::FormatRegistry::write(
                world_structure, water_structure::StructureId::MCFunction,
                generic_mcfunction, registry);
            set_test_environment("WATER_STRUCTURE_MCFUNCTION_NO_PALETTE", "");
            check(written_generic.ok(), "MCFunction generic path succeeds");
            {
                std::ifstream palette_file(palette_mcfunction, std::ios::binary);
                std::ifstream generic_file(generic_mcfunction, std::ios::binary);
                const std::string palette_bytes{
                    std::istreambuf_iterator<char>(palette_file),
                    std::istreambuf_iterator<char>() };
                const std::string generic_bytes{
                    std::istreambuf_iterator<char>(generic_file),
                    std::istreambuf_iterator<char>() };
                // The palette path keys the cuboid merge by formatted state
                // while the generic path keys by runtime ID, so the emission
                // order can differ; the emitted command sets must be equal.
                const auto split_lines = [](const std::string& text) {
                    std::vector<std::string> lines;
                    std::string line;
                    std::istringstream stream(text);
                    while (std::getline(stream, line)) lines.push_back(line);
                    std::sort(lines.begin(), lines.end());
                    return lines;
                };
                const auto palette_lines = split_lines(palette_bytes);
                const auto generic_lines = split_lines(generic_bytes);
                check(palette_lines == generic_lines,
                    "MCFunction palette/generic output covers identical commands");
                check(palette_bytes.find("minecraft:stone") != std::string::npos &&
                    palette_bytes.find("oak_log") != std::string::npos &&
                    palette_bytes.find("water") != std::string::npos,
                    "MCFunction palette output contains block states");
            }
            std::filesystem::remove(palette_mcfunction);
            std::filesystem::remove(generic_mcfunction);
            std::filesystem::remove_all(palette_world_dir, cleanup_error);
        }

        {
            const auto stats_archive = std::filesystem::temp_directory_path() /
                "water_structure_cpp_deferred_stats.mcworld";
            std::error_code cleanup_error;
            std::filesystem::remove(stats_archive, cleanup_error);
            auto stats_world = water_structure::BedrockWorldAdapter::open(stats_archive);
            check(stats_world.ok(), "statistics archive opens");
            std::size_t statistics_calls = 0;
            water_structure::ConversionStats final_statistics;
            water_structure::ConversionCallbacks callbacks;
            callbacks.collect_statistics = true;
            callbacks.statistics = [&](const water_structure::ConversionStats& value) {
                ++statistics_calls;
                final_statistics = value;
            };
            const auto stats_written = schematic.value()->write_to_world(
                stats_world.value(), { 0, -4, 0 }, std::move(callbacks));
            check(stats_written.ok() && statistics_calls == 0,
                "world statistics wait for LevelDB close and archive packing");
            const auto stats_closed = stats_world.value().close();
            check(stats_closed.ok() && statistics_calls == 1 &&
                final_statistics.success &&
                final_statistics.target_format == water_structure::StructureId::MCWorld &&
                final_statistics.completed_chunks == final_statistics.source_chunks,
                "world statistics publish one final post-close snapshot");
            std::filesystem::remove(stats_archive, cleanup_error);
        }

        {
            const auto destructor_archive = std::filesystem::temp_directory_path() /
                "water_structure_cpp_destructor_stats.mcworld";
            std::error_code cleanup_error;
            std::filesystem::remove(destructor_archive, cleanup_error);
            std::size_t early_calls = 0;
            std::size_t late_calls = 0;
            {
                auto destructor_world = water_structure::BedrockWorldAdapter::open(
                    destructor_archive);
                check(destructor_world.ok(), "destructor statistics archive opens");
                auto callback_lifetime = std::make_shared<int>(1);
                std::weak_ptr<int> weak_lifetime = callback_lifetime;
                water_structure::ConversionCallbacks callbacks;
                callbacks.collect_statistics = true;
                callbacks.statistics = [weak_lifetime, &early_calls, &late_calls](
                    const water_structure::ConversionStats&) {
                    if (weak_lifetime.expired()) ++late_calls;
                    else ++early_calls;
                };
                const auto written = schematic.value()->write_to_world(
                    destructor_world.value(), { 0, -4, 0 }, std::move(callbacks));
                check(written.ok() && early_calls == 0,
                    "destructor statistics remain deferred before scope exit");
                // callbacks and callback_lifetime are destroyed before the
                // adapter. Its destructor must close resources without
                // invoking deferred user code with expired captures.
            }
            check(early_calls == 0 && late_calls == 0,
                "world destructor does not invoke deferred statistics callbacks");
            std::filesystem::remove(destructor_archive, cleanup_error);
        }

        {
            // A failed stream is an aborted archive transaction: existing
            // bytes remain unchanged, a new target is not created, and the
            // destructor never commits the partial LevelDB contents.
            const auto transaction_bytes = [](const std::filesystem::path& path) {
                std::ifstream input(path, std::ios::binary);
                return std::vector<std::uint8_t>{
                    std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>() };
            };
            const auto transaction_archive = std::filesystem::temp_directory_path() /
                "water_structure_cpp_failed_transaction.mcworld";
            const auto new_transaction_archive = std::filesystem::temp_directory_path() /
                "water_structure_cpp_failed_new_transaction.mcworld";
            std::error_code cleanup_error;
            std::filesystem::remove(transaction_archive, cleanup_error);
            cleanup_error.clear();
            std::filesystem::remove(new_transaction_archive, cleanup_error);
            {
                auto baseline = water_structure::BedrockWorldAdapter::open(transaction_archive);
                check(baseline.ok(), "failed transaction baseline opens");
                check(baseline.value().close().ok(), "failed transaction baseline closes");
            }
            const auto original_archive = transaction_bytes(transaction_archive);
            TestStructure failing_structure;
            failing_structure.fail_after_first_chunk = true;
            const auto partial_chunks = failing_structure.get_chunks(
                std::array{ water_structure::ChunkPos{ 0, 0 } });
            check(partial_chunks.ok(), "failed transaction partial chunk builds");
            {
                auto transaction = water_structure::BedrockWorldAdapter::open(
                    transaction_archive);
                check(transaction.ok(), "existing transaction archive opens");
                check(transaction.value().save_chunk(
                    { 0, 0 }, partial_chunks.value().at({ 0, 0 })).ok(),
                    "existing transaction writes partial chunk");
                const auto failed = failing_structure.write_to_world(
                    transaction.value(), { 0, -4, 0 }, {});
                check(!failed.ok(), "existing transaction reports stream failure");
            }
            check(transaction_bytes(transaction_archive) == original_archive,
                "failed transaction preserves existing archive bytes");
            {
                auto transaction = water_structure::BedrockWorldAdapter::open(
                    new_transaction_archive);
                check(transaction.ok(), "new transaction archive opens");
                check(transaction.value().save_chunk(
                    { 0, 0 }, partial_chunks.value().at({ 0, 0 })).ok(),
                    "new transaction writes partial chunk");
                const auto failed = failing_structure.write_to_world(
                    transaction.value(), { 0, -4, 0 }, {});
                check(!failed.ok(), "new transaction reports stream failure");
            }
            check(!std::filesystem::exists(new_transaction_archive),
                "failed transaction does not create a new archive");
            const auto temporary_prefix = transaction_archive.filename().string() +
                ".water_structure_tmp-";
            bool leaked_temporary_archive = false;
            for (const auto& entry : std::filesystem::directory_iterator(
                    transaction_archive.parent_path())) {
                if (entry.path().filename().string().starts_with(temporary_prefix)) {
                    leaked_temporary_archive = true;
                    break;
                }
            }
            check(!leaked_temporary_archive,
                "failed transaction removes temporary archive files");
            std::filesystem::remove(transaction_archive, cleanup_error);
            cleanup_error.clear();
            std::filesystem::remove(new_transaction_archive, cleanup_error);
        }

        const auto converted = structure.write_to_world(world, { 3, 2, -5 }, {});
        check(converted.ok(), "conversion succeeds");
        check(world.saved_pos == water_structure::ChunkPos{ 3, -5 }, "chunk X/Z translation");
        check(world.saved_chunk.sub_chunks.contains(2), "subchunk Y translation");
        check(world.saved_chunk.sub_chunks.at(2).layer0[0] == 7, "translated subchunk contents");
        check(world.saved_chunk.sub_chunks.at(2).layer1[0] == 1,
            "streaming conversion preserves secondary layer");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "core tests passed\n";
    return 0;
}
