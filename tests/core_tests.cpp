#include <WaterStructure/canonical.hpp>
#include <WaterStructure/coordinates.hpp>
#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>
#include <WaterStructure/types.hpp>
#include <WaterStructure/world.hpp>

#include "../src/world/archive.hpp"
#include "../src/formats/nbt_text.hpp"

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
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
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

std::array<std::uint8_t, 16> fixture_md5(std::span<const std::uint8_t> input)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) >= 0,
        "open fixture MD5 provider");
    ULONG object_size = 0;
    ULONG received = 0;
    check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0) >= 0,
        "query fixture MD5 object size");
    std::vector<std::uint8_t> object(object_size);
    check(BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0,
        "create fixture MD5 hash");
    check(BCryptHashData(hash, const_cast<PUCHAR>(input.data()),
        static_cast<ULONG>(input.size()), 0) >= 0, "hash fixture MD5 input");
    std::array<std::uint8_t, 16> result{};
    check(BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0,
        "finish fixture MD5 hash");
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
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
    const auto key = fixture_md5(key_material);
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
            nlohmann::json::array({ 1, 0, 1, 4, 0 })
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
    water_structure::Result<water_structure::NbtChunkMap> get_chunk_nbt(
        std::span<const water_structure::ChunkPos> positions) const override
    {
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
        for (const auto format_id : {
            water_structure::StructureId::Schematic,
            water_structure::StructureId::SchemV1,
            water_structure::StructureId::SchemV2,
            water_structure::StructureId::Litematic,
            water_structure::StructureId::MCStructure,
            water_structure::StructureId::BDX,
            water_structure::StructureId::AxiomBP,
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
        check(schem_v2.value()->count_non_air_blocks().value() == 1, "synthetic SchemV2 non-air count");
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
        if (const auto* fixture_output = std::getenv("WATER_STRUCTURE_BDX_FIXTURE_OUTPUT")) {
            std::filesystem::copy_file(
                bdx_path,
                fixture_output,
                std::filesystem::copy_options::overwrite_existing);
        }
        std::filesystem::remove(bdx_path);

        const auto truncated_bdx_path = write_truncated_bdx_sample();
        const auto truncated_bdx = water_structure::FormatRegistry::open(truncated_bdx_path, registry);
        check(!truncated_bdx.ok() && truncated_bdx.error().find("command #1") != std::string::npos,
            "BDX truncated command error context");
        std::filesystem::remove(truncated_bdx_path);

        const auto ibimport_path = write_ibimport_sample();
        const auto ibimport = water_structure::FormatRegistry::open(ibimport_path, registry);
        check(ibimport.ok(), "synthetic IBImport parses");
        check(ibimport.value()->count_non_air_blocks().value() == 1, "IBImport non-air count");
        const std::vector<water_structure::ChunkPos> ib_positions{{ 0, 0 }};
        const auto ib_entities = ibimport.value()->get_chunk_nbt(ib_positions);
        check(ib_entities.ok() && ib_entities.value().at({ 0, 0 }).size() == 1,
            "IBImport command block NBT");
        std::filesystem::remove(ibimport_path);

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
            const auto opened = water_structure::FormatRegistry::open(gangban_paths[index], registry);
            check(opened.ok() && opened.value()->id() == gangban_ids[index],
                "GangBan magic detection and parse");
            const auto size = opened.value()->size();
            check(size.width == (index == 4 ? 1 : 2) && size.height == 1 && size.length == 1,
                "GangBan dimensions");
            check(opened.value()->count_non_air_blocks().value() == 1,
                "GangBan non-air count");
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
        const auto converted = structure.write_to_world(world, { 3, 2, -5 }, {});
        check(converted.ok(), "conversion succeeds");
        check(world.saved_pos == water_structure::ChunkPos{ 3, -5 }, "chunk X/Z translation");
        check(world.saved_chunk.sub_chunks.contains(2), "subchunk Y translation");
        check(world.saved_chunk.sub_chunks.at(2).layer0[0] == 7, "translated subchunk contents");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "core tests passed\n";
    return 0;
}
